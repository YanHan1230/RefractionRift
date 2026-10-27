// Copyright Epic Games, Inc. All Rights Reserved.

#include "WebFrameMcpModule.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "FileHelpers.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraLightRendererProperties.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraNode.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOp.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraTypes.h"
#include "NiagaraUserRedirectionParameterStore.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
	const FName OwnerName(TEXT("WebFrameMcpNiagara"));

	static FString JsonValueToString(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return FString();
		}
		if (Value->Type == EJson::String)
		{
			return Value->AsString();
		}
		if (Value->Type == EJson::Number)
		{
			const double Number = Value->AsNumber();
			const int64 IntegerNumber = static_cast<int64>(Number);
			return FMath::IsNearlyEqual(Number, static_cast<double>(IntegerNumber))
				? LexToString(IntegerNumber)
				: FString::SanitizeFloat(Number);
		}
		if (Value->Type == EJson::Boolean)
		{
			return Value->AsBool() ? TEXT("true") : TEXT("false");
		}

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Value, TEXT(""), Writer);
		return Out;
	}

	static bool GetStringParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FString& OutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Missing params.%s."), Field);
			return false;
		}
		return true;
	}

	static bool ReadBoolParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, bool bDefault = false)
	{
		bool bValue = bDefault;
		if (Params.IsValid())
		{
			Params->TryGetBoolField(Field, bValue);
		}
		return bValue;
	}

	static double ReadNumberParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, double DefaultValue = 0.0)
	{
		double Value = DefaultValue;
		if (Params.IsValid())
		{
			Params->TryGetNumberField(Field, Value);
		}
		return Value;
	}

	static int32 ReadIntParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, int32 DefaultValue = INDEX_NONE)
	{
		double Value = static_cast<double>(DefaultValue);
		if (Params.IsValid())
		{
			Params->TryGetNumberField(Field, Value);
		}
		return static_cast<int32>(Value);
	}

	static FString NormalizeAssetObjectPath(const FString& RawPath)
	{
		FString Path = RawPath;
		Path.ReplaceInline(TEXT("/All/Game"), TEXT("/Game"), ESearchCase::IgnoreCase);
		if (Path.StartsWith(TEXT("/Game")) || Path.StartsWith(TEXT("/Engine")) || Path.StartsWith(TEXT("/Niagara")))
		{
			if (!Path.Contains(TEXT(".")))
			{
				Path += TEXT(".") + FPaths::GetBaseFilename(Path);
			}
		}
		return Path;
	}

	static bool ResolvePackageAndAssetName(const TSharedPtr<FJsonObject>& Params, FString& OutPackagePath, FString& OutAssetName, FString& OutError)
	{
		FString AssetPath;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("asset"), AssetPath);
			Params->TryGetStringField(TEXT("path"), OutPackagePath);
			Params->TryGetStringField(TEXT("name"), OutAssetName);
		}

		if (!AssetPath.IsEmpty())
		{
			FString PackagePart;
			FString ObjectPart;
			if (AssetPath.Split(TEXT("."), &PackagePart, &ObjectPart, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
			{
				OutPackagePath = FPaths::GetPath(PackagePart);
				OutAssetName = ObjectPart;
			}
			else
			{
				OutPackagePath = FPaths::GetPath(AssetPath);
				OutAssetName = FPaths::GetBaseFilename(AssetPath);
			}
		}

		OutPackagePath.ReplaceInline(TEXT("/All/Game"), TEXT("/Game"), ESearchCase::IgnoreCase);
		if (!OutPackagePath.StartsWith(TEXT("/Game")))
		{
			OutError = FString::Printf(TEXT("Niagara asset path must be under /Game: %s."), *OutPackagePath);
			return false;
		}
		if (OutAssetName.IsEmpty())
		{
			OutError = TEXT("Missing params.name or params.asset.");
			return false;
		}
		return true;
	}

	static bool SaveAssetIfRequested(UObject* Asset, bool bSave)
	{
		if (!Asset)
		{
			return false;
		}

		Asset->MarkPackageDirty();
		if (!bSave)
		{
			return true;
		}

		UPackage* Package = Asset->GetOutermost();
		if (!Package)
		{
			return false;
		}

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Package);
		return FEditorFileUtils::PromptForCheckoutAndSave(PackagesToSave, false, false) == FEditorFileUtils::EPromptReturnCode::PR_Success;
	}

	static void NotifyAssetCreated(UObject* Asset)
	{
		if (!Asset)
		{
			return;
		}
		FAssetRegistryModule::AssetCreated(Asset);
		Asset->MarkPackageDirty();
	}

	static void MarkNiagaraAssetChanged(UObject* Asset)
	{
		if (!Asset)
		{
			return;
		}
		Asset->Modify();
		Asset->PostEditChange();
		Asset->MarkPackageDirty();
	}

	static void RecordOperation(FWebFrameMcpModule& Core, const FString& AssetPath, const FString& Summary, const FString& DetailObject, const FString& Detail)
	{
		TArray<TPair<FString, FString>> Details;
		Details.Add(TPair<FString, FString>(DetailObject, Detail));
		Core.RecordBlueprintChange(AssetPath, Summary, Details);
	}

	static FString TypeToString(const FNiagaraTypeDefinition& TypeDef)
	{
		return TypeDef.GetNameText().ToString();
	}

	static TSharedPtr<FJsonObject> NiagaraVariableToJson(const FNiagaraVariable& Variable, const FNiagaraParameterStore* Store = nullptr)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("name"), Variable.GetName().ToString());
		Object->SetStringField(TEXT("type"), TypeToString(Variable.GetType()));
		if (Store)
		{
			if (const uint8* Data = Store->GetParameterData(Variable))
			{
				Object->SetStringField(TEXT("value"), Variable.GetType().ToString(Data));
			}
		}
		return Object;
	}

	static TSharedPtr<FJsonObject> RendererToJson(const UNiagaraRendererProperties* Renderer, int32 Index)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("index"), Index);
		if (!Renderer)
		{
			Object->SetBoolField(TEXT("valid"), false);
			return Object;
		}

		Object->SetBoolField(TEXT("valid"), true);
		Object->SetStringField(TEXT("name"), Renderer->GetName());
		Object->SetStringField(TEXT("class"), Renderer->GetClass()->GetPathName());
		Object->SetBoolField(TEXT("enabled"), Renderer->GetIsEnabled());

		if (const UNiagaraSpriteRendererProperties* SpriteRenderer = Cast<UNiagaraSpriteRendererProperties>(Renderer))
		{
			Object->SetStringField(TEXT("type"), TEXT("sprite"));
			Object->SetStringField(TEXT("material"), SpriteRenderer->Material ? SpriteRenderer->Material->GetPathName() : FString());
		}
		else if (const UNiagaraMeshRendererProperties* MeshRenderer = Cast<UNiagaraMeshRendererProperties>(Renderer))
		{
			Object->SetStringField(TEXT("type"), TEXT("mesh"));
			TArray<TSharedPtr<FJsonValue>> Meshes;
			for (const FNiagaraMeshRendererMeshProperties& MeshInfo : MeshRenderer->Meshes)
			{
				TSharedPtr<FJsonObject> MeshObject = MakeShared<FJsonObject>();
				MeshObject->SetStringField(TEXT("mesh"), MeshInfo.Mesh ? MeshInfo.Mesh->GetPathName() : FString());
				Meshes.Add(MakeShared<FJsonValueObject>(MeshObject));
			}
			Object->SetArrayField(TEXT("meshes"), Meshes);
		}
		else if (const UNiagaraRibbonRendererProperties* RibbonRenderer = Cast<UNiagaraRibbonRendererProperties>(Renderer))
		{
			Object->SetStringField(TEXT("type"), TEXT("ribbon"));
			Object->SetStringField(TEXT("material"), RibbonRenderer->Material ? RibbonRenderer->Material->GetPathName() : FString());
		}
		else if (Renderer->IsA<UNiagaraLightRendererProperties>())
		{
			Object->SetStringField(TEXT("type"), TEXT("light"));
		}
		else
		{
			Object->SetStringField(TEXT("type"), TEXT("unknown"));
		}
		return Object;
	}

	static FString PinDirectionToString(EEdGraphPinDirection Direction)
	{
		return Direction == EGPD_Input ? TEXT("input") : TEXT("output");
	}

	static TSharedPtr<FJsonObject> PinToJson(const UEdGraphPin* Pin)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		if (!Pin)
		{
			return Object;
		}

		Object->SetStringField(TEXT("id"), Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
		Object->SetStringField(TEXT("name"), Pin->PinName.ToString());
		Object->SetStringField(TEXT("direction"), PinDirectionToString(Pin->Direction));
		Object->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);

		TArray<TSharedPtr<FJsonValue>> LinkedTo;
		for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (!LinkedPin || !LinkedPin->GetOwningNode())
			{
				continue;
			}

			TSharedPtr<FJsonObject> Link = MakeShared<FJsonObject>();
			Link->SetStringField(TEXT("nodeId"), LinkedPin->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			Link->SetStringField(TEXT("pinId"), LinkedPin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
			Link->SetStringField(TEXT("pinName"), LinkedPin->PinName.ToString());
			LinkedTo.Add(MakeShared<FJsonValueObject>(Link));
		}
		Object->SetArrayField(TEXT("linkedTo"), LinkedTo);
		return Object;
	}

	static TSharedPtr<FJsonObject> NodeToJson(const UEdGraphNode* Node)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		if (!Node)
		{
			return Object;
		}

		Object->SetStringField(TEXT("id"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Object->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
		Object->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		Object->SetNumberField(TEXT("x"), Node->NodePosX);
		Object->SetNumberField(TEXT("y"), Node->NodePosY);

		if (const UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node))
		{
			Object->SetStringField(TEXT("niagaraInputName"), InputNode->Input.GetName().ToString());
			Object->SetStringField(TEXT("niagaraInputType"), TypeToString(InputNode->Input.GetType()));
		}
		else if (const UNiagaraNodeOp* OpNode = Cast<UNiagaraNodeOp>(Node))
		{
			Object->SetStringField(TEXT("opName"), OpNode->OpName.ToString());
		}
		else if (const UNiagaraNodeAssignment* AssignmentNode = Cast<UNiagaraNodeAssignment>(Node))
		{
			TArray<TSharedPtr<FJsonValue>> Targets;
			for (const FNiagaraVariable& Target : AssignmentNode->GetAssignmentTargets())
			{
				Targets.Add(MakeShared<FJsonValueObject>(NiagaraVariableToJson(Target)));
			}
			Object->SetArrayField(TEXT("assignmentTargets"), Targets);
		}

		TArray<TSharedPtr<FJsonValue>> Pins;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			Pins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin)));
		}
		Object->SetArrayField(TEXT("pins"), Pins);
		return Object;
	}

	static TSharedPtr<FJsonObject> GraphToJson(const UNiagaraGraph* Graph, const FString& Name)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("name"), Name);
		if (!Graph)
		{
			Object->SetBoolField(TEXT("valid"), false);
			return Object;
		}

		Object->SetBoolField(TEXT("valid"), true);
		Object->SetStringField(TEXT("path"), Graph->GetPathName());

		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node)
			{
				Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Node)));
			}
		}
		Object->SetArrayField(TEXT("nodes"), Nodes);
		return Object;
	}

	static bool ParseType(const FString& RawType, FNiagaraTypeDefinition& OutType, FString& OutError)
	{
		FString Type = RawType.ToLower();
		Type.ReplaceInline(TEXT(" "), TEXT(""));
		Type.ReplaceInline(TEXT("_"), TEXT(""));

		if (Type == TEXT("float") || Type == TEXT("scalar"))
		{
			OutType = FNiagaraTypeDefinition::GetFloatDef();
			return true;
		}
		if (Type == TEXT("int") || Type == TEXT("int32") || Type == TEXT("integer"))
		{
			OutType = FNiagaraTypeDefinition::GetIntDef();
			return true;
		}
		if (Type == TEXT("bool") || Type == TEXT("boolean"))
		{
			OutType = FNiagaraTypeDefinition::GetBoolDef();
			return true;
		}
		if (Type == TEXT("vec2") || Type == TEXT("vector2") || Type == TEXT("vector2d"))
		{
			OutType = FNiagaraTypeDefinition::GetVec2Def();
			return true;
		}
		if (Type == TEXT("vec3") || Type == TEXT("vector") || Type == TEXT("vector3"))
		{
			OutType = FNiagaraTypeDefinition::GetVec3Def();
			return true;
		}
		if (Type == TEXT("vec4") || Type == TEXT("vector4"))
		{
			OutType = FNiagaraTypeDefinition::GetVec4Def();
			return true;
		}
		if (Type == TEXT("color") || Type == TEXT("linearcolor"))
		{
			OutType = FNiagaraTypeDefinition::GetColorDef();
			return true;
		}
		if (Type == TEXT("position"))
		{
			OutType = FNiagaraTypeDefinition::GetPositionDef();
			return true;
		}

		OutError = FString::Printf(TEXT("Unsupported Niagara type: %s."), *RawType);
		return false;
	}

	static FName NormalizeUserParameterName(const FString& Name)
	{
		return Name.StartsWith(TEXT("User.")) ? FName(*Name) : FName(*(TEXT("User.") + Name));
	}

	static double NumberAt(const TArray<TSharedPtr<FJsonValue>>& Values, int32 Index, double DefaultValue = 0.0)
	{
		return Values.IsValidIndex(Index) && Values[Index].IsValid() ? Values[Index]->AsNumber() : DefaultValue;
	}

	static bool SetStoreValueFromJson(FNiagaraUserRedirectionParameterStore& Store, const FNiagaraVariable& Variable, const TSharedPtr<FJsonValue>& Value, FString& OutError)
	{
		const FNiagaraTypeDefinition& Type = Variable.GetType();
		if (!Value.IsValid())
		{
			return true;
		}

		if (Type == FNiagaraTypeDefinition::GetFloatDef())
		{
			Store.SetParameterValue<float>(static_cast<float>(Value->AsNumber()), Variable, true);
			return true;
		}
		if (Type == FNiagaraTypeDefinition::GetIntDef())
		{
			Store.SetParameterValue<int32>(static_cast<int32>(Value->AsNumber()), Variable, true);
			return true;
		}
		if (Type == FNiagaraTypeDefinition::GetBoolDef())
		{
			Store.SetParameterValue<FNiagaraBool>(FNiagaraBool(Value->AsBool()), Variable, true);
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* ArrayValue = nullptr;
		if (!Value->TryGetArray(ArrayValue) || !ArrayValue)
		{
			OutError = FString::Printf(TEXT("Niagara type %s expects params.value as an array."), *TypeToString(Type));
			return false;
		}

		if (Type == FNiagaraTypeDefinition::GetVec2Def())
		{
			Store.SetParameterValue<FVector2f>(FVector2f(static_cast<float>(NumberAt(*ArrayValue, 0)), static_cast<float>(NumberAt(*ArrayValue, 1))), Variable, true);
			return true;
		}
		if (Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef())
		{
			Store.SetParameterValue<FVector3f>(FVector3f(static_cast<float>(NumberAt(*ArrayValue, 0)), static_cast<float>(NumberAt(*ArrayValue, 1)), static_cast<float>(NumberAt(*ArrayValue, 2))), Variable, true);
			return true;
		}
		if (Type == FNiagaraTypeDefinition::GetVec4Def())
		{
			Store.SetParameterValue<FVector4f>(FVector4f(static_cast<float>(NumberAt(*ArrayValue, 0)), static_cast<float>(NumberAt(*ArrayValue, 1)), static_cast<float>(NumberAt(*ArrayValue, 2)), static_cast<float>(NumberAt(*ArrayValue, 3))), Variable, true);
			return true;
		}
		if (Type == FNiagaraTypeDefinition::GetColorDef())
		{
			Store.SetParameterValue<FLinearColor>(FLinearColor(static_cast<float>(NumberAt(*ArrayValue, 0)), static_cast<float>(NumberAt(*ArrayValue, 1)), static_cast<float>(NumberAt(*ArrayValue, 2)), static_cast<float>(NumberAt(*ArrayValue, 3, 1.0))), Variable, true);
			return true;
		}

		OutError = FString::Printf(TEXT("Setting value for Niagara type %s is not implemented."), *TypeToString(Type));
		return false;
	}

	static bool ParseUsage(const FString& RawUsage, ENiagaraScriptUsage& OutUsage, FString& OutError)
	{
		FString Usage = RawUsage.ToLower();
		Usage.ReplaceInline(TEXT("_"), TEXT(""));
		Usage.ReplaceInline(TEXT(" "), TEXT(""));

		if (Usage == TEXT("systemspawn"))
		{
			OutUsage = ENiagaraScriptUsage::SystemSpawnScript;
			return true;
		}
		if (Usage == TEXT("systemupdate"))
		{
			OutUsage = ENiagaraScriptUsage::SystemUpdateScript;
			return true;
		}
		if (Usage == TEXT("emitterspawn"))
		{
			OutUsage = ENiagaraScriptUsage::EmitterSpawnScript;
			return true;
		}
		if (Usage == TEXT("emitterupdate"))
		{
			OutUsage = ENiagaraScriptUsage::EmitterUpdateScript;
			return true;
		}
		if (Usage == TEXT("particlespawn"))
		{
			OutUsage = ENiagaraScriptUsage::ParticleSpawnScript;
			return true;
		}
		if (Usage == TEXT("particleupdate"))
		{
			OutUsage = ENiagaraScriptUsage::ParticleUpdateScript;
			return true;
		}
		if (Usage == TEXT("particlegpu") || Usage == TEXT("gpuparticlespawn") || Usage == TEXT("gpuparticleupdate"))
		{
			OutUsage = ENiagaraScriptUsage::ParticleGPUComputeScript;
			return true;
		}
		if (Usage == TEXT("module"))
		{
			OutUsage = ENiagaraScriptUsage::Module;
			return true;
		}
		if (Usage == TEXT("function"))
		{
			OutUsage = ENiagaraScriptUsage::Function;
			return true;
		}
		if (Usage == TEXT("dynamicinput"))
		{
			OutUsage = ENiagaraScriptUsage::DynamicInput;
			return true;
		}

		OutError = FString::Printf(TEXT("Unsupported Niagara script usage: %s."), *RawUsage);
		return false;
	}

	static UNiagaraSystem* LoadSystem(const FString& AssetPath, FString& OutError)
	{
		const FString NormalizedPath = NormalizeAssetObjectPath(AssetPath);
		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizedPath);
		if (!System)
		{
			OutError = FString::Printf(TEXT("NiagaraSystem not found: %s."), *NormalizedPath);
		}
		return System;
	}

	static UNiagaraEmitter* LoadEmitter(const FString& AssetPath, FString& OutError)
	{
		const FString NormalizedPath = NormalizeAssetObjectPath(AssetPath);
		UNiagaraEmitter* Emitter = LoadObject<UNiagaraEmitter>(nullptr, *NormalizedPath);
		if (!Emitter)
		{
			OutError = FString::Printf(TEXT("NiagaraEmitter not found: %s."), *NormalizedPath);
		}
		return Emitter;
	}

	static FNiagaraEmitterHandle* FindEmitterHandle(UNiagaraSystem* System, const FString& EmitterNameOrId)
	{
		if (!System)
		{
			return nullptr;
		}

		FGuid EmitterId;
		const bool bHasGuid = FGuid::Parse(EmitterNameOrId, EmitterId);
		for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			if (Handle.GetName().ToString() == EmitterNameOrId || (bHasGuid && Handle.GetId() == EmitterId))
			{
				return &Handle;
			}
		}
		return nullptr;
	}

	static UNiagaraScriptSource* GetScriptSourceFromSystem(UNiagaraSystem* System, ENiagaraScriptUsage Usage)
	{
		if (!System)
		{
			return nullptr;
		}

		UNiagaraScript* Script = nullptr;
		if (Usage == ENiagaraScriptUsage::SystemSpawnScript)
		{
			Script = System->GetSystemSpawnScript();
		}
		else if (Usage == ENiagaraScriptUsage::SystemUpdateScript)
		{
			Script = System->GetSystemUpdateScript();
		}

		return Script ? Cast<UNiagaraScriptSource>(Script->GetSource(FGuid())) : nullptr;
	}

	static UNiagaraScriptSource* GetScriptSourceFromEmitter(UNiagaraEmitter* Emitter)
	{
		if (!Emitter)
		{
			return nullptr;
		}

		const FGuid VersionGuid = Emitter->GetExposedVersion().VersionGuid;
		FVersionedNiagaraEmitterData* Data = Emitter->GetEmitterData(VersionGuid);
		return Data ? Cast<UNiagaraScriptSource>(Data->GraphSource) : nullptr;
	}

	static UNiagaraScriptSource* ResolveScriptSource(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, UObject*& OutAsset, FString& OutError)
	{
		FString AssetPath;
		if (!GetStringParam(Params, TEXT("asset"), AssetPath, OutError))
		{
			return nullptr;
		}

		OutAssetPath = AssetPath;
		OutAsset = nullptr;

		FString UsageString;
		ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("usage"), UsageString) && !UsageString.IsEmpty())
		{
			if (!ParseUsage(UsageString, Usage, OutError))
			{
				return nullptr;
			}
		}

		if (UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *AssetPath))
		{
			OutAsset = System;
			if (Usage == ENiagaraScriptUsage::SystemSpawnScript || Usage == ENiagaraScriptUsage::SystemUpdateScript)
			{
				return GetScriptSourceFromSystem(System, Usage);
			}

			FString EmitterName;
			Params->TryGetStringField(TEXT("emitter"), EmitterName);
			if (EmitterName.IsEmpty())
			{
				OutError = TEXT("Missing params.emitter for emitter graph access on a NiagaraSystem.");
				return nullptr;
			}

			FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
			if (!Handle)
			{
				OutError = FString::Printf(TEXT("Emitter handle not found: %s."), *EmitterName);
				return nullptr;
			}

			FVersionedNiagaraEmitterData* Data = Handle->GetEmitterData();
			return Data ? Cast<UNiagaraScriptSource>(Data->GraphSource) : nullptr;
		}

		if (UNiagaraEmitter* Emitter = LoadObject<UNiagaraEmitter>(nullptr, *AssetPath))
		{
			OutAsset = Emitter;
			return GetScriptSourceFromEmitter(Emitter);
		}

		OutError = FString::Printf(TEXT("Niagara asset not found: %s."), *AssetPath);
		return nullptr;
	}

	static UNiagaraGraph* ResolveGraph(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, UObject*& OutAsset, FString& OutError)
	{
		UNiagaraScriptSource* Source = ResolveScriptSource(Params, OutAssetPath, OutAsset, OutError);
		if (!Source || !Source->NodeGraph)
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("Niagara script source graph not found.");
			}
			return nullptr;
		}
		return Source->NodeGraph;
	}

	static FVersionedNiagaraEmitterData* ResolveEmitterData(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, UObject*& OutAsset, UNiagaraEmitter*& OutEmitter, UNiagaraSystem*& OutSystem, FString& OutEmitterName, FString& OutError)
	{
		FString AssetPath;
		if (!GetStringParam(Params, TEXT("asset"), AssetPath, OutError))
		{
			return nullptr;
		}

		OutAssetPath = AssetPath;
		OutAsset = nullptr;
		OutEmitter = nullptr;
		OutSystem = nullptr;
		OutEmitterName.Empty();

		if (UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *AssetPath))
		{
			OutAsset = System;
			OutSystem = System;
			FString EmitterName;
			Params->TryGetStringField(TEXT("emitter"), EmitterName);
			if (EmitterName.IsEmpty())
			{
				OutError = TEXT("Missing params.emitter for NiagaraSystem emitter access.");
				return nullptr;
			}

			FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
			if (!Handle)
			{
				OutError = FString::Printf(TEXT("Emitter handle not found: %s."), *EmitterName);
				return nullptr;
			}
			OutEmitterName = Handle->GetName().ToString();
			OutEmitter = Handle->GetInstance().Emitter;
			return Handle->GetEmitterData();
		}

		if (UNiagaraEmitter* Emitter = LoadObject<UNiagaraEmitter>(nullptr, *AssetPath))
		{
			OutAsset = Emitter;
			OutEmitter = Emitter;
			OutEmitterName = Emitter->GetUniqueEmitterName();
			return Emitter->GetLatestEmitterData();
		}

		OutError = FString::Printf(TEXT("Niagara asset not found: %s."), *AssetPath);
		return nullptr;
	}

	static UNiagaraRendererProperties* FindRendererByIndexOrName(FVersionedNiagaraEmitterData* EmitterData, const FString& RendererId, int32 RendererIndex)
	{
		if (!EmitterData)
		{
			return nullptr;
		}

		const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
		if (Renderers.IsValidIndex(RendererIndex))
		{
			return Renderers[RendererIndex];
		}
		for (UNiagaraRendererProperties* Renderer : Renderers)
		{
			if (Renderer && (Renderer->GetName() == RendererId || Renderer->GetClass()->GetName() == RendererId || Renderer->GetClass()->GetPathName() == RendererId))
			{
				return Renderer;
			}
		}
		return nullptr;
	}

	static bool ParseRendererType(const FString& RawType, UClass*& OutRendererClass, FString& OutNormalizedType, FString& OutError)
	{
		FString Type = RawType.ToLower();
		Type.ReplaceInline(TEXT("-"), TEXT("_"));
		Type.ReplaceInline(TEXT(" "), TEXT("_"));

		if (Type.IsEmpty() || Type == TEXT("sprite"))
		{
			OutRendererClass = UNiagaraSpriteRendererProperties::StaticClass();
			OutNormalizedType = TEXT("sprite");
			return true;
		}
		if (Type == TEXT("mesh") || Type == TEXT("static_mesh"))
		{
			OutRendererClass = UNiagaraMeshRendererProperties::StaticClass();
			OutNormalizedType = TEXT("mesh");
			return true;
		}
		if (Type == TEXT("ribbon"))
		{
			OutRendererClass = UNiagaraRibbonRendererProperties::StaticClass();
			OutNormalizedType = TEXT("ribbon");
			return true;
		}
		if (Type == TEXT("light"))
		{
			OutRendererClass = UNiagaraLightRendererProperties::StaticClass();
			OutNormalizedType = TEXT("light");
			return true;
		}

		OutError = FString::Printf(TEXT("Unsupported Niagara renderer type: %s."), *RawType);
		return false;
	}

	static void ApplyRendererAssetReferences(UNiagaraRendererProperties* Renderer, const FString& MaterialPath, const FString& MeshPath)
	{
		if (!Renderer)
		{
			return;
		}

		UMaterialInterface* Material = MaterialPath.IsEmpty() ? nullptr : LoadObject<UMaterialInterface>(nullptr, *NormalizeAssetObjectPath(MaterialPath));
		if (UNiagaraSpriteRendererProperties* SpriteRenderer = Cast<UNiagaraSpriteRendererProperties>(Renderer))
		{
			if (Material)
			{
				SpriteRenderer->Material = Material;
			}
		}
		else if (UNiagaraRibbonRendererProperties* RibbonRenderer = Cast<UNiagaraRibbonRendererProperties>(Renderer))
		{
			if (Material)
			{
				RibbonRenderer->Material = Material;
			}
		}
		else if (UNiagaraMeshRendererProperties* MeshRenderer = Cast<UNiagaraMeshRendererProperties>(Renderer))
		{
			if (UStaticMesh* Mesh = MeshPath.IsEmpty() ? nullptr : LoadObject<UStaticMesh>(nullptr, *NormalizeAssetObjectPath(MeshPath)))
			{
				MeshRenderer->Meshes.Empty();
				FNiagaraMeshRendererMeshProperties MeshInfo;
				MeshInfo.Mesh = Mesh;
				MeshInfo.Scale = FVector::OneVector;
				MeshRenderer->Meshes.Add(MeshInfo);
			}

			if (Material)
			{
				MeshRenderer->bOverrideMaterials = true;
				MeshRenderer->OverrideMaterials.Empty();
				FNiagaraMeshMaterialOverride MaterialOverride;
				MaterialOverride.ExplicitMat = Material;
				MeshRenderer->OverrideMaterials.Add(MaterialOverride);
			}
		}
	}

	static UNiagaraRendererProperties* AddRendererToEmitter(UNiagaraEmitter* Emitter, FVersionedNiagaraEmitterData* EmitterData, const FString& Type, const FString& Name, const FString& MaterialPath, const FString& MeshPath, FString& OutNormalizedType, FString& OutError)
	{
		if (!Emitter || !EmitterData)
		{
			OutError = TEXT("Missing Niagara emitter data.");
			return nullptr;
		}

		UClass* RendererClass = nullptr;
		if (!ParseRendererType(Type, RendererClass, OutNormalizedType, OutError))
		{
			return nullptr;
		}

		const FString BaseName = Name.IsEmpty() ? FString::Printf(TEXT("Mcp%sRenderer"), *OutNormalizedType) : Name;
		const FName UniqueName = MakeUniqueObjectName(Emitter, RendererClass, FName(*BaseName));
		UNiagaraRendererProperties* Renderer = NewObject<UNiagaraRendererProperties>(Emitter, RendererClass, UniqueName, RF_Transactional);
		if (!Renderer)
		{
			OutError = FString::Printf(TEXT("Failed to create Niagara renderer: %s."), *RendererClass->GetName());
			return nullptr;
		}

		ApplyRendererAssetReferences(Renderer, MaterialPath, MeshPath);
		Emitter->AddRenderer(Renderer, EmitterData->Version.VersionGuid);
		return Renderer;
	}

	static bool AddModuleToEmitterDataIfMissing(FVersionedNiagaraEmitterData* EmitterData, const FString& ModulePath, ENiagaraScriptUsage Usage)
	{
		if (!EmitterData || ModulePath.IsEmpty())
		{
			return false;
		}

		UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
		if (!Source)
		{
			return false;
		}

		bool bFoundModule = false;
		return Source->AddModuleIfMissing(ModulePath, Usage, bFoundModule);
	}

	static FNiagaraEmitterHandle* AddDefaultEmitterToSystem(UNiagaraSystem* System, const FString& EmitterName)
	{
		if (!System)
		{
			return nullptr;
		}

		UNiagaraEmitter* SourceEmitter = NewObject<UNiagaraEmitter>(GetTransientPackage(), FName(*EmitterName), RF_Transactional);
		if (!SourceEmitter)
		{
			return nullptr;
		}

		UNiagaraEmitterFactoryNew::InitializeEmitter(SourceEmitter, true);
		SourceEmitter->SetUniqueEmitterName(EmitterName);
		SourceEmitter->bIsInheritable = false;
		const FGuid VersionGuid = SourceEmitter->GetExposedVersion().VersionGuid;
		const FNiagaraEmitterHandle AddedHandle = System->AddEmitterHandle(*SourceEmitter, FName(*EmitterName), VersionGuid);
		for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			if (Handle.GetId() == AddedHandle.GetId())
			{
				return &Handle;
			}
		}
		return nullptr;
	}

	static void AddCommonPresetUserParameters(UNiagaraSystem* System)
	{
		if (!System)
		{
			return;
		}

		auto AddParameter = [System](const FString& Name, const FNiagaraTypeDefinition& Type, const TSharedPtr<FJsonValue>& Value)
		{
			FNiagaraVariable Variable(Type, NormalizeUserParameterName(Name));
			FNiagaraUserRedirectionParameterStore::MakeUserVariable(Variable);
			FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
			Store.AddParameter(Variable, true, true);
			FString IgnoredError;
			SetStoreValueFromJson(Store, Variable, Value, IgnoredError);
		};

		AddParameter(TEXT("EffectScale"), FNiagaraTypeDefinition::GetFloatDef(), MakeShared<FJsonValueNumber>(1.0));
		AddParameter(TEXT("Intensity"), FNiagaraTypeDefinition::GetFloatDef(), MakeShared<FJsonValueNumber>(1.0));
	}

	static UEdGraphNode* FindNodeByIdOrTitle(UEdGraph* Graph, const FString& NodeIdOrTitle)
	{
		if (!Graph)
		{
			return nullptr;
		}

		FGuid NodeGuid;
		const bool bHasGuid = FGuid::Parse(NodeIdOrTitle, NodeGuid);
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			if ((bHasGuid && Node->NodeGuid == NodeGuid) || Node->GetNodeTitle(ENodeTitleType::ListView).ToString() == NodeIdOrTitle || Node->GetName() == NodeIdOrTitle)
			{
				return Node;
			}
		}
		return nullptr;
	}

	static UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinNameOrId, EEdGraphPinDirection Direction)
	{
		if (!Node)
		{
			return nullptr;
		}

		FGuid PinGuid;
		const bool bHasGuid = FGuid::Parse(PinNameOrId, PinGuid);
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != Direction)
			{
				continue;
			}
			if ((bHasGuid && Pin->PinId == PinGuid) || Pin->PinName.ToString() == PinNameOrId)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static UEdGraphPin* FindPinAnyDirection(UEdGraphNode* Node, const FString& PinNameOrId)
	{
		if (!Node)
		{
			return nullptr;
		}

		FGuid PinGuid;
		const bool bHasGuid = FGuid::Parse(PinNameOrId, PinGuid);
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}
			if ((bHasGuid && Pin->PinId == PinGuid) || Pin->PinName.ToString() == PinNameOrId)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static UClass* ResolveNodeClass(const FString& RawClassOrAlias)
	{
		FString Alias = RawClassOrAlias.ToLower();
		Alias.ReplaceInline(TEXT("-"), TEXT("_"));
		Alias.ReplaceInline(TEXT(" "), TEXT("_"));

		if (Alias.IsEmpty() || Alias == TEXT("function") || Alias == TEXT("function_call") || Alias == TEXT("functioncall"))
		{
			return UNiagaraNodeFunctionCall::StaticClass();
		}
		if (Alias == TEXT("input"))
		{
			return UNiagaraNodeInput::StaticClass();
		}
		if (Alias == TEXT("output"))
		{
			return UNiagaraNodeOutput::StaticClass();
		}
		if (Alias == TEXT("op") || Alias == TEXT("operator"))
		{
			return UNiagaraNodeOp::StaticClass();
		}
		if (Alias == TEXT("assignment") || Alias == TEXT("assign") || Alias == TEXT("set"))
		{
			return UNiagaraNodeAssignment::StaticClass();
		}

		UClass* LoadedClass = LoadClass<UEdGraphNode>(nullptr, *RawClassOrAlias);
		if (!LoadedClass)
		{
			LoadedClass = FindObject<UClass>(nullptr, *RawClassOrAlias);
		}
		if (LoadedClass && LoadedClass->IsChildOf(UNiagaraNode::StaticClass()))
		{
			return LoadedClass;
		}
		return nullptr;
	}

	static bool SetObjectPropertyFromString(UObject* Object, const FString& PropertyName, const FString& Value, FString& OutOldValue, FString& OutError)
	{
		if (!Object)
		{
			OutError = TEXT("Missing target object.");
			return false;
		}

		FProperty* Property = Object->GetClass()->FindPropertyByName(*PropertyName);
		if (!Property)
		{
			OutError = FString::Printf(TEXT("Property not found on %s: %s."), *Object->GetClass()->GetName(), *PropertyName);
			return false;
		}

		void* PropertyValue = Property->ContainerPtrToValuePtr<void>(Object);
		Property->ExportText_Direct(OutOldValue, PropertyValue, PropertyValue, Object, PPF_None);
		const TCHAR* ImportResult = Property->ImportText_Direct(*Value, PropertyValue, Object, PPF_None);
		if (!ImportResult)
		{
			OutError = FString::Printf(TEXT("Failed to import value for property %s: %s."), *PropertyName, *Value);
			return false;
		}
		return true;
	}

	class FWebFrameMcpNiagaraModule : public IModuleInterface
	{
	public:
		virtual void StartupModule() override
		{
			FWebFrameMcpModule& Core = FModuleManager::LoadModuleChecked<FWebFrameMcpModule>(TEXT("WebFrameMcp"));

			Core.RegisterCommand({ OwnerName, TEXT("niagara.system.list"), TEXT("List NiagaraSystem assets."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.ListAssets(FTopLevelAssetPath(TEXT("/Script/Niagara"), TEXT("NiagaraSystem")), Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.emitter.list"), TEXT("List NiagaraEmitter assets."), [&Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Core.ListAssets(FTopLevelAssetPath(TEXT("/Script/Niagara"), TEXT("NiagaraEmitter")), Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.system.create"), TEXT("Create a NiagaraSystem asset."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return CreateSystem(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.emitter.create"), TEXT("Create a NiagaraEmitter asset."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return CreateEmitter(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.describe"), TEXT("Describe a NiagaraSystem or NiagaraEmitter asset."), [this](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return Describe(Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.system.add_emitter"), TEXT("Add an emitter asset to a NiagaraSystem."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return AddEmitterToSystem(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.system.remove_emitter"), TEXT("Remove an emitter handle from a NiagaraSystem."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return RemoveEmitterFromSystem(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.emitter.set_enabled"), TEXT("Enable or disable a NiagaraSystem emitter handle."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return SetEmitterEnabled(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.user_parameter.add"), TEXT("Add a NiagaraSystem user parameter."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return AddUserParameter(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.user_parameter.remove"), TEXT("Remove a NiagaraSystem user parameter."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return RemoveUserParameter(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.user_parameter.set"), TEXT("Set a NiagaraSystem user parameter value."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return SetUserParameter(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.emitter.set_property"), TEXT("Set a Niagara emitter data property."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return SetEmitterProperty(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.renderer.list"), TEXT("List Niagara emitter renderers."), [this](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return ListRenderers(Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.renderer.add"), TEXT("Add a Niagara renderer to an emitter."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return AddRenderer(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.renderer.remove"), TEXT("Remove a Niagara renderer from an emitter."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return RemoveRenderer(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.renderer.set_property"), TEXT("Set a Niagara renderer property."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return SetRendererProperty(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.module.add_if_missing"), TEXT("Add a Niagara module script to a usage stack if absent."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return AddModuleIfMissing(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.graph.node.add"), TEXT("Add a node to a Niagara graph."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return AddGraphNode(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.graph.node.set_property"), TEXT("Set a property on a Niagara graph node."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return SetGraphNodeProperty(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.graph.node.move"), TEXT("Move a Niagara graph node."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return MoveGraphNode(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.graph.node.delete"), TEXT("Delete a Niagara graph node."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return DeleteGraphNode(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.graph.pin.set_default"), TEXT("Set a Niagara graph pin default value."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return SetGraphPinDefault(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.graph.node.connect"), TEXT("Connect two Niagara graph node pins."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return ConnectGraphPins(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.graph.pin.disconnect"), TEXT("Disconnect Niagara graph node pins."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return DisconnectGraphPins(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.graph.node.disconnect"), TEXT("Disconnect all links on a Niagara graph node or one of its pins."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return DisconnectGraphPins(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.compile"), TEXT("Request compile on a NiagaraSystem."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return CompileSystem(Core, Params, OutError); } });
			Core.RegisterCommand({ OwnerName, TEXT("niagara.preset.create"), TEXT("Create a practical Niagara effect preset asset."), [this, &Core](const TSharedPtr<FJsonObject>& Params, FString& OutError) { return CreatePreset(Core, Params, OutError); } });
		}

		virtual void ShutdownModule() override
		{
			if (FWebFrameMcpModule* Core = FModuleManager::GetModulePtr<FWebFrameMcpModule>(TEXT("WebFrameMcp")))
			{
				Core->UnregisterCommandsByOwner(OwnerName);
			}
		}

	private:
		TSharedPtr<FJsonObject> CreateSystem(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString PackagePath;
			FString AssetName;
			if (!GetStringParam(Params, TEXT("path"), PackagePath, OutError) || !GetStringParam(Params, TEXT("name"), AssetName, OutError))
			{
				return nullptr;
			}

			const FString PackageName = PackagePath / AssetName;
			UPackage* Package = CreatePackage(*PackageName);
			UNiagaraSystem* System = NewObject<UNiagaraSystem>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
			if (!System)
			{
				OutError = FString::Printf(TEXT("Failed to create NiagaraSystem: %s/%s."), *PackagePath, *AssetName);
				return nullptr;
			}

			const bool bCreateDefaultNodes = ReadBoolParam(Params, TEXT("createDefaultNodes"), true);
			UNiagaraSystemFactoryNew::InitializeSystem(System, bCreateDefaultNodes);
			NotifyAssetCreated(System);
			SaveAssetIfRequested(System, ReadBoolParam(Params, TEXT("save")));

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), System->GetPathName());
			Result->SetBoolField(TEXT("created"), true);
			RecordOperation(Core, System->GetPathName(), TEXT("Create Niagara system"), AssetName, TEXT("Created NiagaraSystem asset."));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> CreateEmitter(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString PackagePath;
			FString AssetName;
			if (!GetStringParam(Params, TEXT("path"), PackagePath, OutError) || !GetStringParam(Params, TEXT("name"), AssetName, OutError))
			{
				return nullptr;
			}

			const FString PackageName = PackagePath / AssetName;
			UPackage* Package = CreatePackage(*PackageName);
			UNiagaraEmitter* Emitter = NewObject<UNiagaraEmitter>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
			if (!Emitter)
			{
				OutError = FString::Printf(TEXT("Failed to create NiagaraEmitter: %s/%s."), *PackagePath, *AssetName);
				return nullptr;
			}

			const bool bAddDefaults = ReadBoolParam(Params, TEXT("addDefaultModulesAndRenderers"), true);
			UNiagaraEmitterFactoryNew::InitializeEmitter(Emitter, bAddDefaults);
			NotifyAssetCreated(Emitter);
			SaveAssetIfRequested(Emitter, ReadBoolParam(Params, TEXT("save")));

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), Emitter->GetPathName());
			Result->SetBoolField(TEXT("created"), true);
			RecordOperation(Core, Emitter->GetPathName(), TEXT("Create Niagara emitter"), AssetName, TEXT("Created NiagaraEmitter asset."));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> Describe(const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString AssetPath;
			if (!GetStringParam(Params, TEXT("asset"), AssetPath, OutError))
			{
				return nullptr;
			}

			if (UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *AssetPath))
			{
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("asset"), System->GetPathName());
				Result->SetStringField(TEXT("type"), TEXT("NiagaraSystem"));

				TArray<TSharedPtr<FJsonValue>> UserParameters;
				TArray<FNiagaraVariable> Parameters;
				System->GetExposedParameters().GetUserParameters(Parameters);
				for (const FNiagaraVariable& Parameter : Parameters)
				{
					UserParameters.Add(MakeShared<FJsonValueObject>(NiagaraVariableToJson(Parameter, &System->GetExposedParameters())));
				}
				Result->SetArrayField(TEXT("userParameters"), UserParameters);

				TArray<TSharedPtr<FJsonValue>> Emitters;
				for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
				{
					TSharedPtr<FJsonObject> EmitterJson = MakeShared<FJsonObject>();
					EmitterJson->SetStringField(TEXT("id"), Handle.GetId().ToString(EGuidFormats::DigitsWithHyphens));
					EmitterJson->SetStringField(TEXT("name"), Handle.GetName().ToString());
					EmitterJson->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
					if (FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
					{
						EmitterJson->SetStringField(TEXT("simTarget"), UEnum::GetValueAsString(Data->SimTarget));
						EmitterJson->SetBoolField(TEXT("localSpace"), Data->bLocalSpace);
						TArray<TSharedPtr<FJsonValue>> Renderers;
						int32 RendererIndex = 0;
						for (const UNiagaraRendererProperties* Renderer : Data->GetRenderers())
						{
							Renderers.Add(MakeShared<FJsonValueObject>(RendererToJson(Renderer, RendererIndex++)));
						}
						EmitterJson->SetArrayField(TEXT("renderers"), Renderers);
						if (UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Data->GraphSource))
						{
							EmitterJson->SetObjectField(TEXT("graph"), GraphToJson(Source->NodeGraph, TEXT("emitterGraph")));
						}
					}
					Emitters.Add(MakeShared<FJsonValueObject>(EmitterJson));
				}
				Result->SetArrayField(TEXT("emitters"), Emitters);

				TArray<TSharedPtr<FJsonValue>> Graphs;
				if (UNiagaraScriptSource* SpawnSource = GetScriptSourceFromSystem(System, ENiagaraScriptUsage::SystemSpawnScript))
				{
					Graphs.Add(MakeShared<FJsonValueObject>(GraphToJson(SpawnSource->NodeGraph, TEXT("systemSpawn"))));
				}
				if (UNiagaraScriptSource* UpdateSource = GetScriptSourceFromSystem(System, ENiagaraScriptUsage::SystemUpdateScript))
				{
					Graphs.Add(MakeShared<FJsonValueObject>(GraphToJson(UpdateSource->NodeGraph, TEXT("systemUpdate"))));
				}
				Result->SetArrayField(TEXT("graphs"), Graphs);
				return Result;
			}

			if (UNiagaraEmitter* Emitter = LoadObject<UNiagaraEmitter>(nullptr, *AssetPath))
			{
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("asset"), Emitter->GetPathName());
				Result->SetStringField(TEXT("type"), TEXT("NiagaraEmitter"));
				Result->SetStringField(TEXT("versionGuid"), Emitter->GetExposedVersion().VersionGuid.ToString(EGuidFormats::DigitsWithHyphens));

				if (UNiagaraScriptSource* Source = GetScriptSourceFromEmitter(Emitter))
				{
					Result->SetObjectField(TEXT("graph"), GraphToJson(Source->NodeGraph, TEXT("emitterGraph")));
				}
				if (FVersionedNiagaraEmitterData* Data = Emitter->GetLatestEmitterData())
				{
					Result->SetStringField(TEXT("simTarget"), UEnum::GetValueAsString(Data->SimTarget));
					Result->SetBoolField(TEXT("localSpace"), Data->bLocalSpace);
					TArray<TSharedPtr<FJsonValue>> Renderers;
					int32 RendererIndex = 0;
					for (const UNiagaraRendererProperties* Renderer : Data->GetRenderers())
					{
						Renderers.Add(MakeShared<FJsonValueObject>(RendererToJson(Renderer, RendererIndex++)));
					}
					Result->SetArrayField(TEXT("renderers"), Renderers);
				}
				return Result;
			}

			OutError = FString::Printf(TEXT("Niagara asset not found: %s."), *AssetPath);
			return nullptr;
		}

		TSharedPtr<FJsonObject> AddEmitterToSystem(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString SystemPath;
			FString EmitterPath;
			if (!GetStringParam(Params, TEXT("asset"), SystemPath, OutError) || !GetStringParam(Params, TEXT("emitterAsset"), EmitterPath, OutError))
			{
				return nullptr;
			}

			UNiagaraSystem* System = LoadSystem(SystemPath, OutError);
			if (!System)
			{
				return nullptr;
			}
			UNiagaraEmitter* Emitter = LoadEmitter(EmitterPath, OutError);
			if (!Emitter)
			{
				return nullptr;
			}

			FString EmitterName;
			Params->TryGetStringField(TEXT("name"), EmitterName);
			if (EmitterName.IsEmpty())
			{
				EmitterName = Emitter->GetName();
			}

			System->Modify();
			const FGuid VersionGuid = Emitter->GetExposedVersion().VersionGuid;
			const FNiagaraEmitterHandle Handle = System->AddEmitterHandle(*Emitter, FName(*EmitterName), VersionGuid);
			System->RequestCompile(true);
			SaveAssetIfRequested(System, ReadBoolParam(Params, TEXT("save")));

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), System->GetPathName());
			Result->SetStringField(TEXT("emitter"), EmitterPath);
			Result->SetStringField(TEXT("handleId"), Handle.GetId().ToString(EGuidFormats::DigitsWithHyphens));
			Result->SetStringField(TEXT("handleName"), Handle.GetName().ToString());
			RecordOperation(Core, System->GetPathName(), TEXT("Add Niagara emitter"), EmitterName, FString::Printf(TEXT("Added emitter asset %s."), *EmitterPath));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> RemoveEmitterFromSystem(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString SystemPath;
			FString EmitterName;
			if (!GetStringParam(Params, TEXT("asset"), SystemPath, OutError) || !GetStringParam(Params, TEXT("emitter"), EmitterName, OutError))
			{
				return nullptr;
			}

			UNiagaraSystem* System = LoadSystem(SystemPath, OutError);
			if (!System)
			{
				return nullptr;
			}

			FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
			if (!Handle)
			{
				OutError = FString::Printf(TEXT("Emitter handle not found: %s."), *EmitterName);
				return nullptr;
			}

			const FString RemovedName = Handle->GetName().ToString();
			const FGuid RemovedId = Handle->GetId();
			System->Modify();
			System->RemoveEmitterHandle(*Handle);
			System->RequestCompile(true);
			SaveAssetIfRequested(System, ReadBoolParam(Params, TEXT("save")));

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), System->GetPathName());
			Result->SetStringField(TEXT("removedEmitter"), RemovedName);
			Result->SetStringField(TEXT("removedId"), RemovedId.ToString(EGuidFormats::DigitsWithHyphens));
			RecordOperation(Core, System->GetPathName(), TEXT("Remove Niagara emitter"), RemovedName, TEXT("Removed emitter handle from NiagaraSystem."));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> SetEmitterEnabled(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString SystemPath;
			FString EmitterName;
			if (!GetStringParam(Params, TEXT("asset"), SystemPath, OutError) || !GetStringParam(Params, TEXT("emitter"), EmitterName, OutError))
			{
				return nullptr;
			}

			UNiagaraSystem* System = LoadSystem(SystemPath, OutError);
			if (!System)
			{
				return nullptr;
			}

			FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
			if (!Handle)
			{
				OutError = FString::Printf(TEXT("Emitter handle not found: %s."), *EmitterName);
				return nullptr;
			}

			const bool bEnabled = ReadBoolParam(Params, TEXT("enabled"), true);
			System->Modify();
			const bool bChanged = Handle->SetIsEnabled(bEnabled, *System, true);
			SaveAssetIfRequested(System, ReadBoolParam(Params, TEXT("save")));

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), System->GetPathName());
			Result->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
			Result->SetBoolField(TEXT("enabled"), Handle->GetIsEnabled());
			Result->SetBoolField(TEXT("changed"), bChanged);
			RecordOperation(Core, System->GetPathName(), TEXT("Set Niagara emitter enabled"), Handle->GetName().ToString(), bEnabled ? TEXT("Enabled emitter handle.") : TEXT("Disabled emitter handle."));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> AddUserParameter(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString SystemPath;
			FString ParamName;
			FString TypeString;
			if (!GetStringParam(Params, TEXT("asset"), SystemPath, OutError) || !GetStringParam(Params, TEXT("name"), ParamName, OutError) || !GetStringParam(Params, TEXT("type"), TypeString, OutError))
			{
				return nullptr;
			}

			FNiagaraTypeDefinition TypeDef;
			if (!ParseType(TypeString, TypeDef, OutError))
			{
				return nullptr;
			}

			UNiagaraSystem* System = LoadSystem(SystemPath, OutError);
			if (!System)
			{
				return nullptr;
			}

			FNiagaraVariable Variable(TypeDef, NormalizeUserParameterName(ParamName));
			FNiagaraUserRedirectionParameterStore::MakeUserVariable(Variable);
			System->Modify();
			FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
			const bool bAdded = Store.AddParameter(Variable, true, true);
			if (const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value")))
			{
				if (!SetStoreValueFromJson(Store, Variable, Value, OutError))
				{
					return nullptr;
				}
			}
			SaveAssetIfRequested(System, ReadBoolParam(Params, TEXT("save")));

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), System->GetPathName());
			Result->SetStringField(TEXT("name"), Variable.GetName().ToString());
			Result->SetStringField(TEXT("type"), TypeToString(TypeDef));
			Result->SetBoolField(TEXT("added"), bAdded);
			RecordOperation(Core, System->GetPathName(), TEXT("Add Niagara user parameter"), Variable.GetName().ToString(), FString::Printf(TEXT("Added user parameter of type %s."), *TypeToString(TypeDef)));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> RemoveUserParameter(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString SystemPath;
			FString ParamName;
			if (!GetStringParam(Params, TEXT("asset"), SystemPath, OutError) || !GetStringParam(Params, TEXT("name"), ParamName, OutError))
			{
				return nullptr;
			}

			UNiagaraSystem* System = LoadSystem(SystemPath, OutError);
			if (!System)
			{
				return nullptr;
			}

			FNiagaraVariable FoundVariable;
			bool bFound = false;
			TArray<FNiagaraVariable> Parameters;
			System->GetExposedParameters().GetUserParameters(Parameters);
			const FName TargetName = NormalizeUserParameterName(ParamName);
			for (const FNiagaraVariable& Parameter : Parameters)
			{
				if (Parameter.GetName() == TargetName)
				{
					FoundVariable = Parameter;
					bFound = true;
					break;
				}
			}

			if (!bFound)
			{
				OutError = FString::Printf(TEXT("User parameter not found: %s."), *TargetName.ToString());
				return nullptr;
			}

			System->Modify();
			const bool bRemoved = System->GetExposedParameters().RemoveParameter(FoundVariable);
			SaveAssetIfRequested(System, ReadBoolParam(Params, TEXT("save")));

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), System->GetPathName());
			Result->SetStringField(TEXT("name"), FoundVariable.GetName().ToString());
			Result->SetBoolField(TEXT("removed"), bRemoved);
			RecordOperation(Core, System->GetPathName(), TEXT("Remove Niagara user parameter"), FoundVariable.GetName().ToString(), TEXT("Removed user parameter."));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> SetUserParameter(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString SystemPath;
			FString ParamName;
			if (!GetStringParam(Params, TEXT("asset"), SystemPath, OutError) || !GetStringParam(Params, TEXT("name"), ParamName, OutError))
			{
				return nullptr;
			}

			UNiagaraSystem* System = LoadSystem(SystemPath, OutError);
			if (!System)
			{
				return nullptr;
			}

			FNiagaraVariable FoundVariable;
			bool bFound = false;
			TArray<FNiagaraVariable> Parameters;
			System->GetExposedParameters().GetUserParameters(Parameters);
			const FName TargetName = NormalizeUserParameterName(ParamName);
			for (const FNiagaraVariable& Parameter : Parameters)
			{
				if (Parameter.GetName() == TargetName)
				{
					FoundVariable = Parameter;
					bFound = true;
					break;
				}
			}

			if (!bFound)
			{
				OutError = FString::Printf(TEXT("User parameter not found: %s."), *TargetName.ToString());
				return nullptr;
			}

			const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
			if (!Value.IsValid())
			{
				OutError = TEXT("Missing params.value.");
				return nullptr;
			}

			System->Modify();
			if (!SetStoreValueFromJson(System->GetExposedParameters(), FoundVariable, Value, OutError))
			{
				return nullptr;
			}
			SaveAssetIfRequested(System, ReadBoolParam(Params, TEXT("save")));

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), System->GetPathName());
			Result->SetStringField(TEXT("name"), FoundVariable.GetName().ToString());
			Result->SetStringField(TEXT("value"), JsonValueToString(Value));
			RecordOperation(Core, System->GetPathName(), TEXT("Set Niagara user parameter"), FoundVariable.GetName().ToString(), FString::Printf(TEXT("Set user parameter to %s."), *JsonValueToString(Value)));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> SetEmitterProperty(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString AssetPath;
			UObject* Asset = nullptr;
			UNiagaraEmitter* Emitter = nullptr;
			UNiagaraSystem* System = nullptr;
			FString EmitterName;
			FVersionedNiagaraEmitterData* EmitterData = ResolveEmitterData(Params, AssetPath, Asset, Emitter, System, EmitterName, OutError);
			if (!EmitterData || !Asset)
			{
				return nullptr;
			}

			FString PropertyName;
			if (!GetStringParam(Params, TEXT("property"), PropertyName, OutError))
			{
				return nullptr;
			}

			const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
			if (!Value.IsValid())
			{
				OutError = TEXT("Missing params.value.");
				return nullptr;
			}

			const FString NewValue = JsonValueToString(Value);
			FString OldValue;

			Asset->Modify();
			if (Emitter)
			{
				Emitter->Modify();
			}

			if (PropertyName.Equals(TEXT("localSpace"), ESearchCase::IgnoreCase) || PropertyName.Equals(TEXT("bLocalSpace"), ESearchCase::IgnoreCase))
			{
				OldValue = EmitterData->bLocalSpace ? TEXT("true") : TEXT("false");
				EmitterData->bLocalSpace = Value->AsBool();
			}
			else if (PropertyName.Equals(TEXT("simTarget"), ESearchCase::IgnoreCase) || PropertyName.Equals(TEXT("SimTarget"), ESearchCase::CaseSensitive))
			{
				OldValue = UEnum::GetValueAsString(EmitterData->SimTarget);
				FString Target = NewValue.ToLower();
				Target.ReplaceInline(TEXT(" "), TEXT(""));
				Target.ReplaceInline(TEXT("_"), TEXT(""));
				if (Target == TEXT("gpu") || Target == TEXT("gpucompute") || Target == TEXT("gpucomputesim") || Target.Contains(TEXT("gpucompute")))
				{
					EmitterData->SimTarget = ENiagaraSimTarget::GPUComputeSim;
				}
				else if (Target == TEXT("cpu") || Target == TEXT("cpusim") || Target.Contains(TEXT("cpusim")))
				{
					EmitterData->SimTarget = ENiagaraSimTarget::CPUSim;
				}
				else
				{
					OutError = FString::Printf(TEXT("Unsupported Niagara sim target: %s."), *NewValue);
					return nullptr;
				}
			}
			else if (PropertyName.Equals(TEXT("spawnRate"), ESearchCase::IgnoreCase))
			{
				UNiagaraScript* EmitterUpdateScript = EmitterData->EmitterUpdateScriptProps.Script;
				if (!EmitterUpdateScript)
				{
					OutError = TEXT("Emitter update script is unavailable.");
					return nullptr;
				}

				const float SpawnRate = FCString::Atof(*NewValue);
				TArray<FNiagaraVariable> RapidIterationVariables;
				EmitterUpdateScript->RapidIterationParameters.GetParameters(RapidIterationVariables);
				TArray<FString> PreviousValues;
				int32 UpdatedParameters = 0;

				EmitterUpdateScript->Modify();
				for (const FNiagaraVariable& Variable : RapidIterationVariables)
				{
					if (Variable.GetType() != FNiagaraTypeDefinition::GetFloatDef() ||
						!Variable.GetName().ToString().EndsWith(TEXT(".SpawnRate"), ESearchCase::IgnoreCase))
					{
						continue;
					}

					const float PreviousValue = EmitterUpdateScript->RapidIterationParameters.GetParameterValue<float>(Variable);
					PreviousValues.Add(FString::Printf(TEXT("%s=%g"), *Variable.GetName().ToString(), PreviousValue));
					EmitterUpdateScript->RapidIterationParameters.SetParameterData(
						reinterpret_cast<const uint8*>(&SpawnRate), Variable, false);
					++UpdatedParameters;
				}

				if (UpdatedParameters == 0)
				{
					OutError = FString::Printf(TEXT("No SpawnRate rapid-iteration parameter was found on emitter %s."), *EmitterName);
					return nullptr;
				}

				OldValue = FString::Join(PreviousValues, TEXT(", "));
			}
			else
			{
				FProperty* Property = FVersionedNiagaraEmitterData::StaticStruct()->FindPropertyByName(*PropertyName);
				if (!Property)
				{
					OutError = FString::Printf(TEXT("Emitter data property not found: %s."), *PropertyName);
					return nullptr;
				}

				void* PropertyValue = Property->ContainerPtrToValuePtr<void>(EmitterData);
				Property->ExportText_Direct(OldValue, PropertyValue, PropertyValue, nullptr, PPF_None);
				const TCHAR* ImportResult = Property->ImportText_Direct(*NewValue, PropertyValue, nullptr, PPF_None);
				if (!ImportResult)
				{
					OutError = FString::Printf(TEXT("Failed to import emitter property %s: %s."), *PropertyName, *NewValue);
					return nullptr;
				}
			}

			if (Emitter)
			{
				EmitterData->RebuildRendererBindings(*Emitter);
			}
			if (System)
			{
				System->RequestCompile(true);
			}
			MarkNiagaraAssetChanged(Asset);
			SaveAssetIfRequested(Asset, ReadBoolParam(Params, TEXT("save")));

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), AssetPath);
			Result->SetStringField(TEXT("emitter"), EmitterName);
			Result->SetStringField(TEXT("property"), PropertyName);
			Result->SetStringField(TEXT("oldValue"), OldValue);
			Result->SetStringField(TEXT("newValue"), NewValue);
			RecordOperation(Core, AssetPath, TEXT("Set Niagara emitter property"), EmitterName, FString::Printf(TEXT("Set %s from %s to %s."), *PropertyName, *OldValue, *NewValue));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> ListRenderers(const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString AssetPath;
			UObject* Asset = nullptr;
			UNiagaraEmitter* Emitter = nullptr;
			UNiagaraSystem* System = nullptr;
			FString EmitterName;
			FVersionedNiagaraEmitterData* EmitterData = ResolveEmitterData(Params, AssetPath, Asset, Emitter, System, EmitterName, OutError);
			if (!EmitterData)
			{
				return nullptr;
			}

			TArray<TSharedPtr<FJsonValue>> Renderers;
			int32 RendererIndex = 0;
			for (const UNiagaraRendererProperties* Renderer : EmitterData->GetRenderers())
			{
				Renderers.Add(MakeShared<FJsonValueObject>(RendererToJson(Renderer, RendererIndex++)));
			}

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), AssetPath);
			Result->SetStringField(TEXT("emitter"), EmitterName);
			Result->SetArrayField(TEXT("renderers"), Renderers);
			return Result;
		}

		TSharedPtr<FJsonObject> AddRenderer(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString AssetPath;
			UObject* Asset = nullptr;
			UNiagaraEmitter* Emitter = nullptr;
			UNiagaraSystem* System = nullptr;
			FString EmitterName;
			FVersionedNiagaraEmitterData* EmitterData = ResolveEmitterData(Params, AssetPath, Asset, Emitter, System, EmitterName, OutError);
			if (!EmitterData || !Asset || !Emitter)
			{
				if (OutError.IsEmpty())
				{
					OutError = TEXT("Could not resolve a mutable Niagara emitter for renderer creation.");
				}
				return nullptr;
			}

			FString Type;
			Params->TryGetStringField(TEXT("type"), Type);
			FString Name;
			Params->TryGetStringField(TEXT("name"), Name);
			FString MaterialPath;
			Params->TryGetStringField(TEXT("material"), MaterialPath);
			FString MeshPath;
			Params->TryGetStringField(TEXT("mesh"), MeshPath);

			Asset->Modify();
			Emitter->Modify();
			FString NormalizedType;
			UNiagaraRendererProperties* Renderer = AddRendererToEmitter(Emitter, EmitterData, Type, Name, MaterialPath, MeshPath, NormalizedType, OutError);
			if (!Renderer)
			{
				return nullptr;
			}
			if (System)
			{
				System->RequestCompile(true);
			}
			MarkNiagaraAssetChanged(Asset);
			SaveAssetIfRequested(Asset, ReadBoolParam(Params, TEXT("save")));

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), AssetPath);
			Result->SetStringField(TEXT("emitter"), EmitterName);
			Result->SetObjectField(TEXT("renderer"), RendererToJson(Renderer, EmitterData->GetRenderers().IndexOfByKey(Renderer)));
			RecordOperation(Core, AssetPath, TEXT("Add Niagara renderer"), EmitterName, FString::Printf(TEXT("Added %s renderer %s."), *NormalizedType, *Renderer->GetName()));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> RemoveRenderer(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString AssetPath;
			UObject* Asset = nullptr;
			UNiagaraEmitter* Emitter = nullptr;
			UNiagaraSystem* System = nullptr;
			FString EmitterName;
			FVersionedNiagaraEmitterData* EmitterData = ResolveEmitterData(Params, AssetPath, Asset, Emitter, System, EmitterName, OutError);
			if (!EmitterData || !Asset || !Emitter)
			{
				if (OutError.IsEmpty())
				{
					OutError = TEXT("Could not resolve a mutable Niagara emitter for renderer removal.");
				}
				return nullptr;
			}

			FString RendererId;
			Params->TryGetStringField(TEXT("renderer"), RendererId);
			const int32 RendererIndex = ReadIntParam(Params, TEXT("index"), INDEX_NONE);
			UNiagaraRendererProperties* Renderer = FindRendererByIndexOrName(EmitterData, RendererId, RendererIndex);
			if (!Renderer)
			{
				OutError = RendererId.IsEmpty()
					? FString::Printf(TEXT("Niagara renderer index not found: %d."), RendererIndex)
					: FString::Printf(TEXT("Niagara renderer not found: %s."), *RendererId);
				return nullptr;
			}

			const FString RendererName = Renderer->GetName();
			const FString RendererClass = Renderer->GetClass()->GetName();
			Asset->Modify();
			Emitter->Modify();
			Renderer->Modify();
			Emitter->RemoveRenderer(Renderer, EmitterData->Version.VersionGuid);
			EmitterData->RebuildRendererBindings(*Emitter);
			if (System)
			{
				System->RequestCompile(true);
			}
			MarkNiagaraAssetChanged(Asset);
			SaveAssetIfRequested(Asset, ReadBoolParam(Params, TEXT("save")));

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), AssetPath);
			Result->SetStringField(TEXT("emitter"), EmitterName);
			Result->SetStringField(TEXT("renderer"), RendererName);
			Result->SetStringField(TEXT("class"), RendererClass);
			Result->SetBoolField(TEXT("removed"), true);
			RecordOperation(Core, AssetPath, TEXT("Remove Niagara renderer"), EmitterName, FString::Printf(TEXT("Removed renderer %s (%s)."), *RendererName, *RendererClass));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> SetRendererProperty(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString AssetPath;
			UObject* Asset = nullptr;
			UNiagaraEmitter* Emitter = nullptr;
			UNiagaraSystem* System = nullptr;
			FString EmitterName;
			FVersionedNiagaraEmitterData* EmitterData = ResolveEmitterData(Params, AssetPath, Asset, Emitter, System, EmitterName, OutError);
			if (!EmitterData || !Asset)
			{
				return nullptr;
			}

			FString RendererId;
			Params->TryGetStringField(TEXT("renderer"), RendererId);
			const int32 RendererIndex = ReadIntParam(Params, TEXT("index"), INDEX_NONE);
			UNiagaraRendererProperties* Renderer = FindRendererByIndexOrName(EmitterData, RendererId, RendererIndex);
			if (!Renderer)
			{
				OutError = RendererId.IsEmpty()
					? FString::Printf(TEXT("Niagara renderer index not found: %d."), RendererIndex)
					: FString::Printf(TEXT("Niagara renderer not found: %s."), *RendererId);
				return nullptr;
			}

			FString PropertyName;
			if (!GetStringParam(Params, TEXT("property"), PropertyName, OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
			if (!Value.IsValid())
			{
				OutError = TEXT("Missing params.value.");
				return nullptr;
			}

			const FString NewValue = JsonValueToString(Value);
			FString OldValue;
			Asset->Modify();
			Renderer->Modify();
			if (PropertyName.Equals(TEXT("enabled"), ESearchCase::IgnoreCase) || PropertyName.Equals(TEXT("bIsEnabled"), ESearchCase::IgnoreCase))
			{
				OldValue = Renderer->GetIsEnabled() ? TEXT("true") : TEXT("false");
				Renderer->SetIsEnabled(Value->AsBool());
			}
			else if (PropertyName.Equals(TEXT("material"), ESearchCase::IgnoreCase))
			{
				OldValue = TEXT("");
				UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *NormalizeAssetObjectPath(NewValue));
				if (!Material)
				{
					OutError = FString::Printf(TEXT("Material not found: %s."), *NewValue);
					return nullptr;
				}
				if (UNiagaraSpriteRendererProperties* SpriteRenderer = Cast<UNiagaraSpriteRendererProperties>(Renderer))
				{
					OldValue = SpriteRenderer->Material ? SpriteRenderer->Material->GetPathName() : FString();
					SpriteRenderer->Material = Material;
				}
				else if (UNiagaraRibbonRendererProperties* RibbonRenderer = Cast<UNiagaraRibbonRendererProperties>(Renderer))
				{
					OldValue = RibbonRenderer->Material ? RibbonRenderer->Material->GetPathName() : FString();
					RibbonRenderer->Material = Material;
				}
				else if (UNiagaraMeshRendererProperties* MeshRenderer = Cast<UNiagaraMeshRendererProperties>(Renderer))
				{
					OldValue = MeshRenderer->OverrideMaterials.Num() > 0 && MeshRenderer->OverrideMaterials[0].ExplicitMat
						? MeshRenderer->OverrideMaterials[0].ExplicitMat->GetPathName()
						: FString();
					MeshRenderer->bOverrideMaterials = true;
					if (MeshRenderer->OverrideMaterials.Num() == 0)
					{
						MeshRenderer->OverrideMaterials.AddDefaulted();
					}
					MeshRenderer->OverrideMaterials[0].ExplicitMat = Material;
				}
				else
				{
					OutError = FString::Printf(TEXT("Renderer does not expose a direct material property: %s."), *Renderer->GetClass()->GetName());
					return nullptr;
				}
			}
			else if (PropertyName.Equals(TEXT("mesh"), ESearchCase::IgnoreCase))
			{
				UNiagaraMeshRendererProperties* MeshRenderer = Cast<UNiagaraMeshRendererProperties>(Renderer);
				if (!MeshRenderer)
				{
					OutError = TEXT("The mesh property can only be set on a Niagara mesh renderer.");
					return nullptr;
				}
				UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *NormalizeAssetObjectPath(NewValue));
				if (!Mesh)
				{
					OutError = FString::Printf(TEXT("StaticMesh not found: %s."), *NewValue);
					return nullptr;
				}
				OldValue = MeshRenderer->Meshes.Num() > 0 && MeshRenderer->Meshes[0].Mesh ? MeshRenderer->Meshes[0].Mesh->GetPathName() : FString();
				if (MeshRenderer->Meshes.Num() == 0)
				{
					MeshRenderer->Meshes.AddDefaulted();
				}
				MeshRenderer->Meshes[0].Mesh = Mesh;
				MeshRenderer->Meshes[0].Scale = FVector::OneVector;
			}
			else
			{
				if (!SetObjectPropertyFromString(Renderer, PropertyName, NewValue, OldValue, OutError))
				{
					return nullptr;
				}
			}

			if (Emitter)
			{
				EmitterData->RebuildRendererBindings(*Emitter);
			}
			if (System)
			{
				System->RequestCompile(true);
			}
			Renderer->PostEditChange();
			MarkNiagaraAssetChanged(Asset);
			SaveAssetIfRequested(Asset, ReadBoolParam(Params, TEXT("save")));

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), AssetPath);
			Result->SetStringField(TEXT("emitter"), EmitterName);
			Result->SetStringField(TEXT("renderer"), Renderer->GetName());
			Result->SetStringField(TEXT("property"), PropertyName);
			Result->SetStringField(TEXT("oldValue"), OldValue);
			Result->SetStringField(TEXT("newValue"), NewValue);
			RecordOperation(Core, AssetPath, TEXT("Set Niagara renderer property"), Renderer->GetName(), FString::Printf(TEXT("Set %s from %s to %s."), *PropertyName, *OldValue, *NewValue));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> AddModuleIfMissing(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString AssetPath;
			UObject* Asset = nullptr;
			UNiagaraScriptSource* Source = ResolveScriptSource(Params, AssetPath, Asset, OutError);
			if (!Source)
			{
				return nullptr;
			}

			FString ModulePath;
			FString UsageString;
			if (!GetStringParam(Params, TEXT("module"), ModulePath, OutError) || !GetStringParam(Params, TEXT("usage"), UsageString, OutError))
			{
				return nullptr;
			}

			ENiagaraScriptUsage Usage;
			if (!ParseUsage(UsageString, Usage, OutError))
			{
				return nullptr;
			}

			bool bFoundModule = false;
			Asset->Modify();
			const bool bAdded = Source->AddModuleIfMissing(ModulePath, Usage, bFoundModule);
			if (Source->NodeGraph)
			{
				Source->NodeGraph->MarkPackageDirty();
			}
			SaveAssetIfRequested(Asset, ReadBoolParam(Params, TEXT("save")));

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), AssetPath);
			Result->SetStringField(TEXT("module"), ModulePath);
			Result->SetStringField(TEXT("usage"), UsageString);
			Result->SetBoolField(TEXT("added"), bAdded);
			Result->SetBoolField(TEXT("foundExisting"), bFoundModule);
			RecordOperation(Core, AssetPath, TEXT("Add Niagara module"), UsageString, FString::Printf(TEXT("Add module if missing: %s."), *ModulePath));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> AddGraphNode(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString AssetPath;
			UObject* Asset = nullptr;
			UNiagaraGraph* Graph = ResolveGraph(Params, AssetPath, Asset, OutError);
			if (!Graph)
			{
				return nullptr;
			}

			FString NodeClassString;
			Params->TryGetStringField(TEXT("nodeClass"), NodeClassString);
			UClass* NodeClass = ResolveNodeClass(NodeClassString);
			if (!NodeClass)
			{
				OutError = FString::Printf(TEXT("Unsupported Niagara node class or alias: %s."), *NodeClassString);
				return nullptr;
			}

			Asset->Modify();
			Graph->Modify();
			UNiagaraNode* Node = NewObject<UNiagaraNode>(Graph, NodeClass, NAME_None, RF_Transactional);
			Node->NodePosX = static_cast<int32>(ReadNumberParam(Params, TEXT("x")));
			Node->NodePosY = static_cast<int32>(ReadNumberParam(Params, TEXT("y")));

			if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node))
			{
				FString Name;
				FString TypeString;
				Params->TryGetStringField(TEXT("name"), Name);
				Params->TryGetStringField(TEXT("type"), TypeString);
				if (Name.IsEmpty())
				{
					Name = TEXT("Input");
				}
				FNiagaraTypeDefinition TypeDef = FNiagaraTypeDefinition::GetFloatDef();
				if (!TypeString.IsEmpty() && !ParseType(TypeString, TypeDef, OutError))
				{
					return nullptr;
				}
				InputNode->Input = FNiagaraVariable(TypeDef, FName(*Name));
				InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
			}
			else if (UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(Node))
			{
				FString UsageString;
				if (Params->TryGetStringField(TEXT("usage"), UsageString) && !UsageString.IsEmpty())
				{
					ENiagaraScriptUsage Usage;
					if (!ParseUsage(UsageString, Usage, OutError))
					{
						return nullptr;
					}
					OutputNode->SetUsage(Usage);
				}
			}
			else if (UNiagaraNodeOp* OpNode = Cast<UNiagaraNodeOp>(Node))
			{
				FString OpName;
				Params->TryGetStringField(TEXT("opName"), OpName);
				if (!OpName.IsEmpty())
				{
					OpNode->OpName = FName(*OpName);
				}
			}
			else if (UNiagaraNodeFunctionCall* FunctionNode = Cast<UNiagaraNodeFunctionCall>(Node))
			{
				FString FunctionScriptPath;
				Params->TryGetStringField(TEXT("functionScript"), FunctionScriptPath);
				if (!FunctionScriptPath.IsEmpty())
				{
					FunctionNode->FunctionScript = LoadObject<UNiagaraScript>(nullptr, *FunctionScriptPath);
					FunctionNode->FunctionScriptAssetObjectPath = FName(*FunctionScriptPath);
				}
			}

			Graph->AddNode(Node, true, false);
			Node->CreateNewGuid();
			Node->PostPlacedNewNode();
			Node->AllocateDefaultPins();

			if (UNiagaraNodeAssignment* AssignmentNode = Cast<UNiagaraNodeAssignment>(Node))
			{
				const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
				if (Params->TryGetArrayField(TEXT("targets"), Targets) && Targets)
				{
					for (const TSharedPtr<FJsonValue>& TargetValue : *Targets)
					{
						const TSharedPtr<FJsonObject>* TargetObject = nullptr;
						if (!TargetValue.IsValid() || !TargetValue->TryGetObject(TargetObject) || !TargetObject || !TargetObject->IsValid())
						{
							continue;
						}

						FString TargetName;
						FString TargetType;
						(*TargetObject)->TryGetStringField(TEXT("name"), TargetName);
						(*TargetObject)->TryGetStringField(TEXT("type"), TargetType);
						FString DefaultValue;
						(*TargetObject)->TryGetStringField(TEXT("defaultValue"), DefaultValue);
						if (!TargetName.IsEmpty() && !TargetType.IsEmpty())
						{
							FNiagaraTypeDefinition TypeDef;
							if (ParseType(TargetType, TypeDef, OutError))
							{
								AssignmentNode->AddAssignmentTarget(FNiagaraVariable(TypeDef, FName(*TargetName)), DefaultValue.IsEmpty() ? nullptr : &DefaultValue);
							}
						}
					}
					AssignmentNode->RefreshFromExternalChanges();
					AssignmentNode->AllocateDefaultPins();
				}
			}

			Graph->MarkPackageDirty();
			SaveAssetIfRequested(Asset, ReadBoolParam(Params, TEXT("save")));

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), AssetPath);
			Result->SetObjectField(TEXT("node"), NodeToJson(Node));
			RecordOperation(Core, AssetPath, TEXT("Add Niagara graph node"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), FString::Printf(TEXT("Added node class %s."), *Node->GetClass()->GetName()));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> MoveGraphNode(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString AssetPath;
			UObject* Asset = nullptr;
			UNiagaraGraph* Graph = ResolveGraph(Params, AssetPath, Asset, OutError);
			if (!Graph || !Asset)
			{
				return nullptr;
			}

			FString NodeId;
			if (!GetStringParam(Params, TEXT("node"), NodeId, OutError))
			{
				return nullptr;
			}

			UEdGraphNode* Node = FindNodeByIdOrTitle(Graph, NodeId);
			if (!Node)
			{
				OutError = FString::Printf(TEXT("Niagara graph node not found: %s."), *NodeId);
				return nullptr;
			}

			const int32 OldX = Node->NodePosX;
			const int32 OldY = Node->NodePosY;
			Asset->Modify();
			Graph->Modify();
			Node->Modify();
			Node->NodePosX = static_cast<int32>(ReadNumberParam(Params, TEXT("x"), Node->NodePosX));
			Node->NodePosY = static_cast<int32>(ReadNumberParam(Params, TEXT("y"), Node->NodePosY));
			Graph->NotifyGraphChanged();
			Graph->MarkPackageDirty();
			SaveAssetIfRequested(Asset, ReadBoolParam(Params, TEXT("save")));

			const FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), AssetPath);
			Result->SetStringField(TEXT("node"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			Result->SetStringField(TEXT("title"), NodeTitle);
			Result->SetNumberField(TEXT("oldX"), OldX);
			Result->SetNumberField(TEXT("oldY"), OldY);
			Result->SetNumberField(TEXT("x"), Node->NodePosX);
			Result->SetNumberField(TEXT("y"), Node->NodePosY);
			RecordOperation(Core, AssetPath, TEXT("Move Niagara graph node"), NodeTitle, FString::Printf(TEXT("Move node from (%d, %d) to (%d, %d)."), OldX, OldY, Node->NodePosX, Node->NodePosY));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> DeleteGraphNode(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString AssetPath;
			UObject* Asset = nullptr;
			UNiagaraGraph* Graph = ResolveGraph(Params, AssetPath, Asset, OutError);
			if (!Graph || !Asset)
			{
				return nullptr;
			}

			FString NodeId;
			if (!GetStringParam(Params, TEXT("node"), NodeId, OutError))
			{
				return nullptr;
			}

			UEdGraphNode* Node = FindNodeByIdOrTitle(Graph, NodeId);
			if (!Node)
			{
				OutError = FString::Printf(TEXT("Niagara graph node not found: %s."), *NodeId);
				return nullptr;
			}

			const FString NodeGuid = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
			const FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
			const FString NodeClass = Node->GetClass()->GetName();
			const int32 PinCount = Node->Pins.Num();
			Asset->Modify();
			Graph->Modify();
			Node->Modify();
			const bool bRemoved = Graph->RemoveNode(Node, true);
			if (!bRemoved)
			{
				OutError = FString::Printf(TEXT("Failed to remove Niagara graph node: %s."), *NodeId);
				return nullptr;
			}
			Graph->NotifyGraphChanged();
			Graph->MarkPackageDirty();
			SaveAssetIfRequested(Asset, ReadBoolParam(Params, TEXT("save")));

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), AssetPath);
			Result->SetStringField(TEXT("node"), NodeGuid);
			Result->SetStringField(TEXT("title"), NodeTitle);
			Result->SetStringField(TEXT("class"), NodeClass);
			Result->SetNumberField(TEXT("pinCount"), PinCount);
			Result->SetBoolField(TEXT("deleted"), true);
			RecordOperation(Core, AssetPath, TEXT("Delete Niagara graph node"), NodeTitle, FString::Printf(TEXT("Deleted node %s with %d pin(s)."), *NodeClass, PinCount));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> SetGraphNodeProperty(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString AssetPath;
			UObject* Asset = nullptr;
			UNiagaraGraph* Graph = ResolveGraph(Params, AssetPath, Asset, OutError);
			if (!Graph)
			{
				return nullptr;
			}

			FString NodeId;
			FString PropertyName;
			if (!GetStringParam(Params, TEXT("node"), NodeId, OutError) || !GetStringParam(Params, TEXT("property"), PropertyName, OutError))
			{
				return nullptr;
			}

			UEdGraphNode* Node = FindNodeByIdOrTitle(Graph, NodeId);
			if (!Node)
			{
				OutError = FString::Printf(TEXT("Niagara graph node not found: %s."), *NodeId);
				return nullptr;
			}

			const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
			if (!Value.IsValid())
			{
				OutError = TEXT("Missing params.value.");
				return nullptr;
			}

			Asset->Modify();
			Node->Modify();
			FString OldValue;
			const FString NewValue = JsonValueToString(Value);
			if (!SetObjectPropertyFromString(Node, PropertyName, NewValue, OldValue, OutError))
			{
				return nullptr;
			}
			Node->ReconstructNode();
			Graph->MarkPackageDirty();
			SaveAssetIfRequested(Asset, ReadBoolParam(Params, TEXT("save")));

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), AssetPath);
			Result->SetStringField(TEXT("node"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			Result->SetStringField(TEXT("property"), PropertyName);
			Result->SetStringField(TEXT("oldValue"), OldValue);
			Result->SetStringField(TEXT("newValue"), NewValue);
			RecordOperation(Core, AssetPath, TEXT("Set Niagara node property"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), FString::Printf(TEXT("Set %s from %s to %s."), *PropertyName, *OldValue, *NewValue));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> SetGraphPinDefault(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString AssetPath;
			UObject* Asset = nullptr;
			UNiagaraGraph* Graph = ResolveGraph(Params, AssetPath, Asset, OutError);
			if (!Graph)
			{
				return nullptr;
			}

			FString NodeId;
			FString PinName;
			if (!GetStringParam(Params, TEXT("node"), NodeId, OutError) || !GetStringParam(Params, TEXT("pin"), PinName, OutError))
			{
				return nullptr;
			}

			UEdGraphNode* Node = FindNodeByIdOrTitle(Graph, NodeId);
			UEdGraphPin* Pin = FindPin(Node, PinName, EGPD_Input);
			if (!Pin)
			{
				OutError = FString::Printf(TEXT("Input pin not found: %s."), *PinName);
				return nullptr;
			}

			const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
			if (!Value.IsValid())
			{
				OutError = TEXT("Missing params.value.");
				return nullptr;
			}

			const FString NewValue = JsonValueToString(Value);
			const FString OldValue = Pin->DefaultValue;
			Asset->Modify();
			Node->Modify();
			Pin->DefaultValue = NewValue;
			Graph->MarkPackageDirty();
			SaveAssetIfRequested(Asset, ReadBoolParam(Params, TEXT("save")));

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), AssetPath);
			Result->SetStringField(TEXT("node"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			Result->SetStringField(TEXT("pin"), Pin->PinName.ToString());
			Result->SetStringField(TEXT("oldValue"), OldValue);
			Result->SetStringField(TEXT("newValue"), NewValue);
			RecordOperation(Core, AssetPath, TEXT("Set Niagara pin default"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), FString::Printf(TEXT("Set pin %s from %s to %s."), *Pin->PinName.ToString(), *OldValue, *NewValue));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> ConnectGraphPins(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString AssetPath;
			UObject* Asset = nullptr;
			UNiagaraGraph* Graph = ResolveGraph(Params, AssetPath, Asset, OutError);
			if (!Graph)
			{
				return nullptr;
			}

			FString FromNodeId;
			FString FromPinName;
			FString ToNodeId;
			FString ToPinName;
			if (!GetStringParam(Params, TEXT("fromNode"), FromNodeId, OutError) ||
				!GetStringParam(Params, TEXT("fromPin"), FromPinName, OutError) ||
				!GetStringParam(Params, TEXT("toNode"), ToNodeId, OutError) ||
				!GetStringParam(Params, TEXT("toPin"), ToPinName, OutError))
			{
				return nullptr;
			}

			UEdGraphNode* FromNode = FindNodeByIdOrTitle(Graph, FromNodeId);
			UEdGraphNode* ToNode = FindNodeByIdOrTitle(Graph, ToNodeId);
			UEdGraphPin* FromPin = FindPin(FromNode, FromPinName, EGPD_Output);
			UEdGraphPin* ToPin = FindPin(ToNode, ToPinName, EGPD_Input);
			if (!FromPin || !ToPin)
			{
				OutError = TEXT("Could not resolve source output pin or target input pin.");
				return nullptr;
			}

			Asset->Modify();
			FromNode->Modify();
			ToNode->Modify();
			bool bConnected = false;
			if (const UEdGraphSchema* Schema = Graph->GetSchema())
			{
				bConnected = Schema->TryCreateConnection(FromPin, ToPin);
			}
			if (!bConnected)
			{
				FromPin->MakeLinkTo(ToPin);
				bConnected = true;
			}
			Graph->MarkPackageDirty();
			SaveAssetIfRequested(Asset, ReadBoolParam(Params, TEXT("save")));

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), AssetPath);
			Result->SetStringField(TEXT("fromNode"), FromNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			Result->SetStringField(TEXT("fromPin"), FromPin->PinName.ToString());
			Result->SetStringField(TEXT("toNode"), ToNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			Result->SetStringField(TEXT("toPin"), ToPin->PinName.ToString());
			Result->SetBoolField(TEXT("connected"), bConnected);
			RecordOperation(Core, AssetPath, TEXT("Connect Niagara graph pins"), FromNode->GetNodeTitle(ENodeTitleType::ListView).ToString(), FString::Printf(TEXT("Connected %s to %s.%s."), *FromPin->PinName.ToString(), *ToNode->GetNodeTitle(ENodeTitleType::ListView).ToString(), *ToPin->PinName.ToString()));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> DisconnectGraphPins(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString AssetPath;
			UObject* Asset = nullptr;
			UNiagaraGraph* Graph = ResolveGraph(Params, AssetPath, Asset, OutError);
			if (!Graph || !Asset)
			{
				return nullptr;
			}

			FString NodeId;
			if (!GetStringParam(Params, TEXT("node"), NodeId, OutError))
			{
				return nullptr;
			}

			FString PinName;
			FString OtherNodeId;
			FString OtherPinName;
			Params->TryGetStringField(TEXT("pin"), PinName);
			Params->TryGetStringField(TEXT("otherNode"), OtherNodeId);
			Params->TryGetStringField(TEXT("otherPin"), OtherPinName);

			UEdGraphNode* Node = FindNodeByIdOrTitle(Graph, NodeId);
			if (!Node)
			{
				OutError = FString::Printf(TEXT("Niagara graph node not found: %s."), *NodeId);
				return nullptr;
			}

			const FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
			int32 DisconnectedLinks = 0;
			FString Scope = TEXT("all pins");

			Asset->Modify();
			Graph->Modify();
			Node->Modify();
			if (!PinName.IsEmpty())
			{
				UEdGraphPin* Pin = FindPinAnyDirection(Node, PinName);
				if (!Pin)
				{
					OutError = FString::Printf(TEXT("Niagara graph pin not found: %s."), *PinName);
					return nullptr;
				}
				Scope = Pin->PinName.ToString();
				if (!OtherNodeId.IsEmpty() || !OtherPinName.IsEmpty())
				{
					UEdGraphNode* OtherNode = FindNodeByIdOrTitle(Graph, OtherNodeId);
					UEdGraphPin* OtherPin = FindPinAnyDirection(OtherNode, OtherPinName);
					if (!OtherNode || !OtherPin)
					{
						OutError = TEXT("Other Niagara graph node or pin not found.");
						return nullptr;
					}
					OtherNode->Modify();
					if (Pin->LinkedTo.Contains(OtherPin))
					{
						if (const UEdGraphSchema* Schema = Graph->GetSchema())
						{
							Schema->BreakSinglePinLink(Pin, OtherPin);
						}
						else
						{
							Pin->BreakLinkTo(OtherPin);
						}
						DisconnectedLinks = 1;
					}
				}
				else
				{
					DisconnectedLinks = Pin->LinkedTo.Num();
					if (const UEdGraphSchema* Schema = Graph->GetSchema())
					{
						Schema->BreakPinLinks(*Pin, true);
					}
					else
					{
						Pin->BreakAllPinLinks(true);
					}
				}
			}
			else
			{
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin)
					{
						DisconnectedLinks += Pin->LinkedTo.Num();
					}
				}
				if (const UEdGraphSchema* Schema = Graph->GetSchema())
				{
					Schema->BreakNodeLinks(*Node);
				}
				else
				{
					Node->BreakAllNodeLinks();
				}
			}

			Graph->NotifyGraphChanged();
			Graph->MarkPackageDirty();
			SaveAssetIfRequested(Asset, ReadBoolParam(Params, TEXT("save")));

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), AssetPath);
			Result->SetStringField(TEXT("node"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			Result->SetStringField(TEXT("title"), NodeTitle);
			Result->SetStringField(TEXT("scope"), Scope);
			Result->SetNumberField(TEXT("disconnectedLinks"), DisconnectedLinks);
			RecordOperation(Core, AssetPath, TEXT("Disconnect Niagara graph pins"), NodeTitle, FString::Printf(TEXT("Disconnected %d link(s) from %s."), DisconnectedLinks, *Scope));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> CreatePreset(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString PackagePath;
			FString AssetName;
			if (!ResolvePackageAndAssetName(Params, PackagePath, AssetName, OutError))
			{
				return nullptr;
			}

			FString Preset;
			if (!GetStringParam(Params, TEXT("preset"), Preset, OutError))
			{
				return nullptr;
			}

			FString PresetKey = Preset.ToLower();
			PresetKey.ReplaceInline(TEXT("-"), TEXT("_"));
			PresetKey.ReplaceInline(TEXT(" "), TEXT("_"));
			const bool bFishSchool = PresetKey == TEXT("fish_school") || PresetKey == TEXT("fish") || PresetKey == TEXT("underwater_fish_school");
			const bool bJetExhaust = PresetKey == TEXT("jet_exhaust") || PresetKey == TEXT("exhaust") || PresetKey == TEXT("fighter_jet_exhaust");
			const bool bSonicBoom = PresetKey == TEXT("sonic_boom") || PresetKey == TEXT("vapor_cone") || PresetKey == TEXT("shock_cone");
			if (!bFishSchool && !bJetExhaust && !bSonicBoom)
			{
				OutError = FString::Printf(TEXT("Unsupported Niagara preset: %s. Supported presets: fish_school, jet_exhaust, sonic_boom."), *Preset);
				return nullptr;
			}

			const FString PackageName = PackagePath / AssetName;
			const FString ObjectPath = PackageName + TEXT(".") + AssetName;
			if (LoadObject<UNiagaraSystem>(nullptr, *ObjectPath))
			{
				OutError = FString::Printf(TEXT("NiagaraSystem already exists: %s. Choose another name or rename the existing asset first."), *ObjectPath);
				return nullptr;
			}

			UPackage* Package = CreatePackage(*PackageName);
			UNiagaraSystem* System = NewObject<UNiagaraSystem>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
			if (!System)
			{
				OutError = FString::Printf(TEXT("Failed to create NiagaraSystem preset: %s."), *ObjectPath);
				return nullptr;
			}

			UNiagaraSystemFactoryNew::InitializeSystem(System, true);
			AddCommonPresetUserParameters(System);

			TArray<TPair<FString, FString>> OperationDetails;
			TArray<TSharedPtr<FJsonValue>> AddedEmitters;
			TArray<TSharedPtr<FJsonValue>> AddedModules;
			TArray<TSharedPtr<FJsonValue>> AddedRenderers;
			auto AddUserParameter = [System](const FString& Name, const FNiagaraTypeDefinition& Type, const TSharedPtr<FJsonValue>& Value)
			{
				FNiagaraVariable Variable(Type, NormalizeUserParameterName(Name));
				FNiagaraUserRedirectionParameterStore::MakeUserVariable(Variable);
				FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
				Store.AddParameter(Variable, true, true);
				FString IgnoredError;
				SetStoreValueFromJson(Store, Variable, Value, IgnoredError);
			};

			auto AddModule = [&AddedModules](FVersionedNiagaraEmitterData* EmitterData, const FString& EmitterName, const FString& ModulePath, ENiagaraScriptUsage Usage)
			{
				const bool bAdded = AddModuleToEmitterDataIfMissing(EmitterData, ModulePath, Usage);
				TSharedPtr<FJsonObject> ModuleObject = MakeShared<FJsonObject>();
				ModuleObject->SetStringField(TEXT("emitter"), EmitterName);
				ModuleObject->SetStringField(TEXT("module"), ModulePath);
				ModuleObject->SetStringField(TEXT("usage"), UEnum::GetValueAsString(Usage));
				ModuleObject->SetBoolField(TEXT("added"), bAdded);
				AddedModules.Add(MakeShared<FJsonValueObject>(ModuleObject));
			};

			auto RemoveDefaultRenderers = [](UNiagaraEmitter* Emitter, FVersionedNiagaraEmitterData* EmitterData)
			{
				if (!Emitter || !EmitterData)
				{
					return;
				}
				TArray<UNiagaraRendererProperties*> Renderers = EmitterData->GetRenderers();
				for (UNiagaraRendererProperties* Renderer : Renderers)
				{
					if (Renderer)
					{
						Emitter->RemoveRenderer(Renderer, EmitterData->Version.VersionGuid);
					}
				}
			};

			auto ConfigureEmitter = [&](const FString& EmitterName, ENiagaraSimTarget SimTarget, bool bLocalSpace)
			{
				FNiagaraEmitterHandle* Handle = AddDefaultEmitterToSystem(System, EmitterName);
				if (!Handle)
				{
					return static_cast<FNiagaraEmitterHandle*>(nullptr);
				}
				if (FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData())
				{
					EmitterData->SimTarget = SimTarget;
					EmitterData->bLocalSpace = bLocalSpace;
				}
				TSharedPtr<FJsonObject> EmitterObject = MakeShared<FJsonObject>();
				EmitterObject->SetStringField(TEXT("name"), Handle->GetName().ToString());
				EmitterObject->SetStringField(TEXT("id"), Handle->GetId().ToString(EGuidFormats::DigitsWithHyphens));
				AddedEmitters.Add(MakeShared<FJsonValueObject>(EmitterObject));
				return Handle;
			};

			auto AddPresetRenderer = [&](FNiagaraEmitterHandle* Handle, const FString& Type, const FString& Name, const FString& MaterialPath, const FString& MeshPath)
			{
				if (!Handle || !Handle->GetInstance().Emitter || !Handle->GetEmitterData())
				{
					return;
				}
				FString NormalizedType;
				FString LocalError;
				if (UNiagaraRendererProperties* Renderer = AddRendererToEmitter(Handle->GetInstance().Emitter, Handle->GetEmitterData(), Type, Name, MaterialPath, MeshPath, NormalizedType, LocalError))
				{
					AddedRenderers.Add(MakeShared<FJsonValueObject>(RendererToJson(Renderer, Handle->GetEmitterData()->GetRenderers().IndexOfByKey(Renderer))));
				}
			};

			if (bFishSchool)
			{
				AddUserParameter(TEXT("FishSpawnRate"), FNiagaraTypeDefinition::GetFloatDef(), MakeShared<FJsonValueNumber>(18.0));
				AddUserParameter(TEXT("SwimRadius"), FNiagaraTypeDefinition::GetFloatDef(), MakeShared<FJsonValueNumber>(2500.0));
				AddUserParameter(TEXT("SwimSpeed"), FNiagaraTypeDefinition::GetFloatDef(), MakeShared<FJsonValueNumber>(280.0));
				AddUserParameter(TEXT("WanderStrength"), FNiagaraTypeDefinition::GetFloatDef(), MakeShared<FJsonValueNumber>(0.75));
				AddUserParameter(TEXT("LifetimeMin"), FNiagaraTypeDefinition::GetFloatDef(), MakeShared<FJsonValueNumber>(8.0));
				AddUserParameter(TEXT("LifetimeMax"), FNiagaraTypeDefinition::GetFloatDef(), MakeShared<FJsonValueNumber>(18.0));

				FNiagaraEmitterHandle* FishHandle = ConfigureEmitter(TEXT("FishSchool_MeshFish"), ENiagaraSimTarget::CPUSim, true);
				if (FishHandle && FishHandle->GetInstance().Emitter && FishHandle->GetEmitterData())
				{
					RemoveDefaultRenderers(FishHandle->GetInstance().Emitter, FishHandle->GetEmitterData());
					AddPresetRenderer(FishHandle, TEXT("mesh"), TEXT("FishSphereMeshRenderer"), FString(), TEXT("/Engine/BasicShapes/Sphere.Sphere"));
					AddModule(FishHandle->GetEmitterData(), TEXT("FishSchool_MeshFish"), TEXT("/Niagara/Modules/Spawn/Location/SphereLocation.SphereLocation"), ENiagaraScriptUsage::ParticleSpawnScript);
					AddModule(FishHandle->GetEmitterData(), TEXT("FishSchool_MeshFish"), TEXT("/Niagara/Modules/Spawn/Velocity/AddVelocityInCone.AddVelocityInCone"), ENiagaraScriptUsage::ParticleSpawnScript);
					AddModule(FishHandle->GetEmitterData(), TEXT("FishSchool_MeshFish"), TEXT("/Niagara/Modules/Update/Forces/CurlNoiseForce.CurlNoiseForce"), ENiagaraScriptUsage::ParticleUpdateScript);
					AddModule(FishHandle->GetEmitterData(), TEXT("FishSchool_MeshFish"), TEXT("/Niagara/Modules/Solvers/SolveForcesAndVelocity.SolveForcesAndVelocity"), ENiagaraScriptUsage::ParticleUpdateScript);
					OperationDetails.Add(TPair<FString, FString>(TEXT("FishSchool_MeshFish"), TEXT("Mesh fish placeholder using sphere mesh, spawn sphere, cone velocity, curl-noise steering, and exposed fish-school tuning parameters.")));
				}
			}
			else if (bJetExhaust)
			{
				AddUserParameter(TEXT("ExhaustSpawnRate"), FNiagaraTypeDefinition::GetFloatDef(), MakeShared<FJsonValueNumber>(120.0));
				AddUserParameter(TEXT("ExhaustLength"), FNiagaraTypeDefinition::GetFloatDef(), MakeShared<FJsonValueNumber>(900.0));
				AddUserParameter(TEXT("ExhaustSpread"), FNiagaraTypeDefinition::GetFloatDef(), MakeShared<FJsonValueNumber>(18.0));
				AddUserParameter(TEXT("HeatIntensity"), FNiagaraTypeDefinition::GetFloatDef(), MakeShared<FJsonValueNumber>(1.0));
				AddUserParameter(TEXT("SmokeFadeTime"), FNiagaraTypeDefinition::GetFloatDef(), MakeShared<FJsonValueNumber>(2.0));

				FNiagaraEmitterHandle* CoreFlameHandle = ConfigureEmitter(TEXT("JetExhaust_CoreFlame"), ENiagaraSimTarget::CPUSim, true);
				if (CoreFlameHandle && CoreFlameHandle->GetEmitterData())
				{
					AddModule(CoreFlameHandle->GetEmitterData(), TEXT("JetExhaust_CoreFlame"), TEXT("/Niagara/Modules/Spawn/Location/ConeLocation.ConeLocation"), ENiagaraScriptUsage::ParticleSpawnScript);
					AddModule(CoreFlameHandle->GetEmitterData(), TEXT("JetExhaust_CoreFlame"), TEXT("/Niagara/Modules/Spawn/Velocity/AddVelocityInCone.AddVelocityInCone"), ENiagaraScriptUsage::ParticleSpawnScript);
					AddModule(CoreFlameHandle->GetEmitterData(), TEXT("JetExhaust_CoreFlame"), TEXT("/Niagara/Modules/Update/Color/ScaleColor.ScaleColor"), ENiagaraScriptUsage::ParticleUpdateScript);
					AddModule(CoreFlameHandle->GetEmitterData(), TEXT("JetExhaust_CoreFlame"), TEXT("/Niagara/Modules/Update/Size/ScaleSpriteSize.ScaleSpriteSize"), ENiagaraScriptUsage::ParticleUpdateScript);
					OperationDetails.Add(TPair<FString, FString>(TEXT("JetExhaust_CoreFlame"), TEXT("Dense sprite flame core with cone spawn, cone velocity, color fade, and sprite scale shaping.")));
				}

				FNiagaraEmitterHandle* HeatRibbonHandle = ConfigureEmitter(TEXT("JetExhaust_HeatRibbon"), ENiagaraSimTarget::CPUSim, true);
				if (HeatRibbonHandle && HeatRibbonHandle->GetInstance().Emitter && HeatRibbonHandle->GetEmitterData())
				{
					RemoveDefaultRenderers(HeatRibbonHandle->GetInstance().Emitter, HeatRibbonHandle->GetEmitterData());
					AddPresetRenderer(HeatRibbonHandle, TEXT("ribbon"), TEXT("HeatDistortionRibbonRenderer"), FString(), FString());
					AddModule(HeatRibbonHandle->GetEmitterData(), TEXT("JetExhaust_HeatRibbon"), TEXT("/Niagara/Modules/Spawn/Location/ConeLocation.ConeLocation"), ENiagaraScriptUsage::ParticleSpawnScript);
					AddModule(HeatRibbonHandle->GetEmitterData(), TEXT("JetExhaust_HeatRibbon"), TEXT("/Niagara/Modules/Spawn/Velocity/AddVelocityInCone.AddVelocityInCone"), ENiagaraScriptUsage::ParticleSpawnScript);
					OperationDetails.Add(TPair<FString, FString>(TEXT("JetExhaust_HeatRibbon"), TEXT("Ribbon layer for exhaust heat shimmer / long tail shaping.")));
				}
			}
			else if (bSonicBoom)
			{
				AddUserParameter(TEXT("ConeRadius"), FNiagaraTypeDefinition::GetFloatDef(), MakeShared<FJsonValueNumber>(650.0));
				AddUserParameter(TEXT("ConeLength"), FNiagaraTypeDefinition::GetFloatDef(), MakeShared<FJsonValueNumber>(1200.0));
				AddUserParameter(TEXT("VaporDensity"), FNiagaraTypeDefinition::GetFloatDef(), MakeShared<FJsonValueNumber>(0.65));
				AddUserParameter(TEXT("ShockStrength"), FNiagaraTypeDefinition::GetFloatDef(), MakeShared<FJsonValueNumber>(1.0));
				AddUserParameter(TEXT("DissolveTime"), FNiagaraTypeDefinition::GetFloatDef(), MakeShared<FJsonValueNumber>(0.75));

				FNiagaraEmitterHandle* ConeHandle = ConfigureEmitter(TEXT("SonicBoom_VaporCone"), ENiagaraSimTarget::CPUSim, true);
				if (ConeHandle && ConeHandle->GetEmitterData())
				{
					AddModule(ConeHandle->GetEmitterData(), TEXT("SonicBoom_VaporCone"), TEXT("/Niagara/Modules/Spawn/Location/ConeLocation.ConeLocation"), ENiagaraScriptUsage::ParticleSpawnScript);
					AddModule(ConeHandle->GetEmitterData(), TEXT("SonicBoom_VaporCone"), TEXT("/Niagara/Modules/Spawn/Velocity/AddVelocityInCone.AddVelocityInCone"), ENiagaraScriptUsage::ParticleSpawnScript);
					AddModule(ConeHandle->GetEmitterData(), TEXT("SonicBoom_VaporCone"), TEXT("/Niagara/Modules/Update/Color/ScaleColor.ScaleColor"), ENiagaraScriptUsage::ParticleUpdateScript);
					AddModule(ConeHandle->GetEmitterData(), TEXT("SonicBoom_VaporCone"), TEXT("/Niagara/Modules/Update/Size/ScaleSpriteSize.ScaleSpriteSize"), ENiagaraScriptUsage::ParticleUpdateScript);
					OperationDetails.Add(TPair<FString, FString>(TEXT("SonicBoom_VaporCone"), TEXT("Short-lived cone sprite layer for vapor condensation around a fast aircraft.")));
				}

				FNiagaraEmitterHandle* ShockRingHandle = ConfigureEmitter(TEXT("SonicBoom_ShockRing"), ENiagaraSimTarget::CPUSim, true);
				if (ShockRingHandle && ShockRingHandle->GetInstance().Emitter && ShockRingHandle->GetEmitterData())
				{
					RemoveDefaultRenderers(ShockRingHandle->GetInstance().Emitter, ShockRingHandle->GetEmitterData());
					AddPresetRenderer(ShockRingHandle, TEXT("ribbon"), TEXT("ShockRingRibbonRenderer"), FString(), FString());
					AddModule(ShockRingHandle->GetEmitterData(), TEXT("SonicBoom_ShockRing"), TEXT("/Niagara/Modules/Spawn/Location/SphereLocation.SphereLocation"), ENiagaraScriptUsage::ParticleSpawnScript);
					OperationDetails.Add(TPair<FString, FString>(TEXT("SonicBoom_ShockRing"), TEXT("Ribbon ring layer for shock boundary visualization.")));
				}
			}

			System->RequestCompile(true);
			NotifyAssetCreated(System);
			SaveAssetIfRequested(System, ReadBoolParam(Params, TEXT("save"), true));

			if (OperationDetails.Num() == 0)
			{
				OperationDetails.Add(TPair<FString, FString>(AssetName, FString::Printf(TEXT("Created Niagara preset %s."), *PresetKey)));
			}
			Core.RecordBlueprintChange(System->GetPathName(), TEXT("Create Niagara preset"), OperationDetails);

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), System->GetPathName());
			Result->SetStringField(TEXT("preset"), PresetKey);
			Result->SetArrayField(TEXT("emitters"), AddedEmitters);
			Result->SetArrayField(TEXT("modules"), AddedModules);
			Result->SetArrayField(TEXT("renderers"), AddedRenderers);
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}

		TSharedPtr<FJsonObject> CompileSystem(FWebFrameMcpModule& Core, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			FString SystemPath;
			if (!GetStringParam(Params, TEXT("asset"), SystemPath, OutError))
			{
				return nullptr;
			}

			UNiagaraSystem* System = LoadSystem(SystemPath, OutError);
			if (!System)
			{
				return nullptr;
			}

			const bool bRequested = System->RequestCompile(true);
			if (ReadBoolParam(Params, TEXT("wait"), false))
			{
				System->WaitForCompilationComplete(false, true);
			}

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), System->GetPathName());
			Result->SetBoolField(TEXT("requested"), bRequested);
			RecordOperation(Core, System->GetPathName(), TEXT("Compile Niagara system"), System->GetName(), TEXT("Requested NiagaraSystem compile."));
			Result->SetBoolField(TEXT("operationRecorded"), true);
			return Result;
		}
	};
}

IMPLEMENT_MODULE(FWebFrameMcpNiagaraModule, WebFrameMcpNiagara)
