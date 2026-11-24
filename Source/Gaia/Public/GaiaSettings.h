// Copyright VChuckShunA. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "GaiaSettings.generated.h"

class UGaiaProbe;

/** Project-wide Gaia settings, in DefaultGame.ini. */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Gaia"))
class GAIA_API UGaiaSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/**
	 * The class that reads this project's agents. Soft because Gaia loads before the game module that defines it.
	 * Without one, Gaia uses UGaiaProbe, which reads what the engine knows.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Gaia")
	TSoftClassPtr<UGaiaProbe> ProbeClass;
};
