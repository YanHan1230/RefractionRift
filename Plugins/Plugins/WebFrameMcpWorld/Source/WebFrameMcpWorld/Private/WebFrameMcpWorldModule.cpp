// Copyright Epic Games, Inc. All Rights Reserved.

#include "WebFrameMcpModule.h"

#include "Dom/JsonObject.h"
#include "Modules/ModuleManager.h"

namespace
{
	const FName OwnerName(TEXT("WebFrameMcpWorld"));
}

class FWebFrameMcpWorldModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FWebFrameMcpModule& Core = FModuleManager::LoadModuleChecked<FWebFrameMcpModule>(TEXT("WebFrameMcp"));

		Core.RegisterCommand({ OwnerName, TEXT("world.actor.list"), TEXT("List loaded editor-world actors."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.ListWorldActors(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("world.actor.describe"), TEXT("Describe a loaded editor-world actor."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.DescribeWorldActor(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("world.actor.spawn"), TEXT("Spawn an actor into the current editor world."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.SpawnWorldActor(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("world.actor.set_transform"), TEXT("Set an actor transform in the current editor world."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.SetWorldActorTransform(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("world.actor.delete"), TEXT("Delete an actor from the current editor world."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.DeleteWorldActor(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("world.actor.set_property"), TEXT("Set an actor or component property in the current editor world."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.SetWorldActorProperty(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("world.save_current_level"), TEXT("Save the current editor world level."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.SaveCurrentWorldLevel(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("world.actor.get_bounds"), TEXT("Get actor bounds in the current editor world."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.GetWorldActorBounds(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("world.actor.mesh_geometry"), TEXT("Describe world-space StaticMeshComponent geometry on an actor."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.DescribeWorldActorMeshGeometry(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("world.actor.mesh_geometry.describe"), TEXT("Describe world-space StaticMeshComponent geometry on an actor."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.DescribeWorldActorMeshGeometry(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("world.postprocess.add_material"), TEXT("Add or update a PostProcessVolume material."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.AddPostProcessMaterial(Params, OutError); } });
		Core.RegisterCommand({ OwnerName, TEXT("world.postprocess.spawn_volume"), TEXT("Spawn a PostProcessVolume in the current editor world."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.SpawnPostProcessVolume(Params, OutError); } });
	}

	virtual void ShutdownModule() override
	{
		if (FWebFrameMcpModule* Core = FModuleManager::GetModulePtr<FWebFrameMcpModule>(TEXT("WebFrameMcp")))
		{
			Core->UnregisterCommandsByOwner(OwnerName);
		}
	}
};

IMPLEMENT_MODULE(FWebFrameMcpWorldModule, WebFrameMcpWorld)
