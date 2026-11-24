// Copyright VChuckShunA. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GaiaLibrary.generated.h"

/** What a test tells Gaia, from Blueprint or Python. Does nothing in shipping builds. */
UCLASS()
class GAIA_API UGaiaLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Names the sessions that start from now on in their logs, so a log can be matched to its test. Empty clears it. */
	UFUNCTION(BlueprintCallable, Category = "Gaia")
	static void SetSessionLabel(const FString& Label);

	/**
	 * Says Agent's logic is about to be stopped on purpose, so rules that need it running stay quiet until it runs again.
	 * Agent is the pawn or its controller.
	 */
	UFUNCTION(BlueprintCallable, Category = "Gaia")
	static void ExpectLogicStopped(AActor* Agent);
};
