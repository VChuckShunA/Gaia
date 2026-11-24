// Copyright VChuckShunA. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaiaRule.h"
#include "UObject/Object.h"
#include "GaiaProbe.generated.h"

class AAIController;

/**
 * Reads AI agents into plain data for Gaia. This base class watches every possessed AI pawn and reads what the engine
 * knows, so a project gets the core rules and a log with no setup. A project derives from it, names the class in
 * UGaiaSettings, and adds what only it knows: the common facts on FAgentSample, a FProjectSample, and its own rules.
 *
 * The subsystem creates one for each recording and owns it. A probe only reads: it never changes an agent. Game thread only.
 */
UCLASS()
class GAIA_API UGaiaProbe : public UObject
{
	GENERATED_BODY()

#if GAIA_ENABLED
public:
	/** Called as a recording starts, before anything else. Look up enum names here, as captures happen too often to. */
	virtual void BeginSession();

	/** Whether Gaia should watch Pawn, which Controller possesses. */
	virtual bool IsAgent(const AAIController& Controller, const APawn& Pawn) const { return true; }

	/** Fills in everything about an agent but its identity, which the subsystem has set. Only called if IsAgent is true. */
	virtual void CaptureAgent(const AAIController& Controller, const APawn& Pawn, Gaia::FAgentSample& OutSample) const;

	/**
	 * Adds the assets that decide how an agent behaves, as a role and a name, called once per agent. The engine doesn't
	 * expose a component's StateTree, so only the project can say.
	 */
	virtual void CaptureAssets(const AAIController& Controller, const APawn& Pawn, TArray<TPair<FName, FName>>& OutAssets) const {}

	/** Creates the rules for a recording. They go to the worker thread, and are destroyed with it. */
	virtual void CreateRules(TArray<TUniquePtr<Gaia::FRule>>& OutRules) const;

protected:
	/** The names of an enum's values, indexed by value, for looking up in captures. */
	template <typename TEnum>
	static TArray<FName> GetValueNames()
	{
		TArray<FName> Names;
		if (const UEnum* Enum = StaticEnum<TEnum>())
		{
			for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
			{
				const int64 Value = Enum->GetValueByIndex(Index);
				if (Value >= 0 && Value <= MAX_uint8)
				{
					const int32 NameIndex = static_cast<int32>(Value);
					if (Names.Num() <= NameIndex)
					{
						Names.SetNum(NameIndex + 1);
					}
					Names[NameIndex] = FName(Enum->GetNameStringByIndex(Index));
				}
			}
		}
		return Names;
	}

	/** The name of a raw enum value from GetValueNames; None if there isn't one. */
	static FName NameOf(const TArray<FName>& Names, uint8 Value)
	{
		return Names.IsValidIndex(Value) ? Names[Value] : NAME_None;
	}

private:
	TArray<FName> TreeStatusNames;
	TArray<FName> PathStatusNames;
	TArray<FName> MovementModeNames;
#endif // GAIA_ENABLED
};
