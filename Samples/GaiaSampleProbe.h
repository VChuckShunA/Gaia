// Copyright VChuckShunA. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaiaProbe.h"
#include "GaiaSampleProbe.generated.h"

/**
 * A sample probe. It reads what the engine knows about every character (the base class does that), adds whether the
 * character is in the air, and checks for one that's been up there too long.
 *
 * To use it, copy this file and GaiaSampleProbe.cpp into your game module, rename the class, and name it in
 * DefaultGame.ini under [/Script/Gaia.GaiaSettings] as ProbeClass. See the plugin's README.
 */
UCLASS()
class UGaiaSampleProbe : public UGaiaProbe
{
	GENERATED_BODY()

#if GAIA_ENABLED
public:
	virtual bool IsAgent(const AAIController& Controller, const APawn& Pawn) const override;
	virtual void CaptureAgent(const AAIController& Controller, const APawn& Pawn, Gaia::FAgentSample& OutSample) const override;
	virtual void CreateRules(TArray<TUniquePtr<Gaia::FRule>>& OutRules) const override;
#endif
};
