// Copyright Epic Games, Inc. All Rights Reserved.

#include "WebFrameMcpModule.h"

#include "Dom/JsonObject.h"
#include "Modules/ModuleManager.h"
#include "UObject/TopLevelAssetPath.h"

namespace
{
	const FName OwnerName(TEXT("WebFrameMcpBlueprint"));
}

class FWebFrameMcpBlueprintModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FWebFrameMcpModule& Core = FModuleManager::LoadModuleChecked<FWebFrameMcpModule>(TEXT("WebFrameMcp"));

		Core.RegisterCommand({ OwnerName, TEXT("blueprint.list"), TEXT("List Blueprint assets."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.ListAssets(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("Blueprint")), Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.create"), TEXT("Create a Blueprint asset."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.CreateBlueprintAsset(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.can_create_child"), TEXT("Check whether a Blueprint child can be created."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.CanCreateBlueprintChild(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.describe"), TEXT("Describe Blueprint graphs, variables, functions, and macros."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString AssetPath;
			Params->TryGetStringField(TEXT("asset"), AssetPath);
			return Core.DescribeBlueprintAsset(AssetPath, OutError);
		} });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.variables"), TEXT("List Blueprint variables."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.ListBlueprintVariables(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.variable.add"), TEXT("Add a Blueprint variable."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.AddBlueprintVariable(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.variable.rename"), TEXT("Rename a Blueprint variable."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.RenameBlueprintVariable(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.variable.remove"), TEXT("Remove a Blueprint variable."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.RemoveBlueprintVariable(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.set_variable_default"), TEXT("Set a Blueprint variable default value."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.SetBlueprintVariableDefault(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.set_class_default"), TEXT("Set a Blueprint class default property."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.SetBlueprintClassDefault(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.set_component_default"), TEXT("Set a Blueprint component default property."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.SetBlueprintComponentDefault(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.set_pin_default"), TEXT("Set a Blueprint node pin default value."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.SetBlueprintPinDefault(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.graph.add_function"), TEXT("Add a Blueprint function graph."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.AddBlueprintFunctionGraph(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.graph.add_macro"), TEXT("Add a Blueprint macro graph."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.AddBlueprintMacroGraph(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.node.call_function"), TEXT("Create a Blueprint function call node."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.CreateBlueprintCallFunctionNode(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.node.custom_event"), TEXT("Create a Blueprint custom event node."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.CreateBlueprintCustomEventNode(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.node.comment"), TEXT("Create a Blueprint comment node."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.CreateBlueprintCommentNode(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.node.variable_get"), TEXT("Create a Blueprint variable get node."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.CreateBlueprintVariableNode(Params, false, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.node.variable_set"), TEXT("Create a Blueprint variable set node."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.CreateBlueprintVariableNode(Params, true, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.node.connect"), TEXT("Connect Blueprint node pins."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.ConnectBlueprintNodePins(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.node.move"), TEXT("Move a Blueprint graph node."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.MoveBlueprintNode(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.node.delete"), TEXT("Delete a Blueprint graph node."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.DeleteBlueprintNode(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.node.disconnect"), TEXT("Disconnect Blueprint node pins or all links on a node."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.DisconnectBlueprintNodePins(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("blueprint.compile"), TEXT("Compile a Blueprint asset."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString AssetPath;
			Params->TryGetStringField(TEXT("asset"), AssetPath);
			return Core.CompileBlueprintAsset(AssetPath, OutError);
		} });
	}

	virtual void ShutdownModule() override
	{
		if (FWebFrameMcpModule* Core = FModuleManager::GetModulePtr<FWebFrameMcpModule>(TEXT("WebFrameMcp")))
		{
			Core->UnregisterCommandsByOwner(OwnerName);
		}
	}
};

IMPLEMENT_MODULE(FWebFrameMcpBlueprintModule, WebFrameMcpBlueprint)
