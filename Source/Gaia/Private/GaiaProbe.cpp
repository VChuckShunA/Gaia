// Copyright VChuckShunA. All Rights Reserved.

#include "GaiaProbe.h"

#if GAIA_ENABLED

#include "AIController.h"
#include "Components/StateTreeComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISense_Damage.h"
#include "Perception/AISense_Hearing.h"
#include "Perception/AISense_Sight.h"

void UGaiaProbe::BeginSession()
{
	TreeStatusNames = GetValueNames<EStateTreeRunStatus>();
	PathStatusNames = GetValueNames<EPathFollowingStatus::Type>();
	MovementModeNames = GetValueNames<EMovementMode>();
}

void UGaiaProbe::CaptureAgent(const AAIController& Controller, const APawn& Pawn, Gaia::FAgentSample& OutSample) const
{
	OutSample.Location = Pawn.GetActorLocation();
	OutSample.Velocity = Pawn.GetVelocity();
	OutSample.TimeDilation = Pawn.CustomTimeDilation;
	if (const UCharacterMovementComponent* Movement = Cast<UCharacterMovementComponent>(Pawn.GetMovementComponent()))
	{
		OutSample.MovementMode = NameOf(MovementModeNames, Movement->MovementMode);
	}
	OutSample.FocusActor = GetFNameSafe(Controller.GetFocusActor());

	if (const UStateTreeComponent* StateTree = Controller.FindComponentByClass<UStateTreeComponent>())
	{
		const EStateTreeRunStatus Status = StateTree->GetStateTreeRunStatus();
		OutSample.bHasTree = true;
		OutSample.TreeStatus = NameOf(TreeStatusNames, static_cast<uint8>(Status));
		OutSample.bTreeRunning = Status == EStateTreeRunStatus::Running;
#if WITH_GAMEPLAY_DEBUGGER
		OutSample.ActiveStates = StateTree->GetActiveStateNames();
#endif
	}

	if (const UPathFollowingComponent* PathFollowing = Controller.GetPathFollowingComponent())
	{
		const EPathFollowingStatus::Type Status = PathFollowing->GetStatus();
		OutSample.PathStatus = NameOf(PathStatusNames, static_cast<uint8>(Status));
		OutSample.bMoving = Status == EPathFollowingStatus::Moving;
		const FNavPathSharedPtr Path = PathFollowing->GetPath();
		OutSample.bHasMoveGoal = Path.IsValid() && Path->IsValid();
		if (OutSample.bHasMoveGoal)
		{
			OutSample.MoveGoal = Path->GetDestinationLocation();
		}
	}

	if (const UAIPerceptionComponent* Perception = Controller.GetAIPerceptionComponent())
	{
		OutSample.bHasSight = Perception->GetSenseConfig(UAISense::GetSenseID<UAISense_Sight>()) != nullptr;
		OutSample.bHasHearing = Perception->GetSenseConfig(UAISense::GetSenseID<UAISense_Hearing>()) != nullptr;
		OutSample.bHasDamageSense = Perception->GetSenseConfig(UAISense::GetSenseID<UAISense_Damage>()) != nullptr;
	}
}

void UGaiaProbe::CreateRules(TArray<TUniquePtr<Gaia::FRule>>& OutRules) const
{
	Gaia::AddCoreRules(OutRules);
	OutRules.Add(MakeUnique<Gaia::FNotEngagingRule>());
}

#endif // GAIA_ENABLED
