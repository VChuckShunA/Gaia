// Copyright VChuckShunA. All Rights Reserved.

#include "GaiaRule.h"

#if GAIA_ENABLED

namespace Gaia
{
namespace
{
	/** Following a path without getting anywhere. */
	class FStuckRule final : public FRule
	{
	public:
		FStuckRule() : FRule(TEXT("Stuck"), ESeverity::Warning) {}

		virtual void OnSample(const FAgentSample& Sample, FRuleContext& Context) override
		{
			constexpr double StuckSeconds = 3.0;
			constexpr double StuckSpeed = 10.0;

			const double StuckFor = Context.HeldFor(Sample.bAlive && !Sample.IsTimeFrozen() && Sample.bMoving && Sample.Velocity.Size2D() < StuckSpeed);
			if (StuckFor >= StuckSeconds)
			{
				Context.Raise([&Sample, StuckFor]
				{
					return FString::Printf(TEXT("Has been following a path for %.1f s without moving, %.0f m from its goal %s. Blocked, or is the goal off the navmesh?"),
						StuckFor, FVector::Dist(Sample.Location, Sample.MoveGoal) / 100.0, *Sample.MoveGoal.ToCompactString());
				});
			}
		}
	};

	/** Heading for the world origin: nearly always a location that was never set. */
	class FMovingToOriginRule final : public FRule
	{
	public:
		FMovingToOriginRule() : FRule(TEXT("MovingToOrigin"), ESeverity::Warning) {}

		virtual void OnSample(const FAgentSample& Sample, FRuleContext& Context) override
		{
			// A move goal this close to the origin (horizontally) is suspect, unless the agent is near it too.
			constexpr double OriginGoalRadius = 50.0;
			constexpr double OriginAgentRadius = 500.0;

			const bool bToOrigin = Sample.bAlive && Sample.bMoving && Sample.bHasMoveGoal
				&& Sample.MoveGoal.Size2D() < OriginGoalRadius && Sample.Location.Size2D() > OriginAgentRadius;
			if (Context.HeldFor(bToOrigin) >= 0.0)
			{
				Context.Raise([&Sample]
				{
					return FString::Printf(TEXT("Moving to the world origin, which is usually a location that was never set. Activity %s, running %s."),
						*Sample.Activity.ToString(), *JoinStates(Sample.ActiveStates));
				});
			}
		}
	};

	/** Alive, but its StateTree isn't running, so it does nothing at all. */
	class FTreeNotRunningRule final : public FRule
	{
	public:
		FTreeNotRunningRule() : FRule(TEXT("TreeNotRunning"), ESeverity::Error) {}

		virtual void OnSample(const FAgentSample& Sample, FRuleContext& Context) override
		{
			constexpr double StoppedSeconds = 1.0;

			if (Context.HeldFor(Sample.bAlive && Sample.bHasTree && !Sample.bTreeRunning) >= StoppedSeconds)
			{
				Context.Raise([&Sample]
				{
					return FString::Printf(TEXT("Its StateTree is %s, not running, so it does nothing. Does the tree compile, and does its context match this controller and pawn?"),
						*Sample.TreeStatus.ToString());
				});
			}
		}
	};

	/** The StateTree flipping between states. */
	class FStateThrashRule final : public FRule
	{
	public:
		FStateThrashRule() : FRule(TEXT("StateThrash"), ESeverity::Warning) {}

		virtual void OnSample(const FAgentSample& Sample, FRuleContext& Context) override
		{
			constexpr double Window = 4.0;
			constexpr int32 Changes = 12;

			if (Context.Previous && Sample.ActiveStates != Context.Previous->ActiveStates)
			{
				const int32 Recent = Context.CountRecent(Window);
				if (Recent >= Changes)
				{
					Context.Raise([&Sample, Recent]
					{
						return FString::Printf(TEXT("Its StateTree changed state %d times in %.0f s (now %s): its transitions are fighting each other."),
							Recent, Window, *JoinStates(Sample.ActiveStates));
					});
				}
			}
		}
	};

	/** Dead, but still thinking or walking. */
	class FDeadButActiveRule final : public FRule
	{
	public:
		FDeadButActiveRule() : FRule(TEXT("DeadButActive"), ESeverity::Error) {}

		virtual bool NeedsRunningLogic() const override { return false; }

		virtual void OnSample(const FAgentSample& Sample, FRuleContext& Context) override
		{
			constexpr double ActiveSeconds = 1.5;

			const double ActiveFor = Context.HeldFor(!Sample.bAlive && (Sample.bTreeRunning || Sample.bMoving));
			if (ActiveFor >= ActiveSeconds)
			{
				Context.Raise([&Sample, ActiveFor]
				{
					return FString::Printf(TEXT("Dead for %.1f s, but its %s."), ActiveFor,
						Sample.bTreeRunning ? TEXT("StateTree is still running") : TEXT("path following is still moving it"));
				});
			}
		}
	};

	/** In combat with nothing to fight. */
	class FAttackWithoutTargetRule final : public FRule
	{
	public:
		FAttackWithoutTargetRule() : FRule(TEXT("AttackWithoutTarget"), ESeverity::Warning) {}

		virtual void OnSample(const FAgentSample& Sample, FRuleContext& Context) override
		{
			constexpr double NoTargetSeconds = 4.0;

			const double NoTargetFor = Context.HeldFor(Sample.bAlive && !Sample.IsTimeFrozen() && Sample.bInCombat && Sample.Target.IsNone());
			if (NoTargetFor >= NoTargetSeconds)
			{
				Context.Raise([NoTargetFor]
				{
					return FString::Printf(TEXT("In combat for %.1f s without a target, so it has nothing to act on."), NoTargetFor);
				});
			}
		}
	};
}

FNotEngagingRule::FNotEngagingRule()
	: FRule(TEXT("NotEngaging"), ESeverity::Warning)
{
}

void FNotEngagingRule::OnSample(const FAgentSample& Sample, FRuleContext& Context)
{
	constexpr double QuietSeconds = 5.0;

	// Acting breaks the condition, so its length is how long the agent has been quiet.
	const bool bCanEngage = Sample.bAlive && !Sample.IsTimeFrozen() && Sample.bInCombat && !Sample.Target.IsNone() && Sample.bTargetVisible && Sample.bCanAct;
	const bool bActed = Context.Previous && Sample.LastActionTime > Context.Previous->LastActionTime;
	const double QuietFor = Context.HeldFor(bCanEngage && !bActed);
	if (QuietFor >= QuietSeconds)
	{
		Context.Raise([this, &Sample, &Context, QuietFor]
		{
			const FString Why = Explain(Sample, Context.AgentKey);
			return FString::Printf(TEXT("Fighting %s, in sight, for %.1f s without acting.%s%s"), *Sample.Target.ToString(), QuietFor, Why.IsEmpty() ? TEXT("") : TEXT(" "), *Why);
		});
	}
}

FMissingSenseRule::FMissingSenseRule(bool bInNeedsSight, bool bInNeedsHearing, bool bInNeedsDamage)
	: FRule(TEXT("MissingSense"), ESeverity::Error, ReportOnce)
	, bNeedsSight(bInNeedsSight)
	, bNeedsHearing(bInNeedsHearing)
	, bNeedsDamage(bInNeedsDamage)
{
}

void FMissingSenseRule::OnSample(const FAgentSample& Sample, FRuleContext& Context)
{
	const bool bNoSight = bNeedsSight && !Sample.bHasSight;
	const bool bNoHearing = bNeedsHearing && !Sample.bHasHearing;
	const bool bNoDamage = bNeedsDamage && !Sample.bHasDamageSense;
	if (bNoSight || bNoHearing || bNoDamage)
	{
		Context.Raise([bNoSight, bNoHearing, bNoDamage]
		{
			TArray<const TCHAR*> Missing;
			if (bNoSight)
			{
				Missing.Add(TEXT("sight"));
			}
			if (bNoHearing)
			{
				Missing.Add(TEXT("hearing"));
			}
			if (bNoDamage)
			{
				Missing.Add(TEXT("damage"));
			}
			return FString::Printf(TEXT("Its perception has no %s sense configured, so it never reacts to what that would report."),
				*FString::Join(Missing, TEXT(" or ")));
		});
	}
}

void AddCoreRules(TArray<TUniquePtr<FRule>>& OutRules)
{
	OutRules.Add(MakeUnique<FStuckRule>());
	OutRules.Add(MakeUnique<FMovingToOriginRule>());
	OutRules.Add(MakeUnique<FTreeNotRunningRule>());
	OutRules.Add(MakeUnique<FStateThrashRule>());
	OutRules.Add(MakeUnique<FDeadButActiveRule>());
	OutRules.Add(MakeUnique<FAttackWithoutTargetRule>());
}

FString JoinStates(TConstArrayView<FName> States)
{
	return States.IsEmpty()
		? FString(TEXT("(unknown)"))
		: FString::JoinBy(States, TEXT(" > "), [](FName State) { return State.ToString(); });
}

} // namespace Gaia

#endif // GAIA_ENABLED
