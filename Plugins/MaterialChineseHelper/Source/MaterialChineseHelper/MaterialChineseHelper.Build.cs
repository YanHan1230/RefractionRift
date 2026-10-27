using UnrealBuildTool;

public class MaterialChineseHelper : ModuleRules
{
    public MaterialChineseHelper(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "ApplicationCore", "InputCore",
            "Slate", "SlateCore", "UnrealEd", "GraphEditor", "ToolMenus", "Projects", "Json"
        });
        RuntimeDependencies.Add("$(PluginDir)/Resources/NodeDictionary.json");
    }
}
