// Copyright VChuckShunA. All Rights Reserved.

#include "GaiaSampleProbe.h"

#if GAIA_ENABLED

#include "GaiaJson.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
	/**
	 * What this sample records about a character beyond what Gaia knows of every agent. Plain data only: it's copied to
	 * Gaia's worker thread, so it holds no pointers to objects.
	 */
	struct FSampleCharacterData final : public Gaia::FProjectSample
	{
		bool bFalling = false;
		float Speed = 0.f;

		/** Every field, for the log's snapshots (every two seconds). */
		virtual void WriteSnapshot(Gaia::FJsonFields& Fields) const override
		{
			Fields.Add(TEXT("falling"), bFalling).Add(TEXT("speed"), static_cast<double>(Speed));
		}

		/** Only the fields that changed, for the log's change records. Old is always this type. */
		virtual void WriteChanges(const Gaia::FProjectSample& Old, Gaia::FJsonFields& Fields) const override
		{
			if (bFalling != static_cast<const FSampleCharacterData&>(Old).bFalling)
			{
				Fields.Add(TEXT("falling"), bFalling);
			}
		}

		/** A few words for the gaia.Report console command. */
		virtual FString Describe() const override
		{
			return bFalling ? TEXT("airborne") : TEXT("grounded");
		}
	};

	/**
	 * A character that has been in the air too long: it fell through the floor, or there's no ground below it. A rule
	 * looks at each sample as it arrives, on Gaia's worker thread, and only sees plain data.
	 */
	class FAirborneTooLongRule final : public Gaia::FRule
	{
	public:
		FAirborneTooLongRule()
			: FRule(TEXT("AirborneTooLong"), Gaia::ESeverity::Warning)
		{
		}

		virtual void OnSample(const Gaia::FAgentSample& Sample, Gaia::FRuleContext& Context) override
		{
			constexpr double AirborneSeconds = 5.0;

			// HeldFor times how long the condition has held without a break, and is negative while it doesn't.
			const double AirborneFor = Context.HeldFor(Sample.bAlive && Sample.GetProject<FSampleCharacterData>().bFalling);
			if (AirborneFor >= AirborneSeconds)
			{
				// The message is only built if the rule reports: its cooldown allows one report every ten seconds.
				Context.Raise([AirborneFor]
				{
					return FString::Printf(TEXT("In the air for %.1f s. Did it fall through the floor, or is there no ground below it?"), AirborneFor);
				});
			}
		}
	};
}

bool UGaiaSampleProbe::IsAgent(const AAIController& Controller, const APawn& Pawn) const
{
	// Only characters; Gaia watches every AI-controlled pawn unless a probe says otherwise.
	return Pawn.IsA<ACharacter>();
}

void UGaiaSampleProbe::CaptureAgent(const AAIController& Controller, const APawn& Pawn, Gaia::FAgentSample& OutSample) const
{
	// Location, velocity, movement mode, StateTree status and states, path and senses: all read for you.
	Super::CaptureAgent(Controller, Pawn, OutSample);

	const ACharacter& Character = *CastChecked<ACharacter>(&Pawn);
	TUniquePtr<FSampleCharacterData> Data = MakeUnique<FSampleCharacterData>();
	Data->bFalling = Character.GetCharacterMovement()->IsFalling();
	Data->Speed = Character.GetVelocity().Size();

	// What the generic rules ask of your game. Where your game has a notion of dying, fighting or a target, say so here.
	OutSample.bAlive = true;
	// What it's doing, in your words. The log summary says how long each agent spent in each.
	OutSample.Activity = Data->bFalling ? FName(TEXT("Airborne")) : FName(TEXT("Grounded"));
	OutSample.Project = MoveTemp(Data);
}

void UGaiaSampleProbe::CreateRules(TArray<TUniquePtr<Gaia::FRule>>& OutRules) const
{
	// Gaia's own rules (stuck, heading for the origin, StateTree stopped or thrashing, and so on), then yours.
	Super::CreateRules(OutRules);
	OutRules.Add(MakeUnique<FAirborneTooLongRule>());
}

#endif // GAIA_ENABLED
