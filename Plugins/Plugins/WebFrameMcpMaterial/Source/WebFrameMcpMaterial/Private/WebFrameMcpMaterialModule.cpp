// Copyright Epic Games, Inc. All Rights Reserved.

#include "WebFrameMcpModule.h"

#include "Dom/JsonObject.h"
#include "Modules/ModuleManager.h"
#include "UObject/TopLevelAssetPath.h"

namespace
{
	const FName OwnerName(TEXT("WebFrameMcpMaterial"));
}

class FWebFrameMcpMaterialModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FWebFrameMcpModule& Core = FModuleManager::LoadModuleChecked<FWebFrameMcpModule>(TEXT("WebFrameMcp"));

		Core.RegisterCommand({ OwnerName, TEXT("material.list"), TEXT("List Material assets."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.ListAssets(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("Material")), Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("material.create"), TEXT("Create a Material asset."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.CreateMaterialAsset(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("material.describe"), TEXT("Describe a Material graph and settings."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString AssetPath;
			Params->TryGetStringField(TEXT("asset"), AssetPath);
			return Core.DescribeMaterialAsset(AssetPath, OutError);
		} });
		Core.RegisterCommand({ OwnerName, TEXT("material.set_settings"), TEXT("Set Material settings."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.SetMaterialSettings(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("material.instance.create"), TEXT("Create a MaterialInstanceConstant asset."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.CreateMaterialInstanceAsset(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("material.instance.set_parameter"), TEXT("Set a MaterialInstanceConstant scalar, vector, texture, or parent value."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.SetMaterialInstanceParameter(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("material.node.add"), TEXT("Add a Material expression node."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.AddMaterialExpressionNode(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("material.node.set_property"), TEXT("Set a Material expression property."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.SetMaterialExpressionProperty(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("material.node.connect"), TEXT("Connect Material expression nodes."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.ConnectMaterialExpressionNodes(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("material.connect_property"), TEXT("Connect a Material expression to an output property."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.ConnectMaterialExpressionToProperty(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("material.node.duplicate"), TEXT("Duplicate a Material expression node."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.DuplicateMaterialExpressionNode(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("material.node.layout"), TEXT("Layout Material expression nodes."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.LayoutMaterialExpressionNodes(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("material.node.move"), TEXT("Move a Material expression node."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.MoveMaterialExpressionNode(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("material.node.delete"), TEXT("Delete a Material expression node."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.DeleteMaterialExpressionNode(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("material.node.disconnect"), TEXT("Disconnect Material expression inputs."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.DisconnectMaterialExpressionNode(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("material.disconnect_property"), TEXT("Disconnect a Material output property."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.DisconnectMaterialExpressionFromProperty(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("material.compile"), TEXT("Recompile a Material asset and optionally save it."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.CompileMaterialAsset(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("material.diagnostics"), TEXT("Read recent Material compile diagnostics from the project log."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.GetMaterialDiagnostics(Params, OutError); } });
	}

	virtual void ShutdownModule() override
	{
		if (FWebFrameMcpModule* Core = FModuleManager::GetModulePtr<FWebFrameMcpModule>(TEXT("WebFrameMcp")))
		{
			Core->UnregisterCommandsByOwner(OwnerName);
		}
	}
};

IMPLEMENT_MODULE(FWebFrameMcpMaterialModule, WebFrameMcpMaterial)
