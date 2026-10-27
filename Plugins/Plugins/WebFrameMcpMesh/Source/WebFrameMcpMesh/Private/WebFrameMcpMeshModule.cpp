// Copyright Epic Games, Inc. All Rights Reserved.

#include "WebFrameMcpModule.h"

#include "Dom/JsonObject.h"
#include "Modules/ModuleManager.h"
#include "UObject/TopLevelAssetPath.h"

namespace
{
	const FName OwnerName(TEXT("WebFrameMcpMesh"));
}

class FWebFrameMcpMeshModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FWebFrameMcpModule& Core = FModuleManager::LoadModuleChecked<FWebFrameMcpModule>(TEXT("WebFrameMcp"));

		Core.RegisterCommand({ OwnerName, TEXT("mesh.list"), TEXT("List mesh assets."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString Type;
			Params->TryGetStringField(TEXT("type"), Type);
			if (Type.Equals(TEXT("static"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("staticmesh"), ESearchCase::IgnoreCase))
			{
				return Core.ListAssets(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("StaticMesh")), Params, OutError);
			}
			if (Type.Equals(TEXT("skeletal"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("skeletalmesh"), ESearchCase::IgnoreCase))
			{
				return Core.ListAssets(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("SkeletalMesh")), Params, OutError);
			}

			TSharedPtr<FJsonObject> StaticResult = Core.ListAssets(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("StaticMesh")), Params, OutError);
			if (!StaticResult.IsValid())
			{
				return TSharedPtr<FJsonObject>();
			}
			TSharedPtr<FJsonObject> SkeletalResult = Core.ListAssets(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("SkeletalMesh")), Params, OutError);
			if (!SkeletalResult.IsValid())
			{
				return TSharedPtr<FJsonObject>();
			}

			TArray<TSharedPtr<FJsonValue>> Assets = StaticResult->GetArrayField(TEXT("assets"));
			Assets.Append(SkeletalResult->GetArrayField(TEXT("assets")));
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetArrayField(TEXT("assets"), Assets);
			Result->SetStringField(TEXT("path"), StaticResult->GetStringField(TEXT("path")));
			Result->SetBoolField(TEXT("recursive"), StaticResult->GetBoolField(TEXT("recursive")));
			Result->SetNumberField(TEXT("count"), Assets.Num());
			Result->SetNumberField(TEXT("staticTotal"), StaticResult->GetNumberField(TEXT("total")));
			Result->SetNumberField(TEXT("skeletalTotal"), SkeletalResult->GetNumberField(TEXT("total")));
			Result->SetNumberField(TEXT("total"), StaticResult->GetNumberField(TEXT("total")) + SkeletalResult->GetNumberField(TEXT("total")));
			Result->SetBoolField(TEXT("hasMore"), StaticResult->GetBoolField(TEXT("hasMore")) || SkeletalResult->GetBoolField(TEXT("hasMore")));
			return Result;
		} });
		Core.RegisterCommand({ OwnerName, TEXT("mesh.describe"), TEXT("Describe mesh asset materials, bounds, bones, and sockets."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.DescribeMeshAsset(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("mesh.geometry.describe"), TEXT("Describe StaticMesh vertices, triangles, and radial bounds."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.DescribeMeshGeometry(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("mesh.set_material_slot"), TEXT("Set a mesh material slot."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.SetMeshMaterialSlot(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("mesh.add_socket"), TEXT("Add a mesh socket."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.AddMeshSocket(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("mesh.update_socket"), TEXT("Update a mesh socket."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.UpdateMeshSocket(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("mesh.remove_socket"), TEXT("Remove a mesh socket."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.RemoveMeshSocket(Params, OutError); } });
	}

	virtual void ShutdownModule() override
	{
		if (FWebFrameMcpModule* Core = FModuleManager::GetModulePtr<FWebFrameMcpModule>(TEXT("WebFrameMcp")))
		{
			Core->UnregisterCommandsByOwner(OwnerName);
		}
	}
};

IMPLEMENT_MODULE(FWebFrameMcpMeshModule, WebFrameMcpMesh)
