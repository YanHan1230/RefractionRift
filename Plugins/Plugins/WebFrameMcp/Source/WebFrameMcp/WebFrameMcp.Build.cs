// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class WebFrameMcp : ModuleRules
{
	public WebFrameMcp(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"ApplicationCore",
				"AssetRegistry",
				"AssetTools",
				"BlueprintGraph",
				"CoreUObject",
				"Engine",
				"GraphEditor",
				"InputCore",
				"Json",
				"JsonUtilities",
				"Kismet",
				"KismetCompiler",
				"LevelEditor",
				"MaterialEditor",
				"Projects",
				"Slate",
				"SlateCore",
				"ToolMenus",
				"UnrealEd",
				"WebSocketNetworking"
			}
		);
	}
}
