// Copyright VChuckShunA. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaiaTypes.h"

#if GAIA_ENABLED

#include "Containers/Queue.h"
#include "GaiaCapture.h"
#include "GaiaRule.h"
#include "HAL/Event.h"
#include "HAL/Runnable.h"
#include "Misc/SingleThreadRunnable.h"
#include <atomic>

namespace Gaia
{
	/** What the worker needs to know about a recording session, fixed when it starts. */
	struct FSessionInfo
	{
		FString LogFilePath;
		FString MapName;
		FString NetMode;
		/** What the test running this session calls it; empty otherwise */
		FString Label;
		double StartWorldTime = 0.0;
		TArray<TUniquePtr<FRule>> Rules;
	};

	/**
	 * Gaia's worker. Takes what the game thread captures, works out what changed and whether anything looks wrong,
	 * writes it all to a JSON Lines file, and hands the problems it finds back to the game thread.
	 *
	 * It shares nothing with the game thread but three single-producer single-consumer queues (captures in; problems
	 * and report lines out) and two flags. On platforms without threads the engine ticks it on the game thread instead.
	 */
	class FWorker final : public FRunnable, private FSingleThreadRunnable
	{
	public:
		explicit FWorker(FSessionInfo&& InSession);
		virtual ~FWorker() override;

		// Game thread --------------------------------------------------------------------------------------------------

		/** Queues a capture. Returns false, dropping it, if the worker has fallen too far behind. */
		bool Submit(FCapture&& Capture);

		bool PollAnomaly(FAnomaly& OutAnomaly);

		/** Asks for a summary of every agent, delivered through PollReportLine. */
		void RequestReport();
		bool PollReportLine(FString& OutLine);

		/**
		 * Once the thread has finished: processes what's left, writes the session summary and closes the file. Safe to
		 * call more than once. Returns a one-line summary of the session.
		 */
		FString FinishSession();

		const FString& GetLogFilePath() const { return Session.LogFilePath; }

		// FRunnable ----------------------------------------------------------------------------------------------------

		virtual uint32 Run() override;
		virtual void Stop() override;
		virtual void Exit() override;
		virtual FSingleThreadRunnable* GetSingleThreadInterface() override { return this; }

		/** Reports a rule's problem unless its cooldown forbids it. For FRuleContext. */
		void Raise(const FRule& Rule, FRuleState& State, const FObjectKey& AgentKey, FName Agent, double Now, TFunctionRef<FString()> MakeMessage);

	private:
		/** What the worker remembers about one agent between frames. */
		struct FAgentHistory
		{
			FObjectKey AgentKey;
			FName Agent;
			/** The name people know it by, for the report */
			FString Label;
			FName Class;
			bool bHasSample = false;
			bool bRemoved = false;
			FAgentSample Last;

			/** Indexed like FSessionInfo::Rules. */
			TArray<FRuleState> RuleStates;

			/** A test said its logic is stopped on purpose, and it hasn't run again since. */
			bool bLogicStopExpected = false;
			bool bLogicStopSeen = false;

			// For the coverage record
			TMap<FName, int32> EventCounts;
			TMap<FString, int32> StatesEntered;

			// For the report
			double FirstSeenTime = 0.0;
			double LastSeenTime = 0.0;
			TMap<FName, double> TimeInActivity;
		};

		// FSingleThreadRunnable: replaces Run where there are no threads; called once a frame on the game thread.
		virtual void Tick() override;

		void ProcessPending();
		void ProcessCapture(FCapture&& Capture);
		void ProcessEvent(const FRecordedEvent& Event);
		void ProcessSample(double Now, bool bWriteSnapshot, FAgentSample&& Sample);

		/** Checks a fresh sample against the rules. History.Last is still the previous sample. */
		void CheckSampleRules(double Now, FAgentHistory& History, const FAgentSample& Sample);

		/** Whether the agent's logic is stopped on purpose, following the declaration through the stop and the restart. */
		static bool IsLogicStoppedOnPurpose(FAgentHistory& History, const FAgentSample& Sample);

		void Emit(FAnomaly&& Anomaly);

		FAgentHistory& FindOrAddHistory(const FObjectKey& AgentKey, FName Agent, double Now);

		/** Opens the log on first use, so worlds without AI leave no empty logs behind. */
		void OpenLogFile();

		void WriteAgent(double Now, const FAgentSample& Sample);
		void WriteChanges(double Now, const FAgentSample& Old, const FAgentSample& New);
		void WriteSnapshot(double Now, const FAgentSample& Sample);
		void WriteSummary(double Now, const FAgentHistory& History);

		/** Writes what each class of agent did and where its logic went, so a test can see what never happened. */
		void WriteCoverage(double Now);

		/** Writes what's buffered to the file, flushing it to disk once a second so a crash loses little. */
		void WritePending(bool bForceFlush);

		void BuildReport();
		FString DescribeAgent(const FAgentHistory& History) const;

		FSessionInfo Session;

		// Shared with the game thread
		TQueue<FCapture, EQueueMode::Spsc> Captures;
		TQueue<FAnomaly, EQueueMode::Spsc> Anomalies;
		TQueue<FString, EQueueMode::Spsc> ReportLines;
		std::atomic<int32> QueuedCaptures{0};
		std::atomic<bool> bStopRequested{false};
		std::atomic<bool> bReportRequested{false};
		FEventRef WorkEvent;

		// The worker's alone (the game thread's once the thread has finished)
		TMap<FObjectKey, FAgentHistory> Histories;
		TUniquePtr<FArchive> File;
		bool bTriedOpeningFile = false;
		FString PendingText;
		double LastFlushSeconds = 0.0;
		double LastWorldTime = 0.0;
		uint64 LastFrame = 0;
		bool bReportedNoNavMesh = false;
		bool bFinished = false;
		FString FinalSummary;

		// Statistics
		int64 CapturesProcessed = 0;
		int64 SamplesProcessed = 0;
		int64 EventsProcessed = 0;
		int64 AnomaliesRaised = 0;
		int64 BytesWritten = 0;
		double TotalProcessSeconds = 0.0;
		double MaxProcessSeconds = 0.0;
	};
}

#endif // GAIA_ENABLED
