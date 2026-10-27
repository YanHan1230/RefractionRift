// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Ticker.h"
#include "Containers/Map.h"
#include "Modules/ModuleManager.h"
#include "Templates/Function.h"
#include "UObject/TopLevelAssetPath.h"

class IWebSocketServer;
class INetworkingWebSocket;
class SWebFrameMcpPanel;

struct WEBFRAMEMCP_API FWebFrameMcpBlueprintChange
{
	FString ObjectName;
	FString AssetPath;
	FString Summary;
	TArray<TPair<FString, FString>> Details;
};

struct WEBFRAMEMCP_API FWebFrameMcpCommandSpec
{
	FName Owner;
	FString Method;
	FString Description;
	TFunction<TSharedPtr<class FJsonObject>(const TSharedPtr<class FJsonObject>& Params, FString& OutError)> Handler;
};

struct WEBFRAMEMCP_API FWebFrameMcpPathPermission
{
	FString Path;
	bool bAllowAccess = true;
	bool bAllowModify = true;
};

class WEBFRAMEMCP_API FWebFrameMcpModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	void OpenPanel();
	void AddConsoleLine(const FString& Line);
	const TArray<FString>& GetConsoleLines() const { return ConsoleLines; }
	bool IsServerRunning() const { return bServerRunning; }
	int32 GetPort() const { return Port; }
	void SetPort(int32 InPort);
	void ToggleServer();
	const TArray<TSharedPtr<FWebFrameMcpBlueprintChange>>& GetBlueprintChanges() const { return BlueprintChanges; }
	void ClearBlueprintChangeRecords();
	void RegisterCommand(const FWebFrameMcpCommandSpec& Spec);
	void UnregisterCommandsByOwner(FName Owner);
	TArray<FName> GetRegisteredCommandOwners() const;
	int32 GetRegisteredCommandCountByOwner(FName Owner) const;
	bool IsCommandOwnerEnabled(FName Owner) const;
	void SetCommandOwnerEnabled(FName Owner, bool bEnabled);
	const TArray<FWebFrameMcpPathPermission>& GetPathPermissions(FName Owner) const;
	void AddPathPermission(FName Owner, const FString& Path, bool bAllowAccess, bool bAllowModify);
	void RemovePathPermission(FName Owner, int32 Index);
	void SetPathPermission(FName Owner, int32 Index, bool bAllowAccess, bool bAllowModify);

	TSharedRef<class SDockTab> SpawnPanelTab(const class FSpawnTabArgs& Args);
	void RegisterToolbar();
	void UnregisterToolbar();
	bool StartServer();
	void StopServer();
	bool TickServer(float DeltaTime);
	void OnClientConnected(INetworkingWebSocket* Peer);
	void HandleClientMessage(INetworkingWebSocket* Peer, void* Data, int32 Size);
	void SendResponse(INetworkingWebSocket* Peer, const FString& Id, bool bOk, const TSharedPtr<class FJsonObject>& Result, const FString& Error = FString());
	TSharedPtr<class FJsonObject> DispatchCommand(const FString& Method, const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> BuildStatusPayload() const;
	TSharedPtr<class FJsonObject> RenameAsset(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> ListWorldActors(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> DescribeWorldActor(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> SpawnWorldActor(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> SetWorldActorTransform(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> DeleteWorldActor(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> SetWorldActorProperty(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> SaveCurrentWorldLevel(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> GetWorldActorBounds(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> DescribeWorldActorMeshGeometry(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> AddPostProcessMaterial(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> SpawnPostProcessVolume(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> CreateBlueprintAsset(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> CanCreateBlueprintChild(const TSharedPtr<class FJsonObject>& Params, FString& OutError) const;
	TSharedPtr<class FJsonObject> DescribeBlueprintAsset(const FString& AssetPath, FString& OutError);
	TSharedPtr<class FJsonObject> ListBlueprintVariables(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> AddBlueprintVariable(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> RenameBlueprintVariable(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> RemoveBlueprintVariable(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> SetBlueprintVariableDefault(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> SetBlueprintClassDefault(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> SetBlueprintComponentDefault(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> SetBlueprintPinDefault(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> AddBlueprintFunctionGraph(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> AddBlueprintMacroGraph(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> CreateBlueprintCallFunctionNode(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> CreateBlueprintCustomEventNode(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> CreateBlueprintCommentNode(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> CreateBlueprintVariableNode(const TSharedPtr<class FJsonObject>& Params, bool bSetter, FString& OutError);
	TSharedPtr<class FJsonObject> ConnectBlueprintNodePins(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> MoveBlueprintNode(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> DeleteBlueprintNode(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> DisconnectBlueprintNodePins(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> CompileBlueprintAsset(const FString& AssetPath, FString& OutError);
	TSharedPtr<class FJsonObject> CreateMaterialAsset(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> DescribeMaterialAsset(const FString& AssetPath, FString& OutError);
	TSharedPtr<class FJsonObject> SetMaterialSettings(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> CreateMaterialInstanceAsset(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> SetMaterialInstanceParameter(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> AddMaterialExpressionNode(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> SetMaterialExpressionProperty(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> ConnectMaterialExpressionNodes(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> ConnectMaterialExpressionToProperty(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> DuplicateMaterialExpressionNode(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> LayoutMaterialExpressionNodes(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> MoveMaterialExpressionNode(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> DeleteMaterialExpressionNode(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> DisconnectMaterialExpressionNode(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> DisconnectMaterialExpressionFromProperty(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> CompileMaterialAsset(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> GetMaterialDiagnostics(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> DescribeMeshAsset(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> DescribeMeshGeometry(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> SetMeshMaterialSlot(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> AddMeshSocket(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> UpdateMeshSocket(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> RemoveMeshSocket(const TSharedPtr<class FJsonObject>& Params, FString& OutError);
	TSharedPtr<class FJsonObject> ListAssets(const FTopLevelAssetPath& ClassPathName, const TSharedPtr<class FJsonObject>& Params, FString& OutError) const;
	FString ResolveHourlyLogPath() const;
	void WriteExternalCommandLog(const FString& Line);
	void RecordBlueprintChange(const FString& AssetPath, const FString& Summary, const TArray<TPair<FString, FString>>& Details);

private:
	bool ValidatePathPermissionsForCommand(const FWebFrameMcpCommandSpec& Command, const TSharedPtr<class FJsonObject>& Params, FString& OutError) const;
	void RecordSuccessfulCommandIfNeeded(const FString& Method, const TSharedPtr<class FJsonObject>& Params, const TSharedPtr<class FJsonObject>& Result);
	FString ResolveSettingsPath() const;
	void LoadUserSettings();
	void SaveUserSettings() const;
	TSharedPtr<class FExtender> ToolbarExtender;
	TSharedPtr<class FSlateStyleSet> StyleSet;
	TSharedPtr<SWebFrameMcpPanel> PanelWidget;
	TUniquePtr<IWebSocketServer> WebSocketServer;
	TArray<INetworkingWebSocket*> Clients;
	FTSTicker::FDelegateHandle TickHandle;
	TArray<FString> ConsoleLines;
	TArray<TSharedPtr<FWebFrameMcpBlueprintChange>> BlueprintChanges;
	TMap<FString, FWebFrameMcpCommandSpec> RegisteredCommands;
	TMap<FName, bool> CommandOwnerEnabled;
	TMap<FName, TArray<FWebFrameMcpPathPermission>> PathPermissionsByOwner;
	int32 Port = 30303;
	bool bServerRunning = false;
	bool bStartServerOnStartup = false;
};
