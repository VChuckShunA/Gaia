// Copyright VChuckShunA. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaiaCapture.h"
#include "GaiaTypes.h"

#if GAIA_ENABLED

namespace Gaia
{
	class FJsonFields;
	class FRule;
	class FWorker;

	/** A rule's memory of one agent. FRuleContext reads and writes it. */
	struct FRuleState
	{
		/** When the rule's condition started holding without a break; negative while it doesn't hold */
		double ConditionSince = -1.0;
		/** When the rule last raised a problem; negative if never */
		double LastRaised = -1.0;
		int32 TimesRaised = 0;
		/** Whether the rule has reported the condition that holds now */
		bool bRaisedThisHold = false;
		/** When a reported condition was last seen holding */
		double LastHeldTime = -1.0;
		/** How long the conditions the rule reported lasted, in total */
		double ProblemSeconds = 0.0;
		/** Recent occurrences, for spotting loops */
		TArray<double> RecentTimes;
	};

	/** What a rule can see and do for one agent at one moment. Worker thread only. */
	class GAIA_API FRuleContext
	{
	public:
		FRuleContext(FWorker& InWorker, const FRule& InRule, FRuleState& InState, const FObjectKey& InAgentKey, FName InAgent, double InNow, const FAgentSample* InPrevious)
			: Now(InNow)
			, Previous(InPrevious)
			, AgentKey(InAgentKey)
			, Worker(InWorker)
			, Rule(InRule)
			, State(InState)
			, Agent(InAgent)
		{
		}

		/** Tracks how long the rule's condition has held. Returns that time, or a negative value if it doesn't hold. */
		double HeldFor(bool bCondition);

		/** When the rule's condition started holding; negative while it doesn't hold. */
		double GetConditionSince() const { return State.ConditionSince; }

		/** Counts an occurrence now, forgets those more than Window seconds ago, and returns how many are left. */
		int32 CountRecent(double Window);

		/** Reports a problem unless this rule did for this agent within its cooldown. MakeMessage is only called if it does. */
		void Raise(TFunctionRef<FString()> MakeMessage);

		/** World time of the sample or event being looked at. */
		const double Now;
		/** The agent's previous sample; null for an agent's first. */
		const FAgentSample* const Previous;
		const FObjectKey AgentKey;

	private:
		FWorker& Worker;
		const FRule& Rule;
		FRuleState& State;
		FName Agent;
	};

	/**
	 * Something Gaia checks every agent for. One instance serves every agent, so it keeps per-agent memory in FRuleContext,
	 * or in its own map keyed by agent. It runs on the worker thread and only sees plain data. Thresholds are generous on
	 * purpose: a false alarm costs the tool more trust than a late one.
	 */
	class GAIA_API FRule
	{
	public:
		/** Least time between two reports of a rule for one agent, unless the rule says otherwise. */
		static constexpr double DefaultCooldown = 10.0;
		static constexpr double ReportOnce = TNumericLimits<double>::Max();

		FRule(const TCHAR* InName, ESeverity InSeverity, double InCooldown = DefaultCooldown)
			: Name(InName)
			, Severity(InSeverity)
			, Cooldown(InCooldown)
		{
		}

		virtual ~FRule() = default;

		/** Whether the rule is about an agent's logic, so has nothing to say while a test has stopped it on purpose. */
		virtual bool NeedsRunningLogic() const { return true; }

		/** Checks a fresh sample. Context.Previous is the one before it. */
		virtual void OnSample(const FAgentSample& Sample, FRuleContext& Context) {}

		virtual void OnEvent(const FRecordedEvent& Event, FRuleContext& Context) {}

		/** Adds what the rule counted about an agent to the log's end-of-session summary. */
		virtual void AddToSummary(const FObjectKey& AgentKey, FJsonFields& Fields) const {}

		/** Adds what the rule counted about an agent to its line in the console report. */
		virtual void AddToReport(const FObjectKey& AgentKey, FString& Text) const {}

		const TCHAR* GetName() const { return Name; }
		ESeverity GetSeverity() const { return Severity; }
		double GetCooldown() const { return Cooldown; }

	private:
		const TCHAR* Name;
		ESeverity Severity;
		double Cooldown;
	};

	/** In combat with a visible target and the means to act, but not acting. Needs the probe's common facts. */
	class GAIA_API FNotEngagingRule : public FRule
	{
	public:
		FNotEngagingRule();

		virtual void OnSample(const FAgentSample& Sample, FRuleContext& Context) override;

	protected:
		/** Why it isn't acting, in the project's words, or empty if there's nothing to add. */
		virtual FString Explain(const FAgentSample& Sample, const FObjectKey& AgentKey) const { return FString(); }
	};

	/** A sense the project's agents all rely on is missing from an agent's perception. */
	class GAIA_API FMissingSenseRule : public FRule
	{
	public:
		FMissingSenseRule(bool bInNeedsSight, bool bInNeedsHearing, bool bInNeedsDamage);

		virtual bool NeedsRunningLogic() const override { return false; }
		virtual void OnSample(const FAgentSample& Sample, FRuleContext& Context) override;

	private:
		bool bNeedsSight;
		bool bNeedsHearing;
		bool bNeedsDamage;
	};

	/** Adds the rules that need nothing from the project, and those any project's common facts feed, but FNotEngagingRule. */
	GAIA_API void AddCoreRules(TArray<TUniquePtr<FRule>>& OutRules);

	/** States joined outermost first, for messages. */
	GAIA_API FString JoinStates(TConstArrayView<FName> States);
}

#endif // GAIA_ENABLED
