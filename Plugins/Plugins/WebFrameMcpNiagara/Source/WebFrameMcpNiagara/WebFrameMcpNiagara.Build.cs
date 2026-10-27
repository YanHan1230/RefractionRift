// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class WebFrameMcpNiagara : ModuleRules
{
	public WebFrameMcpNiagara(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.NoPCHs;

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"AssetRegistry",
				"AssetTools",
				"UnrealEd",
				"Json",
				"Niagara",
				"NiagaraCore",
				"NiagaraEditor",
				"WebFrameMcp"
			}
		);
	}
}
