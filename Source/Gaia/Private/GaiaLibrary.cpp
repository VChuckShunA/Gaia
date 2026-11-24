// Copyright VChuckShunA. All Rights Reserved.

#include "GaiaLibrary.h"

#include "GaiaSubsystem.h"

void UGaiaLibrary::SetSessionLabel(const FString& Label)
{
#if GAIA_ENABLED
	UGaiaSubsystem::SetSessionLabel(Label);
#endif
}

void UGaiaLibrary::ExpectLogicStopped(AActor* Agent)
{
#if GAIA_ENABLED
	if (Agent)
	{
		GAIA_EVENT(*Agent, Gaia::TAG_LogicStopExpected, TEXT("Its logic is stopped on purpose"));
	}
#endif
}
