// Copyright VChuckShunA. All Rights Reserved.

#include "GaiaSubsystem.h"

#if GAIA_ENABLED

#include "AIController.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GaiaProbe.h"
#include "GaiaSettings.h"
#include "GaiaWorker.h"
#include "HAL/IConsoleManager.h"
#include "HAL/RunnableThread.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "NavigationSystem.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "VisualLogger/VisualLogger.h"

DEFINE_LOG_CATEGORY(LogGaia);

namespace Gaia
{
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(TAG_LogicStopExpected, "Gaia.LogicStopExpected", "A test stopped an agent's logic on purpose.");

	FString EventName(const FGameplayTag& Tag)
	{
		const FString Name = Tag.ToString();
		int32 LastDot = INDEX_NONE;
		return Name.FindLastChar(TEXT('.'), LastDot) ? Name.RightChop(LastDot + 1) : Name;
	}

namespace
{
	/** What sessions from now on are called, set by a test. */
	FString NextSessionLabel;

	TAutoConsoleVariable<bool> CVarDiagnostics(
		TEXT("gaia.Enabled"),
		true,
		TEXT("Watch AI agents for broken behaviour on a background thread, logging what they do to Saved/Logs/AIDiagnostics."));

	TAutoConsoleVariable<float> CVarSampleRate(
		TEXT("gaia.SampleRate"),
		10.f,
		TEXT("How many times a second Gaia captures every agent (1 to 60)."));

	/** Seconds between full snapshots of every agent in the log. Between them, only changes are logged. */
	constexpr float SnapshotInterval = 2.f;

	TAutoConsoleVariable<bool> CVarOnScreen(
		TEXT("gaia.OnScreen"),
		true,
		TEXT("Show the problems Gaia finds on screen, as well as in the log."));

	void RequestReportInEveryWorld()
	{
		bool bAnyRecording = false;
		if (GEngine)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				const UWorld* World = Context.World();
				UGaiaSubsystem* Diagnostics = World ? World->GetSubsystem<UGaiaSubsystem>() : nullptr;
				if (Diagnostics && Diagnostics->IsRecording())
				{
					Diagnostics->RequestReport();
					bAnyRecording = true;
				}
			}
		}
		if (!bAnyRecording)
		{
			UE_LOG(LogGaia, Display, TEXT("Gaia isn't recording. It runs in game worlds on the server, while gaia.Enabled is 1."));
		}
	}

	FAutoConsoleCommand ReportCommand(
		TEXT("gaia.Report"),
		TEXT("Print what Gaia knows about every agent to the log."),
		FConsoleCommandDelegate::CreateStatic(&RequestReportInEveryWorld));

	/** Problems kept per agent for the gameplay debugger */
	constexpr int32 MaxRecentAnomalies = 5;
	/** Events recorded between two captures beyond this are dropped: something is recording one every frame */
	constexpr int32 MaxPendingEvents = 1000;
	constexpr float OnScreenSeconds = 10.f;
	constexpr uint32 WorkerStackSize = 128 * 1024;
}
}

/** A recording in progress: the worker, and the thread it runs on. */
struct UGaiaSubsystem::FRecording
{
	explicit FRecording(Gaia::FSessionInfo&& Session)
		: Worker(MoveTemp(Session))
	{
	}

	Gaia::FWorker Worker;
	/** Declared after the worker, so it's destroyed (stopping the thread) first. */
	TUniquePtr<FRunnableThread> Thread;
};

#endif // GAIA_ENABLED

bool UGaiaSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
#if GAIA_ENABLED
	return Super::ShouldCreateSubsystem(Outer);
#else
	return false;
#endif
}

TStatId UGaiaSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGaiaSubsystem, STATGROUP_Tickables);
}

#if GAIA_ENABLED

void UGaiaSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	// Before any actor begins play, so nothing an agent does at the start goes unrecorded.
	if (ShouldRecord(InWorld))
	{
		StartRecording(InWorld);
	}
}

void UGaiaSubsystem::Deinitialize()
{
	StopRecording();
	Super::Deinitialize();
}

void UGaiaSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	UWorld* World = GetWorld();
	if (!World || !World->HasBegunPlay())
	{
		return;
	}

	// Follow the console variable, and the world turning into a client by joining a game.
	const bool bShouldRecord = ShouldRecord(*World);
	if (bShouldRecord && !IsRecording())
	{
		StartRecording(*World);
	}
	else if (!bShouldRecord && IsRecording())
	{
		StopRecording();
	}
	if (!IsRecording())
	{
		return;
	}

	TimeUntilNextSnapshot -= DeltaTime;
	TimeUntilNextSample -= DeltaTime;
	if (TimeUntilNextSample <= 0.f)
	{
		const float SampleInterval = 1.f / FMath::Clamp(Gaia::CVarSampleRate.GetValueOnGameThread(), 1.f, 60.f);
		// At most one capture a frame: a long frame mustn't cause a burst of them.
		TimeUntilNextSample = FMath::Max(TimeUntilNextSample + SampleInterval, 0.f);
		CaptureFrame(*World);
	}

	DrainWorker();
}

FString UGaiaSubsystem::GetLogDirectory()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectLogDir() / TEXT("AIDiagnostics"));
}

void UGaiaSubsystem::SetSessionLabel(const FString& Label)
{
	check(IsInGameThread());
	Gaia::NextSessionLabel = Label;
}

UGaiaSubsystem* UGaiaSubsystem::GetIfRecording(const UObject& WorldContext)
{
	const UWorld* World = WorldContext.GetWorld();
	UGaiaSubsystem* Diagnostics = World ? World->GetSubsystem<UGaiaSubsystem>() : nullptr;
	return Diagnostics && Diagnostics->IsRecording() ? Diagnostics : nullptr;
}

void UGaiaSubsystem::RecordEvent(const AActor& Source, const FGameplayTag& Event, FString&& Detail)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Gaia events must be recorded on the game thread.")))
	{
		return;
	}
	if (PendingEvents.Num() >= Gaia::MaxPendingEvents)
	{
		++EventsDropped;
		return;
	}

	// Agents are known by their pawns; controllers act for them.
	const AActor* Agent = &Source;
	if (const AController* Controller = Cast<AController>(&Source))
	{
		if (const APawn* Pawn = Controller->GetPawn())
		{
			Agent = Pawn;
		}
	}

	// On the agent's Visual Logger timeline, next to its alerts and state changes, for reading back from an alert.
	UE_VLOG(Agent, LogGaia, Log, TEXT("%s: %s"), *Gaia::EventName(Event), *Detail);

	Gaia::FRecordedEvent& Recorded = PendingEvents.AddDefaulted_GetRef();
	Recorded.Time = GetWorld()->GetTimeSeconds();
	Recorded.Frame = GFrameCounter;
	Recorded.AgentKey = FObjectKey(Agent);
	Recorded.Agent = Agent->GetFName();
	Recorded.Event = Event;
	Recorded.Detail = MoveTemp(Detail);
}

TConstArrayView<Gaia::FAnomaly> UGaiaSubsystem::GetRecentAnomalies(const AActor& Agent) const
{
	const TArray<Gaia::FAnomaly>* Recent = RecentAnomalies.Find(FObjectKey(&Agent));
	return Recent ? TConstArrayView<Gaia::FAnomaly>(*Recent) : TConstArrayView<Gaia::FAnomaly>();
}

void UGaiaSubsystem::RequestReport()
{
	if (!Recording)
	{
		return;
	}

	UE_LOG(LogGaia, Display, TEXT("Gaia in %s. Game thread: %.3f ms a capture on average, %.3f ms at most, over %lld captures; %lld captures and %lld events dropped."),
		*UWorld::RemovePIEPrefix(GetWorld()->GetMapName()), Captures > 0 ? CaptureSecondsTotal * 1000.0 / Captures : 0.0, CaptureSecondsMax * 1000.0,
		Captures, CapturesDropped, EventsDropped);
	Recording->Worker.RequestReport();
}

bool UGaiaSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

bool UGaiaSubsystem::ShouldRecord(const UWorld& World) const
{
	// AI runs on the server; clients have nothing to watch.
	return !bCouldNotStart && Gaia::CVarDiagnostics.GetValueOnGameThread() && World.GetNetMode() != NM_Client;
}

void UGaiaSubsystem::StartRecording(UWorld& World)
{
	using namespace Gaia;

	const TSoftClassPtr<UGaiaProbe>& ProbeSetting = GetDefault<UGaiaSettings>()->ProbeClass;
	TSubclassOf<UGaiaProbe> ProbeClass = ProbeSetting.LoadSynchronous();
	if (!ProbeClass)
	{
		if (!ProbeSetting.IsNull())
		{
			UE_LOG(LogGaia, Warning, TEXT("Can't load Probe Class %s from the Gaia settings, so Gaia only sees what the engine knows."), *ProbeSetting.ToString());
		}
		ProbeClass = UGaiaProbe::StaticClass();
	}
	Probe = NewObject<UGaiaProbe>(this, ProbeClass);
	Probe->BeginSession();

	int32 PIEInstance = INDEX_NONE;
	FSessionInfo Session;
	Session.MapName = UWorld::RemovePIEPrefix(World.GetMapName(), &PIEInstance);
	Session.NetMode = ToString(World.GetNetMode());
	Session.Label = NextSessionLabel;
	Session.StartWorldTime = World.GetTimeSeconds();
	Probe->CreateRules(Session.Rules);

	// Named for the time first, so the logs sort by age.
	FString FileName = FString::Printf(TEXT("AIDiag_%s_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")), *Session.MapName);
	if (PIEInstance != INDEX_NONE)
	{
		FileName.Appendf(TEXT("_PIE%d"), PIEInstance);
	}
	FileName += TEXT(".jsonl");
	Session.LogFilePath = GetLogDirectory() / FileName;
	const FString MapName = Session.MapName;

	Recording = MakePimpl<FRecording>(MoveTemp(Session));
	Recording->Thread.Reset(FRunnableThread::Create(&Recording->Worker, TEXT("Gaia"), WorkerStackSize, TPri_BelowNormal));
	if (!Recording->Thread)
	{
		UE_LOG(LogGaia, Warning, TEXT("Couldn't start Gaia's thread. Gaia is off in %s."), *MapName);
		Recording.Reset();
		Probe = nullptr;
		bCouldNotStart = true;
		return;
	}

	TimeUntilNextSample = 0.f;
	TimeUntilNextSnapshot = 0.f;
	Captures = 0;
	CapturesDropped = 0;
	EventsDropped = 0;
	CaptureSecondsTotal = 0.0;
	CaptureSecondsMax = 0.0;
	UE_LOG(LogGaia, Log, TEXT("Gaia watching %s. Log: %s"), *MapName, *Recording->Worker.GetLogFilePath());
}

void UGaiaSubsystem::StopRecording()
{
	if (!Recording)
	{
		return;
	}

	// Stops the worker and waits for it to finish what's queued.
	Recording->Thread->Kill(/*bShouldWait=*/ true);
	Recording->Thread.Reset();

	// With the thread gone, the worker is the game thread's alone.
	const FString Summary = Recording->Worker.FinishSession();
	DrainWorker();
	UE_LOG(LogGaia, Log, TEXT("%s"), *Summary);

	Recording.Reset();
	Probe = nullptr;
	PendingEvents.Reset();
	KnownAgents.Reset();
	LastLoggedStates.Reset();
}

void UGaiaSubsystem::CaptureFrame(UWorld& World)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UGaiaSubsystem::CaptureFrame);
	using namespace Gaia;

	const uint64 StartCycles = FPlatformTime::Cycles64();

	FCapture Capture;
	Capture.WorldTime = World.GetTimeSeconds();
	Capture.Frame = GFrameCounter;
	Capture.bWriteSnapshot = TimeUntilNextSnapshot <= 0.f;
	if (Capture.bWriteSnapshot)
	{
		TimeUntilNextSnapshot = SnapshotInterval;
	}

	for (TActorIterator<AAIController> It(&World); It; ++It)
	{
		// The controller's pawn, not its agent: an agent is only an agent while possessed.
		const APawn* Agent = It->GetPawn();
		if (Agent && Probe->IsAgent(**It, *Agent))
		{
			CaptureAgent(**It, *Agent, Capture.Agents.AddDefaulted_GetRef());
		}
	}

	// A missing navmesh only matters where there's AI to move on it.
	if (!Capture.Agents.IsEmpty())
	{
		UNavigationSystemV1* NavigationSystem = FNavigationSystem::GetCurrent<UNavigationSystemV1>(&World);
		Capture.bHasNavMesh = NavigationSystem && NavigationSystem->GetDefaultNavDataInstance(FNavigationSystem::DontCreate);
	}

	Capture.Events = MoveTemp(PendingEvents);
	PendingEvents.Reset();

	if (!Recording->Worker.Submit(MoveTemp(Capture)))
	{
		++CapturesDropped;
	}

	const double Seconds = FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - StartCycles);
	++Captures;
	CaptureSecondsTotal += Seconds;
	CaptureSecondsMax = FMath::Max(CaptureSecondsMax, Seconds);
}

void UGaiaSubsystem::CaptureAgent(const AAIController& Controller, const APawn& Agent, Gaia::FAgentSample& OutSample)
{
	OutSample.AgentKey = FObjectKey(&Agent);
	OutSample.Agent = Agent.GetFName();
	if (!KnownAgents.Contains(OutSample.AgentKey))
	{
		KnownAgents.Add(OutSample.AgentKey);
		OutSample.Label = Agent.GetActorNameOrLabel();
		Probe->CaptureAssets(Controller, Agent, OutSample.Assets);
	}
	OutSample.Class = Agent.GetClass()->GetFName();

	Probe->CaptureAgent(Controller, Agent, OutSample);

	// Where its StateTree went, on the same timeline. Only worked out while someone is recording.
	if (FVisualLogger::IsRecording() && !OutSample.ActiveStates.IsEmpty())
	{
		TArray<FName>& LastStates = LastLoggedStates.FindOrAdd(OutSample.AgentKey);
		if (OutSample.ActiveStates != LastStates)
		{
			UE_VLOG(&Agent, LogGaia, Log, TEXT("State: %s"), *Gaia::JoinStates(OutSample.ActiveStates));
			LastStates = OutSample.ActiveStates;
		}
	}
}

void UGaiaSubsystem::DrainWorker()
{
	using namespace Gaia;

	FWorker& Worker = Recording->Worker;
	FAnomaly Anomaly;
	while (Worker.PollAnomaly(Anomaly))
	{
		const AActor* Agent = Cast<AActor>(Anomaly.AgentKey.ResolveObjectPtr());
		const FString AgentName = Agent ? Agent->GetActorNameOrLabel() : Anomaly.Agent.IsNone() ? FString(TEXT("World")) : Anomaly.Agent.ToString();
		// The time and frame are the way back to the moment: the log file, the Visual Logger and Gaia's tools all use them.
		const FString Text = FString::Printf(TEXT("%s [%s] %s (t=%.1f s, frame %llu)"), *AgentName, *Anomaly.Rule.ToString(), *Anomaly.Message, Anomaly.Time, Anomaly.Frame);

		if (Anomaly.Severity == ESeverity::Error)
		{
			UE_LOG(LogGaia, Error, TEXT("%s"), *Text);
		}
		else
		{
			UE_LOG(LogGaia, Warning, TEXT("%s"), *Text);
		}
		if (Agent)
		{
			// Found a moment after it happened, so it says when.
			UE_VLOG(Agent, LogGaia, Warning, TEXT("[%s] %s (t=%.1f s)"), *Anomaly.Rule.ToString(), *Anomaly.Message, Anomaly.Time);
		}
		if (GEngine && CVarOnScreen.GetValueOnGameThread())
		{
			GEngine->AddOnScreenDebugMessage(INDEX_NONE, OnScreenSeconds, Anomaly.Severity == ESeverity::Error ? FColor::Red : FColor::Orange, TEXT("AI: ") + Text);
		}

		TArray<FAnomaly>& Recent = RecentAnomalies.FindOrAdd(Anomaly.AgentKey);
		if (Recent.Num() >= MaxRecentAnomalies)
		{
			Recent.RemoveAt(0);
		}
		Recent.Add(MoveTemp(Anomaly));
	}

	FString ReportLine;
	while (Worker.PollReportLine(ReportLine))
	{
		UE_LOG(LogGaia, Display, TEXT("%s"), *ReportLine);
	}
}

#endif // GAIA_ENABLED
