// Copyright VChuckShunA. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Gaia is development tooling, compiled out of shipping builds. Define as 0 or 1 in a target to override. */
#ifndef GAIA_ENABLED
#define GAIA_ENABLED (!UE_BUILD_SHIPPING)
#endif

#if GAIA_ENABLED

#include "GameplayTagContainer.h"
#include "NativeGameplayTags.h"
#include "UObject/ObjectKey.h"

/** What the game reports to Gaia, and what Gaia reports back. */
namespace Gaia
{
	/** UGaiaLibrary::ExpectLogicStopped was called for the agent. */
	GAIA_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_LogicStopExpected);

	/** A tag's last part, which is how the log and the Visual Logger name an event. */
	GAIA_API FString EventName(const FGameplayTag& Tag);

	/** Something an agent did that sampling its state could miss, recorded where it happens with GAIA_EVENT. */
	struct FRecordedEvent
	{
		double Time = 0.0;
		uint64 Frame = 0;
		/** The agent's pawn. A key, never resolved off the game thread. */
		FObjectKey AgentKey;
		FName Agent;
		/** A native tag owned by the project. The log shows its last part. */
		FGameplayTag Event;
		FString Detail;
	};

	enum class ESeverity : uint8
	{
		/** Probably wrong: worth a look */
		Warning,
		/** Certainly wrong: the agent can't behave as intended */
		Error
	};

	/** A problem Gaia found with an agent, or with the world when AgentKey is null. */
	struct FAnomaly
	{
		double Time = 0.0;
		uint64 Frame = 0;
		FObjectKey AgentKey;
		FName Agent;
		FName Rule;
		ESeverity Severity = ESeverity::Warning;
		FString Message;
	};
}

#endif // GAIA_ENABLED
