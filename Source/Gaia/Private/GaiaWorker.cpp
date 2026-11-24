// Copyright VChuckShunA. All Rights Reserved.

#include "GaiaWorker.h"

#if GAIA_ENABLED

#include "GaiaJson.h"
#include "GaiaSubsystem.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"

namespace Gaia
{
namespace
{
	/** Goes in the log's first line. Raise it whenever a record's meaning changes. */
	constexpr int32 LogSchemaVersion = 3;

	/** Captures queued beyond this (ten seconds' worth at the default rate) are dropped rather than pile up. */
	constexpr int32 MaxQueuedCaptures = 100;
	/** How often the worker wakes without new captures, to flush the file and notice a stop request */
	constexpr uint32 WakeIntervalMs = 250;
	constexpr double FlushIntervalSeconds = 1.0;
	/** Older session logs beyond this many are deleted when a new one starts */
	constexpr int32 MaxSessionLogs = 20;

	const TCHAR* LexToString(ESeverity Severity)
	{
		return Severity == ESeverity::Error ? TEXT("error") : TEXT("warning");
	}

	/**
	 * Appends one JSON object to a buffer as one line. Construct it as a temporary and chain Add calls; the object is
	 * closed when the temporary goes away at the end of the statement:
	 *     FJsonLine(Text, Time, Frame, TEXT("event")).Add(TEXT("agent"), Agent).Add(TEXT("detail"), Detail);
	 */
	class FJsonLine : public FJsonFields
	{
	public:
		FJsonLine(FString& InText, double Time, uint64 Frame, const TCHAR* Type)
			: FJsonFields(Open(InText, Time, Frame, Type), /*bHasFields=*/ true)
		{
		}

		~FJsonLine()
		{
			Text += TEXT("}\n");
		}

	private:
		static FString& Open(FString& Text, double Time, uint64 Frame, const TCHAR* Type)
		{
			Text.Appendf(TEXT("{\"t\":%.3f,\"f\":%llu,\"type\":\"%s\""), Time, Frame, Type);
			return Text;
		}
	};
}

FWorker::FWorker(FSessionInfo&& InSession)
	: Session(MoveTemp(InSession))
	, WorkEvent(EEventMode::AutoReset)
{
}

FWorker::~FWorker()
{
	FinishSession();
}

// ---------------------------------------------------------------------------------------------------------------------
// Game thread

bool FWorker::Submit(FCapture&& Capture)
{
	if (QueuedCaptures.load(std::memory_order_relaxed) >= MaxQueuedCaptures)
	{
		return false;
	}

	QueuedCaptures.fetch_add(1, std::memory_order_relaxed);
	Captures.Enqueue(MoveTemp(Capture));
	WorkEvent->Trigger();
	return true;
}

bool FWorker::PollAnomaly(FAnomaly& OutAnomaly)
{
	return Anomalies.Dequeue(OutAnomaly);
}

void FWorker::RequestReport()
{
	bReportRequested.store(true, std::memory_order_relaxed);
	WorkEvent->Trigger();
}

bool FWorker::PollReportLine(FString& OutLine)
{
	return ReportLines.Dequeue(OutLine);
}

FString FWorker::FinishSession()
{
	if (bFinished)
	{
		return FinalSummary;
	}
	bFinished = true;

	ProcessPending();

	TMap<FName, int32> ProblemsByRule;
	int32 NumAgents = 0;
	for (const TPair<FObjectKey, FAgentHistory>& Pair : Histories)
	{
		const FAgentHistory& History = Pair.Value;
		if (!History.bHasSample)
		{
			continue;
		}

		++NumAgents;
		WriteSummary(LastWorldTime, History);
		for (int32 Index = 0; Index < Session.Rules.Num(); ++Index)
		{
			if (History.RuleStates[Index].TimesRaised > 0)
			{
				ProblemsByRule.FindOrAdd(Session.Rules[Index]->GetName()) += History.RuleStates[Index].TimesRaised;
			}
		}
	}
	if (bReportedNoNavMesh)
	{
		ProblemsByRule.Add(TEXT("NoNavMesh"), 1);
	}
	WriteCoverage(LastWorldTime);
	WritePending(/*bForceFlush=*/ true);
	File.Reset();

	if (NumAgents == 0)
	{
		FinalSummary = TEXT("Gaia: no AI agents this session.");
		return FinalSummary;
	}

	ProblemsByRule.ValueSort(TGreater<int32>());
	const FString Problems = ProblemsByRule.IsEmpty()
		? FString(TEXT("none"))
		: FString::JoinBy(ProblemsByRule, TEXT(", "), [](const TPair<FName, int32>& Rule) { return FString::Printf(TEXT("%s x%d"), *Rule.Key.ToString(), Rule.Value); });
	FinalSummary = FString::Printf(TEXT("Gaia: %d agents, problems: %s. Log: %s"), NumAgents, *Problems, *Session.LogFilePath);
	return FinalSummary;
}

// ---------------------------------------------------------------------------------------------------------------------
// Worker thread

uint32 FWorker::Run()
{
	while (!bStopRequested.load(std::memory_order_relaxed))
	{
		// Woken by a new capture, a report request or Stop; otherwise every WakeIntervalMs to flush the file.
		WorkEvent->Wait(WakeIntervalMs);
		ProcessPending();
	}
	return 0;
}

void FWorker::Stop()
{
	bStopRequested.store(true, std::memory_order_relaxed);
	WorkEvent->Trigger();
}

void FWorker::Exit()
{
	FinishSession();
}

void FWorker::Tick()
{
	ProcessPending();
}

void FWorker::ProcessPending()
{
	FCapture Capture;
	while (Captures.Dequeue(Capture))
	{
		QueuedCaptures.fetch_sub(1, std::memory_order_relaxed);

		// Opened outside the timing, which is meant to show what processing a capture costs.
		if (!bTriedOpeningFile && (!Capture.Agents.IsEmpty() || !Capture.Events.IsEmpty()))
		{
			OpenLogFile();
		}

		const double StartSeconds = FPlatformTime::Seconds();
		ProcessCapture(MoveTemp(Capture));
		const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;

		++CapturesProcessed;
		TotalProcessSeconds += ElapsedSeconds;
		MaxProcessSeconds = FMath::Max(MaxProcessSeconds, ElapsedSeconds);
	}

	if (bReportRequested.exchange(false, std::memory_order_relaxed))
	{
		BuildReport();
	}
	WritePending(/*bForceFlush=*/ false);
}

void FWorker::ProcessCapture(FCapture&& Capture)
{
	const double Now = Capture.WorldTime;
	LastWorldTime = Now;
	LastFrame = Capture.Frame;

	// Events happened between the previous capture and this one, so they come first.
	for (const FRecordedEvent& Event : Capture.Events)
	{
		ProcessEvent(Event);
	}

	if (!Capture.bHasNavMesh && !bReportedNoNavMesh)
	{
		bReportedNoNavMesh = true;

		FAnomaly Anomaly;
		Anomaly.Time = Now;
		Anomaly.Frame = LastFrame;
		Anomaly.Rule = TEXT("NoNavMesh");
		Anomaly.Severity = ESeverity::Error;
		Anomaly.Message = TEXT("The world has AI but no navmesh, so no agent can move. Add a NavMeshBoundsVolume covering the play area.");
		Emit(MoveTemp(Anomaly));
	}

	TSet<FObjectKey, DefaultKeyFuncs<FObjectKey>, TInlineSetAllocator<32>> Sampled;
	for (FAgentSample& Sample : Capture.Agents)
	{
		Sampled.Add(Sample.AgentKey);
		ProcessSample(Now, Capture.bWriteSnapshot, MoveTemp(Sample));
	}

	// Agents no longer captured were destroyed or lost their AI controller.
	for (TPair<FObjectKey, FAgentHistory>& Pair : Histories)
	{
		FAgentHistory& History = Pair.Value;
		if (History.bHasSample && !History.bRemoved && !Sampled.Contains(Pair.Key))
		{
			History.bRemoved = true;
			FJsonLine(PendingText, Now, LastFrame, TEXT("removed")).Add(TEXT("agent"), History.Agent);
		}
	}
}

void FWorker::ProcessEvent(const FRecordedEvent& Event)
{
	++EventsProcessed;
	FJsonLine(PendingText, Event.Time, Event.Frame, TEXT("event"))
		.Add(TEXT("agent"), Event.Agent)
		.Add(TEXT("event"), EventName(Event.Event))
		.Add(TEXT("detail"), Event.Detail);

	FAgentHistory& History = FindOrAddHistory(Event.AgentKey, Event.Agent, Event.Time);
	++History.EventCounts.FindOrAdd(*EventName(Event.Event));
	if (Event.Event == TAG_LogicStopExpected)
	{
		History.bLogicStopExpected = true;
		History.bLogicStopSeen = false;
	}

	for (int32 Index = 0; Index < Session.Rules.Num(); ++Index)
	{
		FRuleContext Context(*this, *Session.Rules[Index], History.RuleStates[Index], History.AgentKey, History.Agent, Event.Time, History.bHasSample ? &History.Last : nullptr);
		Session.Rules[Index]->OnEvent(Event, Context);
	}
}

void FWorker::ProcessSample(double Now, bool bWriteSnapshot, FAgentSample&& Sample)
{
	++SamplesProcessed;
	FAgentHistory& History = FindOrAddHistory(Sample.AgentKey, Sample.Agent, Now);
	if (!Sample.Label.IsEmpty())
	{
		History.Label = Sample.Label;
	}
	History.Class = Sample.Class;
	if (!Sample.ActiveStates.IsEmpty() && (!History.bHasSample || Sample.ActiveStates != History.Last.ActiveStates))
	{
		++History.StatesEntered.FindOrAdd(JoinStates(Sample.ActiveStates));
	}

	// A new (or returning) agent's first line includes a full snapshot.
	const bool bNewAgent = !History.bHasSample || History.bRemoved;
	if (bNewAgent)
	{
		WriteAgent(Now, Sample);
		History.bRemoved = false;
	}
	else
	{
		WriteChanges(Now, History.Last, Sample);

		// The time since the last sample is credited to the state the agent was in.
		if (!History.Last.Activity.IsNone())
		{
			History.TimeInActivity.FindOrAdd(History.Last.Activity) += Now - History.LastSeenTime;
		}
	}

	CheckSampleRules(Now, History, Sample);

	if (bWriteSnapshot && !bNewAgent)
	{
		WriteSnapshot(Now, Sample);
	}

	History.Last = MoveTemp(Sample);
	History.bHasSample = true;
	History.LastSeenTime = Now;
}

bool FWorker::IsLogicStoppedOnPurpose(FAgentHistory& History, const FAgentSample& Sample)
{
	if (!History.bLogicStopExpected)
	{
		return false;
	}
	if (!Sample.bTreeRunning)
	{
		History.bLogicStopSeen = true;
	}
	else if (History.bLogicStopSeen)
	{
		History.bLogicStopExpected = false;
	}
	return !Sample.bTreeRunning;
}

void FWorker::CheckSampleRules(double Now, FAgentHistory& History, const FAgentSample& Sample)
{
	const bool bLogicStopped = IsLogicStoppedOnPurpose(History, Sample);
	for (int32 Index = 0; Index < Session.Rules.Num(); ++Index)
	{
		FRuleContext Context(*this, *Session.Rules[Index], History.RuleStates[Index], History.AgentKey, History.Agent, Now, History.bHasSample ? &History.Last : nullptr);
		if (bLogicStopped && Session.Rules[Index]->NeedsRunningLogic())
		{
			// Breaks any condition in progress, so what the stop caused isn't counted once the logic is back.
			Context.HeldFor(false);
		}
		else
		{
			Session.Rules[Index]->OnSample(Sample, Context);
		}
	}
}

void FWorker::Raise(const FRule& Rule, FRuleState& State, const FObjectKey& AgentKey, FName Agent, double Now, TFunctionRef<FString()> MakeMessage)
{
	if (State.LastRaised >= 0.0 && Now - State.LastRaised < Rule.GetCooldown())
	{
		return;
	}
	State.LastRaised = Now;
	++State.TimesRaised;
	if (State.ConditionSince >= 0.0 && !State.bRaisedThisHold)
	{
		State.bRaisedThisHold = true;
		State.LastHeldTime = Now;
		State.ProblemSeconds += Now - State.ConditionSince;
	}

	FAnomaly Anomaly;
	Anomaly.Time = Now;
	Anomaly.Frame = LastFrame;
	Anomaly.AgentKey = AgentKey;
	Anomaly.Agent = Agent;
	Anomaly.Rule = Rule.GetName();
	Anomaly.Severity = Rule.GetSeverity();
	Anomaly.Message = MakeMessage();
	Emit(MoveTemp(Anomaly));
}

void FWorker::Emit(FAnomaly&& Anomaly)
{
	++AnomaliesRaised;
	FJsonLine(PendingText, Anomaly.Time, Anomaly.Frame, TEXT("anomaly"))
		.Add(TEXT("agent"), Anomaly.Agent)
		.Add(TEXT("rule"), Anomaly.Rule)
		.Add(TEXT("severity"), LexToString(Anomaly.Severity))
		.Add(TEXT("message"), Anomaly.Message);
	Anomalies.Enqueue(MoveTemp(Anomaly));
}

FWorker::FAgentHistory& FWorker::FindOrAddHistory(const FObjectKey& AgentKey, FName Agent, double Now)
{
	FAgentHistory& History = Histories.FindOrAdd(AgentKey);
	if (History.Agent.IsNone())
	{
		History.AgentKey = AgentKey;
		History.Agent = Agent;
		History.FirstSeenTime = Now;
		History.RuleStates.SetNum(Session.Rules.Num());
	}
	return History;
}

// ---------------------------------------------------------------------------------------------------------------------
// Log file

void FWorker::OpenLogFile()
{
	bTriedOpeningFile = true;

	IFileManager& FileManager = IFileManager::Get();
	const FString Directory = FPaths::GetPath(Session.LogFilePath);
	FileManager.MakeDirectory(*Directory, /*Tree=*/ true);

	// Keep the newest sessions only. Names start with the time they were made, so name order is age order.
	TArray<FString> OldLogs;
	FileManager.FindFiles(OldLogs, *(Directory / TEXT("AIDiag_*.jsonl")), /*Files=*/ true, /*Directories=*/ false);
	OldLogs.Sort();
	for (int32 Index = 0; Index <= OldLogs.Num() - MaxSessionLogs; ++Index)
	{
		FileManager.Delete(*(Directory / OldLogs[Index]), /*RequireExists=*/ false, /*EvenReadOnly=*/ false, /*Quiet=*/ true);
	}

	File.Reset(FileManager.CreateFileWriter(*Session.LogFilePath, FILEWRITE_AllowRead));
	if (!File)
	{
		UE_LOG(LogGaia, Warning, TEXT("Can't write %s. Gaia still reports problems, but keeps no log."), *Session.LogFilePath);
	}

	FJsonLine(PendingText, Session.StartWorldTime, LastFrame, TEXT("session"))
		.Add(TEXT("schema"), LogSchemaVersion)
		.Add(TEXT("label"), Session.Label)
		.Add(TEXT("map"), Session.MapName)
		.Add(TEXT("netMode"), Session.NetMode)
		.Add(TEXT("started"), FDateTime::UtcNow().ToIso8601())
		.Add(TEXT("build"), ::LexToString(FApp::GetBuildConfiguration()))
		.Add(TEXT("engine"), FEngineVersion::Current().ToString());
}

void FWorker::WriteAgent(double Now, const FAgentSample& Sample)
{
	FString Assets;
	FJsonFields AssetFields(Assets);
	for (const TPair<FName, FName>& Asset : Sample.Assets)
	{
		AssetFields.Add(*Asset.Key.ToString(), Asset.Value);
	}

	FJsonLine(PendingText, Now, LastFrame, TEXT("agent"))
		.Add(TEXT("agent"), Sample.Agent)
		.Add(TEXT("label"), Sample.Label)
		.Add(TEXT("class"), Sample.Class)
		.AddRaw(TEXT("assets"), TEXT("{") + Assets + TEXT("}"))
		.Add(TEXT("sight"), Sample.bHasSight)
		.Add(TEXT("hearing"), Sample.bHasHearing)
		.Add(TEXT("damageSense"), Sample.bHasDamageSense);
	WriteSnapshot(Now, Sample);
}

void FWorker::WriteChanges(double Now, const FAgentSample& Old, const FAgentSample& New)
{
	// Only written if something changed, and then only the new values.
	TOptional<FJsonLine> Line;
	const auto OpenLine = [this, &Line, &New, Now]
	{
		if (!Line)
		{
			Line.Emplace(PendingText, Now, LastFrame, TEXT("change"));
			Line->Add(TEXT("agent"), New.Agent);
		}
	};
	const auto AddIfChanged = [&Line, &OpenLine](const TCHAR* Field, bool bChanged, const auto& Value)
	{
		if (bChanged)
		{
			OpenLine();
			Line->Add(Field, Value);
		}
	};

	AddIfChanged(TEXT("alive"), New.bAlive != Old.bAlive, New.bAlive);
	AddIfChanged(TEXT("timeDilation"), New.TimeDilation != Old.TimeDilation, static_cast<double>(New.TimeDilation));
	AddIfChanged(TEXT("activity"), New.Activity != Old.Activity, New.Activity);
	AddIfChanged(TEXT("inCombat"), New.bInCombat != Old.bInCombat, New.bInCombat);
	AddIfChanged(TEXT("target"), New.Target != Old.Target, New.Target);
	AddIfChanged(TEXT("inSight"), New.bTargetVisible != Old.bTargetVisible, New.bTargetVisible);
	AddIfChanged(TEXT("canAct"), New.bCanAct != Old.bCanAct, New.bCanAct);
	AddIfChanged(TEXT("tree"), New.TreeStatus != Old.TreeStatus, New.TreeStatus);
	AddIfChanged(TEXT("states"), New.ActiveStates != Old.ActiveStates, TConstArrayView<FName>(New.ActiveStates));
	AddIfChanged(TEXT("path"), New.PathStatus != Old.PathStatus, New.PathStatus);
	AddIfChanged(TEXT("focus"), New.FocusActor != Old.FocusActor, New.FocusActor);

	if (New.Project && Old.Project)
	{
		FString ProjectFields;
		FJsonFields Fields(ProjectFields);
		New.Project->WriteChanges(*Old.Project, Fields);
		if (!ProjectFields.IsEmpty())
		{
			OpenLine();
			Line->AddRaw(TEXT("project"), TEXT("{") + ProjectFields + TEXT("}"));
		}
	}
}

void FWorker::WriteSnapshot(double Now, const FAgentSample& Sample)
{
	FJsonLine Line(PendingText, Now, LastFrame, TEXT("snapshot"));
	Line.Add(TEXT("agent"), Sample.Agent)
		.Add(TEXT("alive"), Sample.bAlive)
		.Add(TEXT("location"), Sample.Location)
		.Add(TEXT("velocity"), Sample.Velocity)
		.Add(TEXT("timeDilation"), static_cast<double>(Sample.TimeDilation))
		.Add(TEXT("movement"), Sample.MovementMode)
		.Add(TEXT("activity"), Sample.Activity)
		.Add(TEXT("inCombat"), Sample.bInCombat)
		.Add(TEXT("target"), Sample.Target)
		.Add(TEXT("inSight"), Sample.bTargetVisible)
		.Add(TEXT("canAct"), Sample.bCanAct)
		.Add(TEXT("lastAction"), Sample.LastActionTime)
		.Add(TEXT("focus"), Sample.FocusActor)
		.Add(TEXT("tree"), Sample.TreeStatus)
		.Add(TEXT("states"), TConstArrayView<FName>(Sample.ActiveStates))
		.Add(TEXT("path"), Sample.PathStatus);
	if (Sample.bHasMoveGoal)
	{
		Line.Add(TEXT("goal"), Sample.MoveGoal);
	}
	if (Sample.Project)
	{
		FString ProjectFields;
		FJsonFields Fields(ProjectFields);
		Sample.Project->WriteSnapshot(Fields);
		Line.AddRaw(TEXT("project"), TEXT("{") + ProjectFields + TEXT("}"));
	}
}

void FWorker::WriteSummary(double Now, const FAgentHistory& History)
{
	FString TimeInActivity;
	FJsonFields TimeFields(TimeInActivity);
	for (const TPair<FName, double>& Pair : History.TimeInActivity)
	{
		TimeFields.AddRaw(*Pair.Key.ToString(), FString::Printf(TEXT("%.1f"), Pair.Value));
	}

	FString Problems;
	FString ProblemTimes;
	FString ProjectFields;
	FJsonFields ProblemFields(Problems);
	FJsonFields ProblemTimeFields(ProblemTimes);
	FJsonFields Project(ProjectFields);
	for (int32 Index = 0; Index < Session.Rules.Num(); ++Index)
	{
		if (History.RuleStates[Index].TimesRaised > 0)
		{
			ProblemFields.Add(Session.Rules[Index]->GetName(), History.RuleStates[Index].TimesRaised);
		}
		if (History.RuleStates[Index].ProblemSeconds > 0.0)
		{
			ProblemTimeFields.Add(Session.Rules[Index]->GetName(), History.RuleStates[Index].ProblemSeconds);
		}
		Session.Rules[Index]->AddToSummary(History.AgentKey, Project);
	}

	FJsonLine Line(PendingText, Now, LastFrame, TEXT("summary"));
	Line.Add(TEXT("agent"), History.Agent)
		.Add(TEXT("firstSeen"), History.FirstSeenTime)
		.Add(TEXT("lastSeen"), History.LastSeenTime)
		.AddRaw(TEXT("timeInActivity"), TEXT("{") + TimeInActivity + TEXT("}"))
		.AddRaw(TEXT("problems"), TEXT("{") + Problems + TEXT("}"))
		.AddRaw(TEXT("problemTime"), TEXT("{") + ProblemTimes + TEXT("}"));
	if (!ProjectFields.IsEmpty())
	{
		Line.AddRaw(TEXT("project"), TEXT("{") + ProjectFields + TEXT("}"));
	}
}

void FWorker::WriteCoverage(double Now)
{
	struct FClassCoverage
	{
		int32 Agents = 0;
		TMap<FName, int32> Events;
		TMap<FString, int32> States;
	};

	TMap<FName, FClassCoverage> Classes;
	for (const TPair<FObjectKey, FAgentHistory>& Pair : Histories)
	{
		const FAgentHistory& History = Pair.Value;
		if (History.bHasSample)
		{
			FClassCoverage& Coverage = Classes.FindOrAdd(History.Class);
			++Coverage.Agents;
			for (const TPair<FName, int32>& Event : History.EventCounts)
			{
				Coverage.Events.FindOrAdd(Event.Key) += Event.Value;
			}
			for (const TPair<FString, int32>& State : History.StatesEntered)
			{
				Coverage.States.FindOrAdd(State.Key) += State.Value;
			}
		}
	}

	for (const TPair<FName, FClassCoverage>& Pair : Classes)
	{
		FString Events;
		FJsonFields EventFields(Events);
		for (const TPair<FName, int32>& Event : Pair.Value.Events)
		{
			EventFields.Add(*Event.Key.ToString(), Event.Value);
		}

		FString States;
		FJsonFields StateFields(States);
		for (const TPair<FString, int32>& State : Pair.Value.States)
		{
			StateFields.Add(*State.Key, State.Value);
		}

		FJsonLine(PendingText, Now, LastFrame, TEXT("coverage"))
			.Add(TEXT("class"), Pair.Key)
			.Add(TEXT("agents"), Pair.Value.Agents)
			.AddRaw(TEXT("events"), TEXT("{") + Events + TEXT("}"))
			.AddRaw(TEXT("states"), TEXT("{") + States + TEXT("}"));
	}
}

void FWorker::WritePending(bool bForceFlush)
{
	if (File && !PendingText.IsEmpty())
	{
		const FTCHARToUTF8 Utf8(*PendingText, PendingText.Len());
		File->Serialize((UTF8CHAR*)Utf8.Get(), Utf8.Length() * sizeof(UTF8CHAR));
		BytesWritten += Utf8.Length();
	}
	// Without a file, what would have been written is dropped; problems still reach the game thread.
	PendingText.Reset();

	const double NowSeconds = FPlatformTime::Seconds();
	if (File && (bForceFlush || NowSeconds - LastFlushSeconds >= FlushIntervalSeconds))
	{
		File->Flush();
		LastFlushSeconds = NowSeconds;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Report

void FWorker::BuildReport()
{
	TArray<const FAgentHistory*> Agents;
	for (const TPair<FObjectKey, FAgentHistory>& Pair : Histories)
	{
		if (Pair.Value.bHasSample)
		{
			Agents.Add(&Pair.Value);
		}
	}
	Agents.Sort([](const FAgentHistory& A, const FAgentHistory& B) { return A.Label < B.Label; });

	ReportLines.Enqueue(FString::Printf(TEXT("Gaia report: %d agents, %lld captures, %lld events, %lld problems. Worker: %.3f ms a capture on average, %.3f ms at most. Log: %s (%.1f KB)"),
		Agents.Num(), CapturesProcessed, EventsProcessed, AnomaliesRaised,
		CapturesProcessed > 0 ? TotalProcessSeconds * 1000.0 / CapturesProcessed : 0.0, MaxProcessSeconds * 1000.0,
		File ? *Session.LogFilePath : TEXT("(none)"), BytesWritten / 1024.0));

	for (const FAgentHistory* History : Agents)
	{
		ReportLines.Enqueue(TEXT("  ") + DescribeAgent(*History));
	}
}

FString FWorker::DescribeAgent(const FAgentHistory& History) const
{
	const FAgentSample& Sample = History.Last;

	FString Status = Sample.Activity.IsNone() ? FString(TEXT("watched")) : Sample.Activity.ToString();
	if (History.bRemoved)
	{
		Status = TEXT("gone");
	}
	else if (!Sample.bAlive)
	{
		Status = TEXT("dead");
	}

	const FString Target = Sample.Target.IsNone()
		? FString(TEXT("no target"))
		: FString::Printf(TEXT("target %s%s"), *Sample.Target.ToString(), Sample.bTargetVisible ? TEXT(" (in sight)") : TEXT(""));

	FString TimeInActivity;
	for (const TPair<FName, double>& Pair : History.TimeInActivity)
	{
		TimeInActivity.Appendf(TEXT("%s%s %.0f s"), TimeInActivity.IsEmpty() ? TEXT("") : TEXT(", "), *Pair.Key.ToString(), Pair.Value);
	}

	FString Problems;
	FString Counted;
	for (int32 Index = 0; Index < Session.Rules.Num(); ++Index)
	{
		if (History.RuleStates[Index].TimesRaised > 0)
		{
			Problems.Appendf(TEXT("%s%s x%d"), Problems.IsEmpty() ? TEXT("") : TEXT(", "), Session.Rules[Index]->GetName(), History.RuleStates[Index].TimesRaised);
		}
		Session.Rules[Index]->AddToReport(History.AgentKey, Counted);
	}

	return FString::Printf(TEXT("%s: %s, %s, in %s%s%s%s. Time: %s. Problems: %s."),
		History.Label.IsEmpty() ? *History.Agent.ToString() : *History.Label, *Status, *Target, *JoinStates(Sample.ActiveStates),
		Sample.Project ? TEXT(", ") : TEXT(""), Sample.Project ? *Sample.Project->Describe() : TEXT(""), *Counted,
		TimeInActivity.IsEmpty() ? TEXT("-") : *TimeInActivity, Problems.IsEmpty() ? TEXT("none") : *Problems);
}

double FRuleContext::HeldFor(bool bCondition)
{
	if (!bCondition)
	{
		State.ConditionSince = -1.0;
		State.bRaisedThisHold = false;
		return -1.0;
	}
	if (State.ConditionSince < 0.0)
	{
		State.ConditionSince = Now;
	}
	if (State.bRaisedThisHold)
	{
		State.ProblemSeconds += Now - State.LastHeldTime;
		State.LastHeldTime = Now;
	}
	return Now - State.ConditionSince;
}

int32 FRuleContext::CountRecent(double Window)
{
	const double Time = Now;
	State.RecentTimes.Add(Time);
	State.RecentTimes.RemoveAll([Time, Window](double Earlier) { return Time - Earlier > Window; });
	return State.RecentTimes.Num();
}

void FRuleContext::Raise(TFunctionRef<FString()> MakeMessage)
{
	Worker.Raise(Rule, State, AgentKey, Agent, Now, MakeMessage);
}

} // namespace Gaia

#endif // GAIA_ENABLED
