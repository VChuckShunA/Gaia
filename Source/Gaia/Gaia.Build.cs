// Copyright VChuckShunA. All Rights Reserved.

using UnrealBuildTool;

public class Gaia : ModuleRules
{
	public Gaia(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "DeveloperSettings", "Engine", "GameplayTags" });

		// Json is for the log (JSON Lines).
		PrivateDependencyModuleNames.AddRange(new string[] { "AIModule", "GameplayStateTreeModule", "Json", "NavigationSystem", "StateTreeModule" });

		// Naming a StateTree's active states needs the gameplay debugger (defines WITH_GAMEPLAY_DEBUGGER).
		SetupGameplayDebuggerSupport(Target);
	}
}
