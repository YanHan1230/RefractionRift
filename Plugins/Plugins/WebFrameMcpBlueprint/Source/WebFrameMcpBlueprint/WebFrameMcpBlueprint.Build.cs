// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class WebFrameMcpBlueprint : ModuleRules
{
	public WebFrameMcpBlueprint(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Json",
				"WebFrameMcp"
			}
		);
	}
}
