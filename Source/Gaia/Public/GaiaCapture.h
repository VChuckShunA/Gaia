// Copyright VChuckShunA. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaiaTypes.h"

#if GAIA_ENABLED

/**
 * Plain data the game thread captures for Gaia's worker thread. Nothing here may point to a UObject: the worker only
 * ever sees copies, so it can work through them while the game carries on.
 */
namespace Gaia
{
	class FJsonFields;

	/**
	 * What a project records about an agent beyond FAgentSample. A probe derives from this and fills it in; the project's
	 * rules read it back with FAgentSample::GetProject. It's plain data, used on the worker thread once captured.
	 */
	struct FProjectSample
	{
		virtual ~FProjectSample() = default;

		/** Writes every field, for the log's snapshots. */
		virtual void WriteSnapshot(FJsonFields& Fields) const = 0;

		/** Writes the fields that differ from Old, which is always the same type as this. */
		virtual void WriteChanges(const FProjectSample& Old, FJsonFields& Fields) const = 0;

		/** A short description for the console report, such as "health 100, ammo 30". */
		virtual FString Describe() const = 0;
	};

	/** One AI agent at one moment. Names of enum values are FNames, which the worker can read and compare. */
	struct FAgentSample
	{
		/** The agent's pawn. Names can repeat across levels; keys can't. */
		FObjectKey AgentKey;
		FName Agent;
		/** Only set the first time an agent is sampled */
		FString Label;
		FName Class;
		/** Only set the first time an agent is sampled: what it runs, as a role and an asset's name */
		TArray<TPair<FName, FName>> Assets;

		// What the engine knows, read by UGaiaProbe with no setup.
		FVector Location = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		/** The pawn's own time dilation (CustomTimeDilation), on top of the world's. A game that pauses its AI sets it to 0. */
		float TimeDilation = 1.f;
		FName MovementMode;
		bool bHasTree = false;
		FName TreeStatus;
		bool bTreeRunning = false;
		/** Outermost first. Empty in builds without the gameplay debugger, which the engine needs to name them. */
		TArray<FName> ActiveStates;
		FName PathStatus;
		bool bMoving = false;
		bool bHasMoveGoal = false;
		FVector MoveGoal = FVector::ZeroVector;
		FName FocusActor;
		bool bHasSight = false;
		bool bHasHearing = false;
		bool bHasDamageSense = false;

		// What generic rules ask of the game, answered by the project's probe. The defaults raise no problems.
		bool bAlive = true;
		/** What it's doing, in the project's words. Time spent in each is reported. */
		FName Activity;
		bool bInCombat = false;
		FName Target;
		bool bTargetVisible = false;
		/** It has the means to act on its target, such as a weapon */
		bool bCanAct = false;
		/** World time of its last action on a target, negative if none */
		double LastActionTime = -1.0;

		/** The project's own data. Null with the base probe. */
		TUniquePtr<FProjectSample> Project;

		/**
		 * Its own time is stopped (TimeDilation 0), so it can't move or act: rules about doing nothing for too long don't
		 * count that time. They measure in world time, which still runs.
		 */
		bool IsTimeFrozen() const { return TimeDilation <= UE_KINDA_SMALL_NUMBER; }

		/** The project's data, as the type its probe puts there. */
		template <typename TSample>
		const TSample& GetProject() const
		{
			check(Project.IsValid());
			return static_cast<const TSample&>(*Project);
		}
	};

	/** Every agent, captured on one game frame. */
	struct FCapture
	{
		double WorldTime = 0.0;
		uint64 Frame = 0;
		bool bHasNavMesh = true;
		/** Write every agent's full state this time, not just what changed */
		bool bWriteSnapshot = false;
		TArray<FAgentSample> Agents;
		/** Recorded since the previous capture, oldest first */
		TArray<FRecordedEvent> Events;
	};
}

#endif // GAIA_ENABLED
