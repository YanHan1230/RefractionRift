// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class WebFrameMcpMaterial : ModuleRules
{
	public WebFrameMcpMaterial(ReadOnlyTargetRules Target) : base(Target)
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
