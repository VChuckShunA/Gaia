// Copyright VChuckShunA. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaiaTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "GaiaSubsystem.generated.h"

#if GAIA_ENABLED
#include "Templates/PimplPtr.h"

class AAIController;

namespace Gaia
{
	struct FAgentSample;
}

GAIA_API DECLARE_LOG_CATEGORY_EXTERN(LogGaia, Log, All);
#endif

class UGaiaProbe;

/**
 * Watches every AI agent the project's probe recognises in a game world and flags behaviour that looks broken: an agent fighting an enemy it
 * can see without shooting, a reaction that keeps replaying, a StateTree that stopped, a move that goes nowhere, and so
 * on. Everything the agents do is also logged to Saved/Logs/AIDiagnostics/AIDiag_<Time>_<Map>.jsonl, one JSON object
 * a line, for reading after a session.
 *
 * Threading: the game thread only copies each agent's state into plain structs, ten times a second by default, and
 * hands them to a worker thread through a lock-free queue. The worker works out what changed, runs the rules and writes
 * the log. The problems it finds come back through a second queue and are reported on the game thread: in the output
 * log, on screen, in the visual logger, and through GetRecentAnomalies.
 *
 * While the Visual Logger is recording (the vislog console command, or its window) each agent's timeline there also
 * gets its StateTree changes and the events code records, so an alert can be read back to its cause. The Rewind
 * Debugger shows the same entries when the Visual Logger records to a trace.
 *
 * Agents are read through the UGaiaProbe named in UGaiaSettings. Code reports what sampling can't see (a reaction
 * starting, a noise heard) with GAIA_EVENT.
 *
 * Runs on the server, where the AI does. Compiled out of shipping builds, GAIA_EVENT included.
 *
 * Console:
 *   gaia.Enabled 0|1          Off or on (on by default)
 *   gaia.SampleRate <Hz>      How often every agent is captured (10)
 *   gaia.OnScreen 0|1         Problems on screen as well as in the log (on)
 *   gaia.Report               Print what's known about every agent to the log
 */
UCLASS()
class GAIA_API UGaiaSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual TStatId GetStatId() const override;

#if GAIA_ENABLED
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;

	/** The folder that holds the session logs. */
	static FString GetLogDirectory();

	/** Names the sessions that start from now on in their logs. See UGaiaLibrary. */
	static void SetSessionLabel(const FString& Label);

	/** WorldContext's world's diagnostics if they're recording, else null. GAIA_EVENT uses it. */
	static UGaiaSubsystem* GetIfRecording(const UObject& WorldContext);

	bool IsRecording() const { return Recording.IsValid(); }

	/** Records something an agent did. Source is the agent's pawn or the controller acting for it. Use GAIA_EVENT. */
	void RecordEvent(const AActor& Source, const FGameplayTag& Event, FString&& Detail);

	/** The latest problems found with Agent (a pawn), oldest first. */
	TConstArrayView<Gaia::FAnomaly> GetRecentAnomalies(const AActor& Agent) const;

	/** Prints what's known about every agent to the log, a frame or two later (gaia.Report). */
	void RequestReport();

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	bool ShouldRecord(const UWorld& World) const;
	void StartRecording(UWorld& World);
	void StopRecording();

	void CaptureFrame(UWorld& World);
	void CaptureAgent(const AAIController& Controller, const APawn& Agent, Gaia::FAgentSample& OutSample);

	/** Reports the problems the worker has found since the last call, and prints any report it has written. */
	void DrainWorker();

	/** The worker and the thread it runs on, while recording. */
	struct FRecording;
	TPimplPtr<FRecording> Recording;
	bool bCouldNotStart = false;

	/** Recorded since the last captured frame */
	TArray<Gaia::FRecordedEvent> PendingEvents;

	/** Agents captured before, so each one's label is only looked up once */
	TSet<FObjectKey> KnownAgents;

	/** Each agent's StateTree states as last put in the Visual Logger, so only changes are */
	TMap<FObjectKey, TArray<FName>> LastLoggedStates;

	TMap<FObjectKey, TArray<Gaia::FAnomaly>> RecentAnomalies;

	float TimeUntilNextSample = 0.f;
	float TimeUntilNextSnapshot = 0.f;

	// What the diagnostics cost the game thread, for the report
	int64 Captures = 0;
	int64 CapturesDropped = 0;
	int64 EventsDropped = 0;
	double CaptureSecondsTotal = 0.0;
	double CaptureSecondsMax = 0.0;
#endif // GAIA_ENABLED

	/** Reads the project's agents, while recording. Outside the #if because reflection can't see a UPROPERTY inside it. */
	UPROPERTY()
	TObjectPtr<UGaiaProbe> Probe;
};

#if GAIA_ENABLED
/**
 * Records an AI agent's event for Gaia:
 *     GAIA_EVENT(*Controller, MyGameplayTags::AI_Diag_ReactionStarted, TEXT("Facing %s"), *GetNameSafe(Target));
 * Agent is the pawn or its controller, and is evaluated twice. Event is a native gameplay tag. The message
 * is only formatted while the diagnostics are recording, and the statement compiles to nothing in shipping builds.
 */
#define GAIA_EVENT(Agent, Event, Format, ...) \
	do \
	{ \
		if (UGaiaSubsystem* GaiaSubsystem_ = UGaiaSubsystem::GetIfRecording(Agent)) \
		{ \
			GaiaSubsystem_->RecordEvent(Agent, Event, FString::Printf(Format, ##__VA_ARGS__)); \
		} \
	} \
	while (false)
#else
#define GAIA_EVENT(Agent, Event, Format, ...) do {} while (false)
#endif
