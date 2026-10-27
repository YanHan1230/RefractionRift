// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class WebFrameMcpMesh : ModuleRules
{
	public WebFrameMcpMesh(ReadOnlyTargetRules Target) : base(Target)
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
