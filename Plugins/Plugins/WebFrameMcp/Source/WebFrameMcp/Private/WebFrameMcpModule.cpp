// Copyright Epic Games, Inc. All Rights Reserved.

#include "WebFrameMcpModule.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Animation/Skeleton.h"
#include "IAssetTools.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Brushes/SlateImageBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphNodeUtils.h"
#include "EdGraphNode_Comment.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Selection.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/Scene.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Engine/BlendableInterface.h"
#include "HAL/FileManager.h"
#include "IWebSocketNetworkingModule.h"
#include "IWebSocketServer.h"
#include "INetworkingWebSocket.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LevelEditor.h"
#include "GameFramework/Pawn.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Event.h"
#include "K2Node_Self.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialDomain.h"
#include "MaterialEditingLibrary.h"
#include "Components/MeshComponent.h"
#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/Texture.h"
#include "StaticMeshResources.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Misc/FileHelper.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "PlayInEditorDataTypes.h"
#include "UObject/UnrealType.h"

#include <initializer_list>

#define LOCTEXT_NAMESPACE "FWebFrameMcpModule"

DEFINE_LOG_CATEGORY_STATIC(LogWebFrameMcp, Log, All);

namespace WebFrameMcp
{
	static const FName TabName(TEXT("WebFrameMcpPanel"));
	static const FName StyleName(TEXT("WebFrameMcpStyle"));

	static FString JsonToString(const TSharedPtr<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Out;
	}

	static bool JsonValueToString(const TSharedPtr<FJsonValue>& Value, FString& Out)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::String)
		{
			Out = Value->AsString();
			return true;
		}
		if (Value->Type == EJson::Number)
		{
			const double Number = Value->AsNumber();
			const int64 IntegerNumber = static_cast<int64>(Number);
			Out = FMath::IsNearlyEqual(Number, static_cast<double>(IntegerNumber))
				? LexToString(IntegerNumber)
				: FString::SanitizeFloat(Number);
			return true;
		}
		if (Value->Type == EJson::Boolean)
		{
			Out = Value->AsBool() ? TEXT("true") : TEXT("false");
			return true;
		}

		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Value, TEXT(""), Writer);
		return true;
	}

	static TSharedPtr<FJsonObject> MakeError(const FString& Message)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("message"), Message);
		return Object;
	}

	static void SendJson(INetworkingWebSocket* Peer, const FString& Json)
	{
		if (!Peer)
		{
			return;
		}
		const FTCHARToUTF8 Utf8(*Json);
		Peer->Send(reinterpret_cast<const uint8*>(Utf8.Get()), static_cast<uint32>(Utf8.Length()), false);
	}

	static FString PinDirectionToString(EEdGraphPinDirection Direction)
	{
		return Direction == EGPD_Input ? TEXT("input") : TEXT("output");
	}

	static TSharedPtr<FJsonObject> PinToJson(const UEdGraphPin* Pin)
	{
		TSharedPtr<FJsonObject> PinJson = MakeShared<FJsonObject>();
		if (!Pin)
		{
			return PinJson;
		}

		PinJson->SetStringField(TEXT("id"), Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
		PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
		PinJson->SetStringField(TEXT("direction"), PinDirectionToString(Pin->Direction));
		PinJson->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		PinJson->SetStringField(TEXT("subCategory"), Pin->PinType.PinSubCategory.ToString());
		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			PinJson->SetStringField(TEXT("subCategoryObject"), Pin->PinType.PinSubCategoryObject->GetPathName());
		}
		if (!Pin->DefaultValue.IsEmpty())
		{
			PinJson->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
		}

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
		PinJson->SetArrayField(TEXT("linkedTo"), LinkedTo);
		return PinJson;
	}

	static TSharedPtr<FJsonObject> NodeToJson(const UEdGraphNode* Node)
	{
		TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
		if (!Node)
		{
			return NodeJson;
		}

		NodeJson->SetStringField(TEXT("id"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		NodeJson->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
		NodeJson->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		NodeJson->SetStringField(TEXT("comment"), Node->NodeComment);
		NodeJson->SetNumberField(TEXT("x"), Node->NodePosX);
		NodeJson->SetNumberField(TEXT("y"), Node->NodePosY);

		TArray<TSharedPtr<FJsonValue>> Pins;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			Pins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin)));
		}
		NodeJson->SetArrayField(TEXT("pins"), Pins);
		return NodeJson;
	}

	static TSharedPtr<FJsonObject> GraphToJson(const UEdGraph* Graph)
	{
		TSharedPtr<FJsonObject> GraphJson = MakeShared<FJsonObject>();
		if (!Graph)
		{
			return GraphJson;
		}

		GraphJson->SetStringField(TEXT("name"), Graph->GetName());
		GraphJson->SetStringField(TEXT("path"), Graph->GetPathName());
		GraphJson->SetStringField(TEXT("schema"), Graph->Schema ? Graph->Schema->GetClass()->GetPathName() : FString());

		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node)
			{
				Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Node)));
			}
		}
		GraphJson->SetArrayField(TEXT("nodes"), Nodes);
		return GraphJson;
	}

	static int32 CountNodes(const UBlueprint* Blueprint)
	{
		if (!Blueprint)
		{
			return 0;
		}

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		int32 Count = 0;
		for (const UEdGraph* Graph : Graphs)
		{
			Count += Graph ? Graph->Nodes.Num() : 0;
		}
		return Count;
	}

	static FString AssetNameFromPath(const FString& AssetPath)
	{
		FString PackagePart;
		FString ObjectPart;
		if (AssetPath.Split(TEXT("."), &PackagePart, &ObjectPart, ESearchCase::CaseSensitive, ESearchDir::FromEnd) && !ObjectPart.IsEmpty())
		{
			return ObjectPart;
		}
		return FPaths::GetBaseFilename(AssetPath);
	}

	static bool IsAllowedGamePath(const FString& InPath)
	{
		return InPath == TEXT("/Game") || InPath.StartsWith(TEXT("/Game/"));
	}

	static FString NormalizeGamePath(FString InPath)
	{
		if (InPath.IsEmpty())
		{
			return TEXT("/Game");
		}
		InPath.TrimStartAndEndInline();
		InPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (InPath.StartsWith(TEXT("/All/Game")))
		{
			InPath = FString(TEXT("/Game")) + InPath.Mid(9);
		}
		InPath.RemoveFromEnd(TEXT("/"));
		return InPath.IsEmpty() ? FString(TEXT("/Game")) : InPath;
	}

	static FString NormalizeMcpPermissionPath(FString InPath)
	{
		InPath.TrimStartAndEndInline();
		InPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (InPath.StartsWith(TEXT("/All/Game")))
		{
			InPath = FString(TEXT("/Game")) + InPath.Mid(9);
		}
		InPath.RemoveFromEnd(TEXT("/"));
		return InPath;
	}

	static bool IsMcpPermissionPath(const FString& InPath)
	{
		const FString Path = NormalizeMcpPermissionPath(InPath);
		return Path == TEXT("/Game") || Path.StartsWith(TEXT("/Game/"));
	}

	static void CollectMcpPermissionPathsFromJsonValue(const TSharedPtr<FJsonValue>& Value, TArray<FString>& OutPaths);

	static void CollectMcpPermissionPathsFromJsonObject(const TSharedPtr<FJsonObject>& Object, TArray<FString>& OutPaths)
	{
		if (!Object.IsValid())
		{
			return;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			CollectMcpPermissionPathsFromJsonValue(Pair.Value, OutPaths);
		}
	}

	static void CollectMcpPermissionPathsFromJsonValue(const TSharedPtr<FJsonValue>& Value, TArray<FString>& OutPaths)
	{
		if (!Value.IsValid())
		{
			return;
		}

		if (Value->Type == EJson::String)
		{
			const FString Path = NormalizeMcpPermissionPath(Value->AsString());
			if (IsMcpPermissionPath(Path))
			{
				OutPaths.AddUnique(Path);
			}
			return;
		}

		if (Value->Type == EJson::Object)
		{
			CollectMcpPermissionPathsFromJsonObject(Value->AsObject(), OutPaths);
			return;
		}

		if (Value->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Child : Value->AsArray())
			{
				CollectMcpPermissionPathsFromJsonValue(Child, OutPaths);
			}
		}
	}

	static FString PinTypeToString(const FEdGraphPinType& PinType)
	{
		FString Type = PinType.PinCategory.ToString();
		if (!PinType.PinSubCategory.IsNone())
		{
			Type += TEXT(".");
			Type += PinType.PinSubCategory.ToString();
		}
		if (PinType.PinSubCategoryObject.IsValid())
		{
			Type += TEXT(" ");
			Type += PinType.PinSubCategoryObject->GetPathName();
		}
		if (PinType.IsArray())
		{
			Type += TEXT("[]");
		}
		else if (PinType.IsSet())
		{
			Type += TEXT("<set>");
		}
		else if (PinType.IsMap())
		{
			Type += TEXT("<map>");
		}
		return Type;
	}

	static FEdGraphPinType ReadPinType(const TSharedPtr<FJsonObject>& Params)
	{
		FString Category;
		FString SubCategory;
		FString SubCategoryObjectPath;
		bool bArray = false;
		bool bSet = false;
		bool bMap = false;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("pinCategory"), Category);
			Params->TryGetStringField(TEXT("pinSubCategory"), SubCategory);
			Params->TryGetStringField(TEXT("pinSubCategoryObject"), SubCategoryObjectPath);
			Params->TryGetBoolField(TEXT("isArray"), bArray);
			Params->TryGetBoolField(TEXT("isSet"), bSet);
			Params->TryGetBoolField(TEXT("isMap"), bMap);
		}
		if (Category.IsEmpty())
		{
			Category = UEdGraphSchema_K2::PC_Boolean.ToString();
		}

		FEdGraphPinType PinType;
		PinType.PinCategory = *Category;
		PinType.PinSubCategory = *SubCategory;
		if (!SubCategoryObjectPath.IsEmpty())
		{
			PinType.PinSubCategoryObject = LoadObject<UObject>(nullptr, *SubCategoryObjectPath);
		}
		if (bArray)
		{
			PinType.ContainerType = EPinContainerType::Array;
		}
		else if (bSet)
		{
			PinType.ContainerType = EPinContainerType::Set;
		}
		else if (bMap)
		{
			PinType.ContainerType = EPinContainerType::Map;
		}
		return PinType;
	}

	static TSharedPtr<FJsonObject> VariableToJson(const FBPVariableDescription& Variable)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Variable.VarName.ToString());
		Item->SetStringField(TEXT("guid"), Variable.VarGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Item->SetStringField(TEXT("type"), PinTypeToString(Variable.VarType));
		Item->SetStringField(TEXT("category"), Variable.Category.ToString());
		Item->SetStringField(TEXT("defaultValue"), Variable.DefaultValue);
		Item->SetBoolField(TEXT("editable"), (Variable.PropertyFlags & CPF_Edit) != 0);
		Item->SetBoolField(TEXT("exposeOnSpawn"), Variable.HasMetaData(TEXT("ExposeOnSpawn")));
		return Item;
	}

	static UMaterialExpression* FindMaterialExpression(UMaterial* Material, const FString& ExpressionName)
	{
		if (!Material || ExpressionName.IsEmpty())
		{
			return nullptr;
		}

		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			if (Expression && Expression->GetName() == ExpressionName)
			{
				return Expression;
			}
		}
		return nullptr;
	}

	static UClass* ResolveMaterialExpressionClass(const FString& TypeOrClassPath)
	{
		if (TypeOrClassPath.IsEmpty())
		{
			return nullptr;
		}

		const TMap<FString, FString> Aliases = {
			{ TEXT("add"), TEXT("/Script/Engine.MaterialExpressionAdd") },
			{ TEXT("subtract"), TEXT("/Script/Engine.MaterialExpressionSubtract") },
			{ TEXT("multiply"), TEXT("/Script/Engine.MaterialExpressionMultiply") },
			{ TEXT("divide"), TEXT("/Script/Engine.MaterialExpressionDivide") },
			{ TEXT("lerp"), TEXT("/Script/Engine.MaterialExpressionLinearInterpolate") },
			{ TEXT("linearinterpolate"), TEXT("/Script/Engine.MaterialExpressionLinearInterpolate") },
			{ TEXT("constant"), TEXT("/Script/Engine.MaterialExpressionConstant") },
			{ TEXT("constant2"), TEXT("/Script/Engine.MaterialExpressionConstant2Vector") },
			{ TEXT("constant3"), TEXT("/Script/Engine.MaterialExpressionConstant3Vector") },
			{ TEXT("constant4"), TEXT("/Script/Engine.MaterialExpressionConstant4Vector") },
			{ TEXT("scalarparameter"), TEXT("/Script/Engine.MaterialExpressionScalarParameter") },
			{ TEXT("vectorparameter"), TEXT("/Script/Engine.MaterialExpressionVectorParameter") },
			{ TEXT("texture"), TEXT("/Script/Engine.MaterialExpressionTextureSample") },
			{ TEXT("texturesample"), TEXT("/Script/Engine.MaterialExpressionTextureSample") },
			{ TEXT("textureparameter"), TEXT("/Script/Engine.MaterialExpressionTextureSampleParameter2D") },
			{ TEXT("texcoord"), TEXT("/Script/Engine.MaterialExpressionTextureCoordinate") },
			{ TEXT("panner"), TEXT("/Script/Engine.MaterialExpressionPanner") },
			{ TEXT("time"), TEXT("/Script/Engine.MaterialExpressionTime") },
			{ TEXT("sine"), TEXT("/Script/Engine.MaterialExpressionSine") },
			{ TEXT("cosine"), TEXT("/Script/Engine.MaterialExpressionCosine") },
			{ TEXT("power"), TEXT("/Script/Engine.MaterialExpressionPower") },
			{ TEXT("oneminus"), TEXT("/Script/Engine.MaterialExpressionOneMinus") },
			{ TEXT("componentmask"), TEXT("/Script/Engine.MaterialExpressionComponentMask") },
			{ TEXT("append"), TEXT("/Script/Engine.MaterialExpressionAppendVector") },
			{ TEXT("fresnel"), TEXT("/Script/Engine.MaterialExpressionFresnel") },
			{ TEXT("desaturation"), TEXT("/Script/Engine.MaterialExpressionDesaturation") },
			{ TEXT("dot"), TEXT("/Script/Engine.MaterialExpressionDotProduct") },
			{ TEXT("cross"), TEXT("/Script/Engine.MaterialExpressionCrossProduct") },
			{ TEXT("normalize"), TEXT("/Script/Engine.MaterialExpressionNormalize") },
			{ TEXT("worldposition"), TEXT("/Script/Engine.MaterialExpressionWorldPosition") },
			{ TEXT("world_position"), TEXT("/Script/Engine.MaterialExpressionWorldPosition") },
			{ TEXT("absoluteworldposition"), TEXT("/Script/Engine.MaterialExpressionWorldPosition") },
			{ TEXT("scenetexture"), TEXT("/Script/Engine.MaterialExpressionSceneTexture") },
			{ TEXT("scene_texture"), TEXT("/Script/Engine.MaterialExpressionSceneTexture") },
			{ TEXT("postprocessinput"), TEXT("/Script/Engine.MaterialExpressionSceneTexture") },
			{ TEXT("post_process_input"), TEXT("/Script/Engine.MaterialExpressionSceneTexture") },
			{ TEXT("customdepth"), TEXT("/Script/Engine.MaterialExpressionSceneTexture") },
			{ TEXT("custom_depth"), TEXT("/Script/Engine.MaterialExpressionSceneTexture") },
			{ TEXT("distance"), TEXT("/Script/Engine.MaterialExpressionDistance") },
			{ TEXT("step"), TEXT("/Script/Engine.MaterialExpressionStep") },
			{ TEXT("clamp"), TEXT("/Script/Engine.MaterialExpressionClamp") },
			{ TEXT("saturate"), TEXT("/Script/Engine.MaterialExpressionSaturate") }
		};

		FString ClassPath = TypeOrClassPath;
		const FString Key = TypeOrClassPath.ToLower();
		if (const FString* AliasPath = Aliases.Find(Key))
		{
			ClassPath = *AliasPath;
		}
		return LoadClass<UMaterialExpression>(nullptr, *ClassPath);
	}

	static bool ParseMaterialProperty(const FString& Name, EMaterialProperty& OutProperty)
	{
		const FString Key = Name.ToLower();
		if (Key == TEXT("basecolor") || Key == TEXT("base_color")) { OutProperty = MP_BaseColor; return true; }
		if (Key == TEXT("metallic")) { OutProperty = MP_Metallic; return true; }
		if (Key == TEXT("specular")) { OutProperty = MP_Specular; return true; }
		if (Key == TEXT("roughness")) { OutProperty = MP_Roughness; return true; }
		if (Key == TEXT("emissive") || Key == TEXT("emissivecolor")) { OutProperty = MP_EmissiveColor; return true; }
		if (Key == TEXT("opacity")) { OutProperty = MP_Opacity; return true; }
		if (Key == TEXT("opacitymask")) { OutProperty = MP_OpacityMask; return true; }
		if (Key == TEXT("normal")) { OutProperty = MP_Normal; return true; }
		if (Key == TEXT("worldpositionoffset")) { OutProperty = MP_WorldPositionOffset; return true; }
		if (Key == TEXT("ambientocclusion") || Key == TEXT("ao")) { OutProperty = MP_AmbientOcclusion; return true; }

		if (const UEnum* Enum = StaticEnum<EMaterialProperty>())
		{
			const int64 Value = Enum->GetValueByNameString(Name);
			if (Value != INDEX_NONE)
			{
				OutProperty = static_cast<EMaterialProperty>(Value);
				return true;
			}
		}
		return false;
	}

	static int32 FindMaterialExpressionOutputIndex(UMaterialExpression* Expression, const FString& OutputName)
	{
		if (!Expression)
		{
			return INDEX_NONE;
		}
		if (Expression->GetOutputs().Num() == 0)
		{
			return INDEX_NONE;
		}
		if (OutputName.IsEmpty())
		{
			return 0;
		}

		for (int32 Index = 0; Index < Expression->GetOutputs().Num(); ++Index)
		{
			const FExpressionOutput& Output = Expression->GetOutputs()[Index];
			if (!Output.OutputName.IsNone() && Output.OutputName.ToString().Equals(OutputName, ESearchCase::IgnoreCase))
			{
				return Index;
			}
			if (Output.OutputName.IsNone())
			{
				if ((Output.MaskR && !Output.MaskG && !Output.MaskB && !Output.MaskA && OutputName.Equals(TEXT("R"), ESearchCase::IgnoreCase)) ||
					(!Output.MaskR && Output.MaskG && !Output.MaskB && !Output.MaskA && OutputName.Equals(TEXT("G"), ESearchCase::IgnoreCase)) ||
					(!Output.MaskR && !Output.MaskG && Output.MaskB && !Output.MaskA && OutputName.Equals(TEXT("B"), ESearchCase::IgnoreCase)) ||
					(!Output.MaskR && !Output.MaskG && !Output.MaskB && Output.MaskA && OutputName.Equals(TEXT("A"), ESearchCase::IgnoreCase)))
				{
					return Index;
				}
			}
		}
		return INDEX_NONE;
	}

	static FExpressionInput* FindMaterialExpressionInputByMemberName(UMaterialExpression* Expression, const FString& InputName)
	{
		if (!Expression)
		{
			return nullptr;
		}
		if (UMaterialExpressionCustom* CustomExpression = Cast<UMaterialExpressionCustom>(Expression))
		{
			if (InputName.IsEmpty())
			{
				return CustomExpression->Inputs.Num() > 0 ? &CustomExpression->Inputs[0].Input : nullptr;
			}
			for (FCustomInput& CustomInput : CustomExpression->Inputs)
			{
				if (CustomInput.InputName.ToString().Equals(InputName, ESearchCase::IgnoreCase))
				{
					return &CustomInput.Input;
				}
			}
		}
		if (InputName.IsEmpty())
		{
			return Expression->GetInput(0);
		}

		for (TFieldIterator<FStructProperty> It(Expression->GetClass()); It; ++It)
		{
			FStructProperty* Property = *It;
			if (!Property || Property->Struct->GetName() != TEXT("ExpressionInput"))
			{
				continue;
			}
			if (Property->GetName().Equals(InputName, ESearchCase::IgnoreCase))
			{
				return Property->ContainerPtrToValuePtr<FExpressionInput>(Expression);
			}
		}
		return nullptr;
	}

	static void ForEachMaterialExpressionInput(UMaterialExpression* Expression, TFunctionRef<void(const FString&, FExpressionInput*)> Visitor)
	{
		if (!Expression)
		{
			return;
		}

		if (UMaterialExpressionCustom* CustomExpression = Cast<UMaterialExpressionCustom>(Expression))
		{
			for (FCustomInput& CustomInput : CustomExpression->Inputs)
			{
				Visitor(CustomInput.InputName.ToString(), &CustomInput.Input);
			}
		}

		for (TFieldIterator<FStructProperty> It(Expression->GetClass()); It; ++It)
		{
			FStructProperty* Property = *It;
			if (!Property || Property->Struct->GetName() != TEXT("ExpressionInput"))
			{
				continue;
			}
			Visitor(Property->GetName(), Property->ContainerPtrToValuePtr<FExpressionInput>(Expression));
		}
	}

	static bool DoesExpressionInputMatch(FExpressionInput* Input, UMaterialExpression* FromExpression, const FString& FromOutputName)
	{
		if (!Input || !Input->Expression)
		{
			return false;
		}
		if (!FromExpression)
		{
			return true;
		}
		if (Input->Expression != FromExpression)
		{
			return false;
		}
		if (FromOutputName.IsEmpty())
		{
			return true;
		}
		const int32 OutputIndex = FindMaterialExpressionOutputIndex(FromExpression, FromOutputName);
		return OutputIndex != INDEX_NONE && Input->OutputIndex == OutputIndex;
	}

	static bool ConnectMaterialExpressionsCompat(UMaterialExpression* FromExpression, const FString& FromOutputName, UMaterialExpression* ToExpression, const FString& ToInputName)
	{
		if (UMaterialEditingLibrary::ConnectMaterialExpressions(FromExpression, FromOutputName, ToExpression, ToInputName))
		{
			return true;
		}

		FExpressionInput* Input = FindMaterialExpressionInputByMemberName(ToExpression, ToInputName);
		const int32 OutputIndex = FindMaterialExpressionOutputIndex(FromExpression, FromOutputName);
		if (!Input || OutputIndex == INDEX_NONE)
		{
			return false;
		}

		Input->Connect(OutputIndex, FromExpression);
		if (ToExpression)
		{
			ToExpression->Modify();
		}
		return true;
	}

	static bool ConnectMaterialExpressionsForce(UMaterialExpression* FromExpression, const FString& FromOutputName, UMaterialExpression* ToExpression, const FString& ToInputName)
	{
		FExpressionInput* Input = FindMaterialExpressionInputByMemberName(ToExpression, ToInputName);
		const int32 OutputIndex = FindMaterialExpressionOutputIndex(FromExpression, FromOutputName);
		if (!Input || OutputIndex == INDEX_NONE)
		{
			return false;
		}

		*Input = FExpressionInput();
		Input->Connect(OutputIndex, FromExpression);
		if (ToExpression)
		{
			ToExpression->Modify();
		}
		return true;
	}

	static bool ConnectMaterialPropertyForce(UMaterial* Material, UMaterialExpression* FromExpression, const FString& FromOutputName, EMaterialProperty Property)
	{
		if (!Material || !FromExpression)
		{
			return false;
		}

		FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
		const int32 OutputIndex = FindMaterialExpressionOutputIndex(FromExpression, FromOutputName);
		if (!Input || OutputIndex == INDEX_NONE)
		{
			return false;
		}

		*Input = FExpressionInput();
		Input->Connect(OutputIndex, FromExpression);
		Material->Modify();
		return true;
	}

	static FString NormalizeSettingKey(FString Value)
	{
		Value.TrimStartAndEndInline();
		Value.ToLowerInline();
		Value.ReplaceInline(TEXT(" "), TEXT(""));
		Value.ReplaceInline(TEXT("_"), TEXT(""));
		Value.ReplaceInline(TEXT("-"), TEXT(""));
		Value.ReplaceInline(TEXT("."), TEXT(""));
		return Value;
	}

	static bool TryParseEnumValue(const UEnum* Enum, const FString& Value, int64& OutValue)
	{
		if (!Enum || Value.IsEmpty())
		{
			return false;
		}

		OutValue = Enum->GetValueByNameString(Value);
		if (OutValue != INDEX_NONE)
		{
			return true;
		}

		if (Value.IsNumeric())
		{
			OutValue = FCString::Atoi64(*Value);
			return Enum->IsValidEnumValue(OutValue);
		}

		const FString Key = NormalizeSettingKey(Value);
		for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
		{
			if (Enum->HasMetaData(TEXT("Hidden"), Index))
			{
				continue;
			}

			const int64 CandidateValue = Enum->GetValueByIndex(Index);
			if (!Enum->IsValidEnumValue(CandidateValue))
			{
				continue;
			}

			const FString Name = Enum->GetNameStringByIndex(Index);
			const FString DisplayName = Enum->GetDisplayNameTextByIndex(Index).ToString();
			if (NormalizeSettingKey(Name) == Key || NormalizeSettingKey(DisplayName) == Key)
			{
				OutValue = CandidateValue;
				return true;
			}

			FString Prefix;
			FString Suffix;
			if (Name.Split(TEXT("_"), &Prefix, &Suffix) && NormalizeSettingKey(Suffix) == Key)
			{
				OutValue = CandidateValue;
				return true;
			}
		}
		return false;
	}

	template<typename EnumType>
	static FString EnumValueToString(EnumType Value)
	{
		if (const UEnum* Enum = StaticEnum<EnumType>())
		{
			return Enum->GetNameStringByValue(static_cast<int64>(Value));
		}
		return FString::FromInt(static_cast<int32>(Value));
	}

	static bool ParseMaterialDomain(const FString& Value, EMaterialDomain& OutDomain)
	{
		const FString Key = NormalizeSettingKey(Value);
		if (Key == TEXT("surface") || Key == TEXT("normal") || Key == TEXT("default") || Key == TEXT("standard"))
		{
			OutDomain = MD_Surface;
			return true;
		}
		if (Key == TEXT("postprocess") || Key == TEXT("post") || Key == TEXT("pp"))
		{
			OutDomain = MD_PostProcess;
			return true;
		}
		if (Key == TEXT("ui") || Key == TEXT("userinterface") || Key == TEXT("umg") || Key == TEXT("slate"))
		{
			OutDomain = MD_UI;
			return true;
		}
		if (Key == TEXT("decal") || Key == TEXT("deferreddecal"))
		{
			OutDomain = MD_DeferredDecal;
			return true;
		}
		if (Key == TEXT("lightfunction"))
		{
			OutDomain = MD_LightFunction;
			return true;
		}
		if (Key == TEXT("volume"))
		{
			OutDomain = MD_Volume;
			return true;
		}

		int64 EnumValue = INDEX_NONE;
		if (TryParseEnumValue(StaticEnum<EMaterialDomain>(), Value, EnumValue))
		{
			OutDomain = static_cast<EMaterialDomain>(EnumValue);
			return OutDomain != MD_MAX;
		}
		return false;
	}

	static bool ParseBlendMode(const FString& Value, EBlendMode& OutBlendMode)
	{
		const FString Key = NormalizeSettingKey(Value);
		if (Key == TEXT("opaque") || Key == TEXT("solid"))
		{
			OutBlendMode = BLEND_Opaque;
			return true;
		}
		if (Key == TEXT("masked") || Key == TEXT("mask") || Key == TEXT("cutout"))
		{
			OutBlendMode = BLEND_Masked;
			return true;
		}
		if (Key == TEXT("translucent") || Key == TEXT("transparent") || Key == TEXT("transparency") || Key == TEXT("alpha"))
		{
			OutBlendMode = BLEND_Translucent;
			return true;
		}
		if (Key == TEXT("additive"))
		{
			OutBlendMode = BLEND_Additive;
			return true;
		}
		if (Key == TEXT("modulate"))
		{
			OutBlendMode = BLEND_Modulate;
			return true;
		}
		if (Key == TEXT("alphacomposite") || Key == TEXT("premultipliedalpha") || Key == TEXT("premultiplied"))
		{
			OutBlendMode = BLEND_AlphaComposite;
			return true;
		}
		if (Key == TEXT("alphaholdout"))
		{
			OutBlendMode = BLEND_AlphaHoldout;
			return true;
		}
		if (Key == TEXT("coloredtransmittance") || Key == TEXT("translucentcoloredtransmittance"))
		{
			OutBlendMode = BLEND_TranslucentColoredTransmittance;
			return true;
		}

		int64 EnumValue = INDEX_NONE;
		if (TryParseEnumValue(StaticEnum<EBlendMode>(), Value, EnumValue))
		{
			OutBlendMode = static_cast<EBlendMode>(EnumValue);
			return OutBlendMode != BLEND_MAX;
		}
		return false;
	}

	static bool ParseShadingModel(const FString& Value, EMaterialShadingModel& OutShadingModel)
	{
		const FString Key = NormalizeSettingKey(Value);
		if (Key == TEXT("lit") || Key == TEXT("defaultlit") || Key == TEXT("default"))
		{
			OutShadingModel = MSM_DefaultLit;
			return true;
		}
		if (Key == TEXT("unlit"))
		{
			OutShadingModel = MSM_Unlit;
			return true;
		}
		if (Key == TEXT("subsurface"))
		{
			OutShadingModel = MSM_Subsurface;
			return true;
		}
		if (Key == TEXT("preintegratedskin"))
		{
			OutShadingModel = MSM_PreintegratedSkin;
			return true;
		}
		if (Key == TEXT("clearcoat"))
		{
			OutShadingModel = MSM_ClearCoat;
			return true;
		}
		if (Key == TEXT("subsurfaceprofile"))
		{
			OutShadingModel = MSM_SubsurfaceProfile;
			return true;
		}
		if (Key == TEXT("twosidedfoliage") || Key == TEXT("foliage"))
		{
			OutShadingModel = MSM_TwoSidedFoliage;
			return true;
		}
		if (Key == TEXT("hair"))
		{
			OutShadingModel = MSM_Hair;
			return true;
		}
		if (Key == TEXT("cloth"))
		{
			OutShadingModel = MSM_Cloth;
			return true;
		}
		if (Key == TEXT("eye"))
		{
			OutShadingModel = MSM_Eye;
			return true;
		}
		if (Key == TEXT("singlelayerwater") || Key == TEXT("water"))
		{
			OutShadingModel = MSM_SingleLayerWater;
			return true;
		}
		if (Key == TEXT("thintranslucent"))
		{
			OutShadingModel = MSM_ThinTranslucent;
			return true;
		}
		if (Key == TEXT("frommaterialexpression") || Key == TEXT("expression"))
		{
			OutShadingModel = MSM_FromMaterialExpression;
			return true;
		}

		int64 EnumValue = INDEX_NONE;
		if (TryParseEnumValue(StaticEnum<EMaterialShadingModel>(), Value, EnumValue))
		{
			OutShadingModel = static_cast<EMaterialShadingModel>(EnumValue);
			return OutShadingModel != MSM_MAX && OutShadingModel != MSM_NUM;
		}
		return false;
	}

	static bool ParseTranslucencyLightingMode(const FString& Value, ETranslucencyLightingMode& OutMode)
	{
		const FString Key = NormalizeSettingKey(Value);
		if (Key == TEXT("volumetric") || Key == TEXT("volumetricnondirectional"))
		{
			OutMode = TLM_VolumetricNonDirectional;
			return true;
		}
		if (Key == TEXT("volumetricdirectional"))
		{
			OutMode = TLM_VolumetricDirectional;
			return true;
		}
		if (Key == TEXT("pervertex") || Key == TEXT("volumetricpervertex") || Key == TEXT("volumetricpervertexnondirectional"))
		{
			OutMode = TLM_VolumetricPerVertexNonDirectional;
			return true;
		}
		if (Key == TEXT("volumetricpervertexdirectional"))
		{
			OutMode = TLM_VolumetricPerVertexDirectional;
			return true;
		}
		if (Key == TEXT("surface") || Key == TEXT("surfacetranslucencyvolume"))
		{
			OutMode = TLM_Surface;
			return true;
		}
		if (Key == TEXT("surfaceforward") || Key == TEXT("surfaceforwardshading") || Key == TEXT("surfaceperpixellighting"))
		{
			OutMode = TLM_SurfacePerPixelLighting;
			return true;
		}

		int64 EnumValue = INDEX_NONE;
		if (TryParseEnumValue(StaticEnum<ETranslucencyLightingMode>(), Value, EnumValue))
		{
			OutMode = static_cast<ETranslucencyLightingMode>(EnumValue);
			return OutMode != TLM_MAX;
		}
		return false;
	}

	static bool ParseBlendableLocation(const FString& Value, EBlendableLocation& OutLocation)
	{
		const FString Key = NormalizeSettingKey(Value);
		if (Key == TEXT("beforedof") || Key == TEXT("scenecolorbeforedof"))
		{
			OutLocation = BL_SceneColorBeforeDOF;
			return true;
		}
		if (Key == TEXT("afterdof") || Key == TEXT("scenecolorafterdof"))
		{
			OutLocation = BL_SceneColorAfterDOF;
			return true;
		}
		if (Key == TEXT("translucencyafterdof") || Key == TEXT("translucent"))
		{
			OutLocation = BL_TranslucencyAfterDOF;
			return true;
		}
		if (Key == TEXT("ssr") || Key == TEXT("ssrinput"))
		{
			OutLocation = BL_SSRInput;
			return true;
		}
		if (Key == TEXT("beforebloom") || Key == TEXT("scenecolorbeforebloom"))
		{
			OutLocation = BL_SceneColorBeforeBloom;
			return true;
		}
		if (Key == TEXT("replacingtonemapper") || Key == TEXT("replacetonemap") || Key == TEXT("tonemapper"))
		{
			OutLocation = BL_ReplacingTonemapper;
			return true;
		}
		if (Key == TEXT("aftertonemapping") || Key == TEXT("aftertonemap") || Key == TEXT("scenecoloraftertonemapping"))
		{
			OutLocation = BL_SceneColorAfterTonemapping;
			return true;
		}

		int64 EnumValue = INDEX_NONE;
		if (TryParseEnumValue(StaticEnum<EBlendableLocation>(), Value, EnumValue))
		{
			OutLocation = static_cast<EBlendableLocation>(EnumValue);
			return OutLocation != BL_MAX;
		}
		return false;
	}

	static TSharedPtr<FJsonObject> MaterialSettingsToJson(const UMaterial* Material)
	{
		TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
		if (!Material)
		{
			return Settings;
		}

		const EMaterialDomain Domain = static_cast<EMaterialDomain>(Material->MaterialDomain.GetValue());
		const EBlendMode BlendMode = static_cast<EBlendMode>(Material->BlendMode.GetValue());
		const FMaterialShadingModelField ShadingModels = Material->GetShadingModels();
		const EMaterialShadingModel ShadingModel = ShadingModels.IsValid() ? ShadingModels.GetFirstShadingModel() : MSM_DefaultLit;
		const ETranslucencyLightingMode TranslucencyLightingMode = static_cast<ETranslucencyLightingMode>(Material->TranslucencyLightingMode.GetValue());
		const EBlendableLocation BlendableLocation = static_cast<EBlendableLocation>(Material->BlendableLocation.GetValue());

		Settings->SetStringField(TEXT("domain"), EnumValueToString(Domain));
		Settings->SetStringField(TEXT("blendMode"), EnumValueToString(BlendMode));
		Settings->SetStringField(TEXT("shadingModel"), EnumValueToString(ShadingModel));
		Settings->SetNumberField(TEXT("shadingModelField"), ShadingModels.GetShadingModelField());
		Settings->SetNumberField(TEXT("opacityMaskClipValue"), Material->OpacityMaskClipValue);
		Settings->SetBoolField(TEXT("twoSided"), Material->TwoSided != 0);
		Settings->SetBoolField(TEXT("ditheredLODTransition"), Material->DitheredLODTransition != 0);
		Settings->SetBoolField(TEXT("useMaterialAttributes"), Material->bUseMaterialAttributes != 0);
		Settings->SetBoolField(TEXT("disableDepthTest"), Material->bDisableDepthTest != 0);
		Settings->SetBoolField(TEXT("outputTranslucentVelocity"), Material->bOutputTranslucentVelocity != 0);
		Settings->SetStringField(TEXT("translucencyLightingMode"), EnumValueToString(TranslucencyLightingMode));
		Settings->SetStringField(TEXT("postProcessLocation"), EnumValueToString(BlendableLocation));
		Settings->SetNumberField(TEXT("blendablePriority"), Material->BlendablePriority);
		Settings->SetBoolField(TEXT("isBlendable"), Material->bIsBlendable != 0);
		Settings->SetBoolField(TEXT("blendableOutputAlpha"), Material->BlendableOutputAlpha != 0);
		Settings->SetBoolField(TEXT("disablePreExposureScale"), Material->bDisablePreExposureScale != 0);
		Settings->SetBoolField(TEXT("enableStencilTest"), Material->bEnableStencilTest != 0);
		Settings->SetStringField(TEXT("stencilCompare"), EnumValueToString(static_cast<EMaterialStencilCompare>(Material->StencilCompare.GetValue())));
		Settings->SetNumberField(TEXT("stencilRefValue"), Material->StencilRefValue);
		return Settings;
	}

	static TSharedPtr<FJsonObject> MaterialPropertyConnectionToJson(UMaterial* Material, const FString& PropertyName, EMaterialProperty Property)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("property"), PropertyName);
		Result->SetBoolField(TEXT("connected"), false);
		if (!Material)
		{
			return Result;
		}

		const FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
		if (!Input || !Input->Expression)
		{
			return Result;
		}

		Result->SetBoolField(TEXT("connected"), true);
		Result->SetStringField(TEXT("expression"), Input->Expression->GetName());
		Result->SetStringField(TEXT("expressionClass"), Input->Expression->GetClass()->GetPathName());
		Result->SetNumberField(TEXT("outputIndex"), Input->OutputIndex);
		return Result;
	}

	static TArray<TSharedPtr<FJsonValue>> MaterialOutputConnectionsToJson(UMaterial* Material)
	{
		TArray<TSharedPtr<FJsonValue>> Outputs;
		Outputs.Add(MakeShared<FJsonValueObject>(MaterialPropertyConnectionToJson(Material, TEXT("baseColor"), MP_BaseColor)));
		Outputs.Add(MakeShared<FJsonValueObject>(MaterialPropertyConnectionToJson(Material, TEXT("emissiveColor"), MP_EmissiveColor)));
		Outputs.Add(MakeShared<FJsonValueObject>(MaterialPropertyConnectionToJson(Material, TEXT("normal"), MP_Normal)));
		Outputs.Add(MakeShared<FJsonValueObject>(MaterialPropertyConnectionToJson(Material, TEXT("worldPositionOffset"), MP_WorldPositionOffset)));
		Outputs.Add(MakeShared<FJsonValueObject>(MaterialPropertyConnectionToJson(Material, TEXT("opacity"), MP_Opacity)));
		Outputs.Add(MakeShared<FJsonValueObject>(MaterialPropertyConnectionToJson(Material, TEXT("opacityMask"), MP_OpacityMask)));
		Outputs.Add(MakeShared<FJsonValueObject>(MaterialPropertyConnectionToJson(Material, TEXT("roughness"), MP_Roughness)));
		Outputs.Add(MakeShared<FJsonValueObject>(MaterialPropertyConnectionToJson(Material, TEXT("metallic"), MP_Metallic)));
		Outputs.Add(MakeShared<FJsonValueObject>(MaterialPropertyConnectionToJson(Material, TEXT("specular"), MP_Specular)));
		Outputs.Add(MakeShared<FJsonValueObject>(MaterialPropertyConnectionToJson(Material, TEXT("ambientOcclusion"), MP_AmbientOcclusion)));
		return Outputs;
	}

	static bool ParseLogDateTimeUtc(const FString& Line, FDateTime& OutTime)
	{
		if (Line.Len() < 25 || Line[0] != TEXT('['))
		{
			return false;
		}

		const int32 Year = FCString::Atoi(*Line.Mid(1, 4));
		const int32 Month = FCString::Atoi(*Line.Mid(6, 2));
		const int32 Day = FCString::Atoi(*Line.Mid(9, 2));
		const int32 Hour = FCString::Atoi(*Line.Mid(12, 2));
		const int32 Minute = FCString::Atoi(*Line.Mid(15, 2));
		const int32 Second = FCString::Atoi(*Line.Mid(18, 2));
		const int32 Millisecond = FCString::Atoi(*Line.Mid(21, 3));
		if (Year <= 0 || Month <= 0 || Day <= 0)
		{
			return false;
		}

		OutTime = FDateTime(Year, Month, Day, Hour, Minute, Second, Millisecond);
		return true;
	}

	static bool IsMaterialDiagnosticLine(const FString& Line)
	{
		const FString Lower = Line.ToLower();
		return Lower.Contains(TEXT("failed to compile material")) ||
			Lower.Contains(TEXT("materialeditorstats: error")) ||
			Lower.Contains(TEXT(" error:")) ||
			Lower.Contains(TEXT("\terror:"));
	}

	static FString ProjectLogPath()
	{
		const FString ExpectedLogPath = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectLogDir() / FString::Printf(TEXT("%s.log"), FApp::GetProjectName()));
		if (FPaths::FileExists(ExpectedLogPath))
		{
			return ExpectedLogPath;
		}

		TArray<FString> LogFiles;
		IFileManager::Get().FindFiles(LogFiles, *(FPaths::ProjectLogDir() / TEXT("*.log")), true, false);
		FString BestPath;
		FDateTime BestTime = FDateTime::MinValue();
		for (const FString& LogFile : LogFiles)
		{
			const FString CandidatePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectLogDir() / LogFile);
			const FDateTime Timestamp = IFileManager::Get().GetTimeStamp(*CandidatePath);
			if (BestPath.IsEmpty() || Timestamp > BestTime)
			{
				BestPath = CandidatePath;
				BestTime = Timestamp;
			}
		}
		return BestPath;
	}

	static TSharedPtr<FJsonObject> MaterialDiagnosticsFromLog(const FString& AssetPath, double LookbackSeconds, int32 MaxLines)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		const FString CurrentLogPath = ProjectLogPath();
		Result->SetStringField(TEXT("logPath"), CurrentLogPath);
		Result->SetNumberField(TEXT("lookbackSeconds"), LookbackSeconds);

		TArray<TSharedPtr<FJsonValue>> Diagnostics;
		if (CurrentLogPath.IsEmpty())
		{
			Result->SetBoolField(TEXT("hasErrors"), false);
			Result->SetStringField(TEXT("note"), TEXT("No project log file was found."));
			Result->SetArrayField(TEXT("lines"), Diagnostics);
			return Result;
		}

		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *CurrentLogPath))
		{
			Result->SetBoolField(TEXT("hasErrors"), false);
			Result->SetStringField(TEXT("note"), TEXT("Could not read project log file."));
			Result->SetArrayField(TEXT("lines"), Diagnostics);
			return Result;
		}

		FString PackagePath = AssetPath;
		FString ObjectName = FPaths::GetBaseFilename(AssetPath);
		FString ObjectPart;
		if (AssetPath.Split(TEXT("."), &PackagePath, &ObjectPart, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			ObjectName = ObjectPart;
		}
		if (ObjectName.IsEmpty())
		{
			ObjectName = FPaths::GetBaseFilename(PackagePath);
		}

		const FDateTime OldestAllowedUtc = FDateTime::UtcNow() - FTimespan::FromSeconds(FMath::Max(0.0, LookbackSeconds));
		int32 CaptureRemaining = 0;
		for (const FString& Line : Lines)
		{
			FDateTime LineTimeUtc;
			const bool bHasTimestamp = ParseLogDateTimeUtc(Line, LineTimeUtc);
			const bool bRecent = !bHasTimestamp || LookbackSeconds <= 0.0 || LineTimeUtc >= OldestAllowedUtc;
			const bool bMentionsAsset = AssetPath.IsEmpty() ||
				(!PackagePath.IsEmpty() && Line.Contains(PackagePath)) ||
				(!ObjectName.IsEmpty() && Line.Contains(ObjectName));
			const bool bDiagnostic = IsMaterialDiagnosticLine(Line);

			bool bInclude = false;
			if (bRecent && bDiagnostic && bMentionsAsset)
			{
				bInclude = true;
				CaptureRemaining = 12;
			}
			else if (bRecent && CaptureRemaining > 0 && (bDiagnostic || Line.StartsWith(TEXT("\t")) || Line.Contains(TEXT("/Engine/Generated/Material.ush"))))
			{
				bInclude = true;
				--CaptureRemaining;
			}
			else if (bHasTimestamp)
			{
				CaptureRemaining = 0;
			}

			if (bInclude)
			{
				Diagnostics.Add(MakeShared<FJsonValueString>(Line));
				while (Diagnostics.Num() > FMath::Max(1, MaxLines))
				{
					Diagnostics.RemoveAt(0);
				}
			}
		}

		Result->SetBoolField(TEXT("hasErrors"), Diagnostics.Num() > 0);
		Result->SetNumberField(TEXT("count"), Diagnostics.Num());
		Result->SetArrayField(TEXT("lines"), Diagnostics);
		return Result;
	}

	static TArray<TSharedPtr<FJsonValue>> LinearColorToJsonArray(const FLinearColor& Value)
	{
		return {
			MakeShared<FJsonValueNumber>(Value.R),
			MakeShared<FJsonValueNumber>(Value.G),
			MakeShared<FJsonValueNumber>(Value.B),
			MakeShared<FJsonValueNumber>(Value.A)
		};
	}

	static TSharedPtr<FJsonObject> MaterialParameterInfoToJson(const FMaterialParameterInfo& ParameterInfo)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), ParameterInfo.Name.ToString());
		Result->SetNumberField(TEXT("association"), static_cast<int32>(ParameterInfo.Association.GetValue()));
		Result->SetNumberField(TEXT("index"), ParameterInfo.Index);
		return Result;
	}

	static TSharedPtr<FJsonObject> MaterialInstanceToJson(UMaterialInstance* Instance)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!Instance)
		{
			return Result;
		}

		Result->SetStringField(TEXT("asset"), Instance->GetPathName());
		Result->SetStringField(TEXT("class"), Instance->GetClass()->GetPathName());
		Result->SetStringField(TEXT("type"), TEXT("MaterialInstance"));
		Result->SetStringField(TEXT("parent"), Instance->Parent ? Instance->Parent->GetPathName() : FString());

		TArray<TSharedPtr<FJsonValue>> ScalarOverrides;
		for (const FScalarParameterValue& Parameter : Instance->ScalarParameterValues)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetObjectField(TEXT("parameterInfo"), MaterialParameterInfoToJson(Parameter.ParameterInfo));
			Item->SetStringField(TEXT("name"), Parameter.ParameterInfo.Name.ToString());
			Item->SetNumberField(TEXT("value"), Parameter.ParameterValue);
			Item->SetStringField(TEXT("expressionGuid"), Parameter.ExpressionGUID.ToString(EGuidFormats::DigitsWithHyphens));
			ScalarOverrides.Add(MakeShared<FJsonValueObject>(Item));
		}

		TArray<TSharedPtr<FJsonValue>> VectorOverrides;
		for (const FVectorParameterValue& Parameter : Instance->VectorParameterValues)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetObjectField(TEXT("parameterInfo"), MaterialParameterInfoToJson(Parameter.ParameterInfo));
			Item->SetStringField(TEXT("name"), Parameter.ParameterInfo.Name.ToString());
			Item->SetArrayField(TEXT("value"), LinearColorToJsonArray(Parameter.ParameterValue));
			Item->SetStringField(TEXT("expressionGuid"), Parameter.ExpressionGUID.ToString(EGuidFormats::DigitsWithHyphens));
			VectorOverrides.Add(MakeShared<FJsonValueObject>(Item));
		}

		TArray<TSharedPtr<FJsonValue>> TextureOverrides;
		for (const FTextureParameterValue& Parameter : Instance->TextureParameterValues)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetObjectField(TEXT("parameterInfo"), MaterialParameterInfoToJson(Parameter.ParameterInfo));
			Item->SetStringField(TEXT("name"), Parameter.ParameterInfo.Name.ToString());
			Item->SetStringField(TEXT("value"), Parameter.ParameterValue ? Parameter.ParameterValue->GetPathName() : FString());
			Item->SetStringField(TEXT("expressionGuid"), Parameter.ExpressionGUID.ToString(EGuidFormats::DigitsWithHyphens));
			TextureOverrides.Add(MakeShared<FJsonValueObject>(Item));
		}

		TArray<FMaterialParameterInfo> ParameterInfos;
		TArray<FGuid> ParameterIds;

		TArray<TSharedPtr<FJsonValue>> ScalarParameters;
		Instance->GetAllScalarParameterInfo(ParameterInfos, ParameterIds);
		for (const FMaterialParameterInfo& ParameterInfo : ParameterInfos)
		{
			float Value = 0.f;
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetObjectField(TEXT("parameterInfo"), MaterialParameterInfoToJson(ParameterInfo));
			Item->SetStringField(TEXT("name"), ParameterInfo.Name.ToString());
			if (Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(ParameterInfo.Name, ParameterInfo.Association, ParameterInfo.Index), Value))
			{
				Item->SetNumberField(TEXT("value"), Value);
			}
			ScalarParameters.Add(MakeShared<FJsonValueObject>(Item));
		}

		ParameterInfos.Reset();
		ParameterIds.Reset();
		TArray<TSharedPtr<FJsonValue>> VectorParameters;
		Instance->GetAllVectorParameterInfo(ParameterInfos, ParameterIds);
		for (const FMaterialParameterInfo& ParameterInfo : ParameterInfos)
		{
			FLinearColor Value = FLinearColor::Black;
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetObjectField(TEXT("parameterInfo"), MaterialParameterInfoToJson(ParameterInfo));
			Item->SetStringField(TEXT("name"), ParameterInfo.Name.ToString());
			if (Instance->GetVectorParameterValue(FHashedMaterialParameterInfo(ParameterInfo.Name, ParameterInfo.Association, ParameterInfo.Index), Value))
			{
				Item->SetArrayField(TEXT("value"), LinearColorToJsonArray(Value));
			}
			VectorParameters.Add(MakeShared<FJsonValueObject>(Item));
		}

		ParameterInfos.Reset();
		ParameterIds.Reset();
		TArray<TSharedPtr<FJsonValue>> TextureParameters;
		Instance->GetAllTextureParameterInfo(ParameterInfos, ParameterIds);
		for (const FMaterialParameterInfo& ParameterInfo : ParameterInfos)
		{
			UTexture* Value = nullptr;
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetObjectField(TEXT("parameterInfo"), MaterialParameterInfoToJson(ParameterInfo));
			Item->SetStringField(TEXT("name"), ParameterInfo.Name.ToString());
			if (Instance->GetTextureParameterValue(FHashedMaterialParameterInfo(ParameterInfo.Name, ParameterInfo.Association, ParameterInfo.Index), Value))
			{
				Item->SetStringField(TEXT("value"), Value ? Value->GetPathName() : FString());
			}
			TextureParameters.Add(MakeShared<FJsonValueObject>(Item));
		}

		Result->SetArrayField(TEXT("scalarOverrides"), ScalarOverrides);
		Result->SetArrayField(TEXT("vectorOverrides"), VectorOverrides);
		Result->SetArrayField(TEXT("textureOverrides"), TextureOverrides);
		Result->SetArrayField(TEXT("scalarParameters"), ScalarParameters);
		Result->SetArrayField(TEXT("vectorParameters"), VectorParameters);
		Result->SetArrayField(TEXT("textureParameters"), TextureParameters);
		return Result;
	}

	static bool TryReadLinearColor(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FLinearColor& OutValue)
	{
		if (!Params.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* ArrayValue = nullptr;
		if (Params->TryGetArrayField(FieldName, ArrayValue) && ArrayValue && ArrayValue->Num() >= 3)
		{
			OutValue.R = static_cast<float>((*ArrayValue)[0]->AsNumber());
			OutValue.G = static_cast<float>((*ArrayValue)[1]->AsNumber());
			OutValue.B = static_cast<float>((*ArrayValue)[2]->AsNumber());
			OutValue.A = ArrayValue->Num() >= 4 ? static_cast<float>((*ArrayValue)[3]->AsNumber()) : 1.f;
			return true;
		}

		const TSharedPtr<FJsonObject>* ObjectValue = nullptr;
		if (Params->TryGetObjectField(FieldName, ObjectValue) && ObjectValue && ObjectValue->IsValid())
		{
			double R = 0.0;
			double G = 0.0;
			double B = 0.0;
			double A = 1.0;
			(*ObjectValue)->TryGetNumberField(TEXT("r"), R);
			(*ObjectValue)->TryGetNumberField(TEXT("g"), G);
			(*ObjectValue)->TryGetNumberField(TEXT("b"), B);
			(*ObjectValue)->TryGetNumberField(TEXT("a"), A);
			(*ObjectValue)->TryGetNumberField(TEXT("R"), R);
			(*ObjectValue)->TryGetNumberField(TEXT("G"), G);
			(*ObjectValue)->TryGetNumberField(TEXT("B"), B);
			(*ObjectValue)->TryGetNumberField(TEXT("A"), A);
			OutValue = FLinearColor(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
			return true;
		}

		return false;
	}

	static bool SetObjectPropertyFromString(UObject* Object, const FString& PropertyName, const FString& Value, FString& OutOldValue, FString& OutError)
	{
		if (!Object || PropertyName.IsEmpty())
		{
			OutError = TEXT("Missing object or property name.");
			return false;
		}

		FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), *PropertyName);
		if (!Property)
		{
			OutError = FString::Printf(TEXT("Property not found: %s.%s"), *Object->GetClass()->GetName(), *PropertyName);
			return false;
		}

		void* PropertyValue = Property->ContainerPtrToValuePtr<void>(Object);
		Property->ExportText_Direct(OutOldValue, PropertyValue, PropertyValue, Object, PPF_None);
		Object->Modify();

		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			UObject* NewObject = Value.IsEmpty() ? nullptr : LoadObject<UObject>(nullptr, *Value);
			if (!Value.IsEmpty() && !NewObject)
			{
				OutError = FString::Printf(TEXT("Object value not found for %s.%s: %s"), *Object->GetClass()->GetName(), *PropertyName, *Value);
				return false;
			}
			ObjectProperty->SetObjectPropertyValue(PropertyValue, NewObject);
			return true;
		}

		const TCHAR* ImportResult = Property->ImportText_Direct(*Value, PropertyValue, Object, PPF_None);
		if (ImportResult == nullptr)
		{
			OutError = FString::Printf(TEXT("Invalid value for %s.%s: %s"), *Object->GetClass()->GetName(), *PropertyName, *Value);
			return false;
		}
		return true;
	}

	static UEdGraphNode* FindNodeByGuid(UBlueprint* Blueprint, const FString& NodeId)
	{
		if (!Blueprint || NodeId.IsEmpty())
		{
			return nullptr;
		}

		FGuid NodeGuid;
		if (!FGuid::Parse(NodeId, NodeGuid))
		{
			return nullptr;
		}

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph)
			{
				continue;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->NodeGuid == NodeGuid)
				{
					return Node;
				}
			}
		}
		return nullptr;
	}

	static UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName, const FString& GraphPath)
	{
		if (!Blueprint)
		{
			return nullptr;
		}

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph)
			{
				continue;
			}

			if (!GraphPath.IsEmpty() && Graph->GetPathName() == GraphPath)
			{
				return Graph;
			}

			if (!GraphName.IsEmpty() && Graph->GetName() == GraphName)
			{
				return Graph;
			}
		}

		if (GraphName.IsEmpty() && GraphPath.IsEmpty())
		{
			for (UEdGraph* Graph : Blueprint->UbergraphPages)
			{
				if (Graph)
				{
					return Graph;
				}
			}
		}

		return nullptr;
	}

	static UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinId, const FString& PinName)
	{
		if (!Node)
		{
			return nullptr;
		}

		if (!PinId.IsEmpty())
		{
			FGuid PinGuid;
			if (FGuid::Parse(PinId, PinGuid))
			{
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && Pin->PinId == PinGuid)
					{
						return Pin;
					}
				}
			}
		}

		if (!PinName.IsEmpty())
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->PinName.ToString() == PinName)
				{
					return Pin;
				}
			}
		}
		return nullptr;
	}

	static FVector ReadVectorArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, const FVector& DefaultValue)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(FieldName, Values) || !Values || Values->Num() < 3)
		{
			return DefaultValue;
		}
		return FVector(
			(*Values)[0]->AsNumber(),
			(*Values)[1]->AsNumber(),
			(*Values)[2]->AsNumber());
	}

	static FRotator ReadRotatorArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, const FRotator& DefaultValue)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(FieldName, Values) || !Values || Values->Num() < 3)
		{
			return DefaultValue;
		}
		return FRotator(
			(*Values)[0]->AsNumber(),
			(*Values)[1]->AsNumber(),
			(*Values)[2]->AsNumber());
	}

	static TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& Value)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Add(MakeShared<FJsonValueNumber>(Value.X));
		Values.Add(MakeShared<FJsonValueNumber>(Value.Y));
		Values.Add(MakeShared<FJsonValueNumber>(Value.Z));
		return Values;
	}

	static TArray<TSharedPtr<FJsonValue>> RotatorToJsonArray(const FRotator& Value)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Add(MakeShared<FJsonValueNumber>(Value.Pitch));
		Values.Add(MakeShared<FJsonValueNumber>(Value.Yaw));
		Values.Add(MakeShared<FJsonValueNumber>(Value.Roll));
		return Values;
	}

	static TSharedPtr<FJsonObject> BoundsToJson(const FBoxSphereBounds& Bounds)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("origin"), VectorToJsonArray(Bounds.Origin));
		Result->SetArrayField(TEXT("boxExtent"), VectorToJsonArray(Bounds.BoxExtent));
		Result->SetNumberField(TEXT("sphereRadius"), Bounds.SphereRadius);
		return Result;
	}

	struct FGeometryRadialBounds
	{
		bool bValid = false;
		double MinRadius = TNumericLimits<double>::Max();
		double MaxRadius = 0.0;
		int32 MinVertexIndex = INDEX_NONE;
		int32 MaxVertexIndex = INDEX_NONE;
		FVector MinPosition = FVector::ZeroVector;
		FVector MaxPosition = FVector::ZeroVector;
		FString MinComponent;
		FString MaxComponent;

		void AddPosition(const FVector& Position, const FVector& Origin, int32 VertexIndex, const FString& ComponentName = FString())
		{
			const double Radius = FVector::Dist(Position, Origin);
			if (!bValid || Radius < MinRadius)
			{
				bValid = true;
				MinRadius = Radius;
				MinVertexIndex = VertexIndex;
				MinPosition = Position;
				MinComponent = ComponentName;
			}
			if (!bValid || Radius > MaxRadius)
			{
				bValid = true;
				MaxRadius = Radius;
				MaxVertexIndex = VertexIndex;
				MaxPosition = Position;
				MaxComponent = ComponentName;
			}
		}
	};

	static TSharedPtr<FJsonObject> RadialBoundsToJson(const FGeometryRadialBounds& Bounds, const FVector& Origin)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("valid"), Bounds.bValid);
		Result->SetArrayField(TEXT("origin"), VectorToJsonArray(Origin));
		if (Bounds.bValid)
		{
			Result->SetNumberField(TEXT("minRadius"), Bounds.MinRadius);
			Result->SetNumberField(TEXT("maxRadius"), Bounds.MaxRadius);
			Result->SetNumberField(TEXT("heightRange"), Bounds.MaxRadius - Bounds.MinRadius);
			Result->SetNumberField(TEXT("minVertexIndex"), Bounds.MinVertexIndex);
			Result->SetNumberField(TEXT("maxVertexIndex"), Bounds.MaxVertexIndex);
			Result->SetArrayField(TEXT("minPosition"), VectorToJsonArray(Bounds.MinPosition));
			Result->SetArrayField(TEXT("maxPosition"), VectorToJsonArray(Bounds.MaxPosition));
			if (!Bounds.MinComponent.IsEmpty())
			{
				Result->SetStringField(TEXT("minComponent"), Bounds.MinComponent);
			}
			if (!Bounds.MaxComponent.IsEmpty())
			{
				Result->SetStringField(TEXT("maxComponent"), Bounds.MaxComponent);
			}
		}
		return Result;
	}

	static FVector StaticMeshVertexPosition(const FStaticMeshLODResources& LodResource, int32 VertexIndex)
	{
		const FVector3f Position = LodResource.VertexBuffers.PositionVertexBuffer.VertexPosition(VertexIndex);
		return FVector(Position.X, Position.Y, Position.Z);
	}

	static bool ReadStaticMeshLod(UStaticMesh* StaticMesh, int32 LodIndex, const FStaticMeshLODResources*& OutLodResource, FString& OutError)
	{
		OutLodResource = nullptr;
		if (!StaticMesh)
		{
			OutError = TEXT("Missing StaticMesh.");
			return false;
		}
		if (!StaticMesh->GetRenderData())
		{
			OutError = FString::Printf(TEXT("StaticMesh has no render data: %s"), *StaticMesh->GetPathName());
			return false;
		}
		if (!StaticMesh->GetRenderData()->LODResources.IsValidIndex(LodIndex))
		{
			OutError = FString::Printf(TEXT("StaticMesh LOD index out of range: %s lod=%d"), *StaticMesh->GetPathName(), LodIndex);
			return false;
		}
		OutLodResource = &StaticMesh->GetRenderData()->LODResources[LodIndex];
		return true;
	}

	static void AddGeometrySamples(
		const FStaticMeshLODResources& LodResource,
		const FTransform& Transform,
		const FVector& RadialOrigin,
		bool bIncludeVertices,
		int32 VertexLimit,
		bool bIncludeTriangles,
		int32 TriangleLimit,
		const FString& ComponentName,
		FGeometryRadialBounds& InOutRadialBounds,
		TArray<TSharedPtr<FJsonValue>>* OutVertices,
		TArray<TSharedPtr<FJsonValue>>* OutTriangles)
	{
		const int32 VertexCount = LodResource.GetNumVertices();
		const int32 VertexSampleLimit = bIncludeVertices ? FMath::Clamp(VertexLimit, 0, 100000) : 0;
		for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
		{
			const FVector Position = Transform.TransformPosition(StaticMeshVertexPosition(LodResource, VertexIndex));
			InOutRadialBounds.AddPosition(Position, RadialOrigin, VertexIndex, ComponentName);
			if (OutVertices && OutVertices->Num() < VertexSampleLimit)
			{
				TSharedPtr<FJsonObject> VertexJson = MakeShared<FJsonObject>();
				VertexJson->SetNumberField(TEXT("index"), VertexIndex);
				VertexJson->SetArrayField(TEXT("position"), VectorToJsonArray(Position));
				VertexJson->SetNumberField(TEXT("radius"), FVector::Dist(Position, RadialOrigin));
				if (!ComponentName.IsEmpty())
				{
					VertexJson->SetStringField(TEXT("component"), ComponentName);
				}
				OutVertices->Add(MakeShared<FJsonValueObject>(VertexJson));
			}
		}

		if (!bIncludeTriangles || !OutTriangles)
		{
			return;
		}

		const FIndexArrayView Indices = LodResource.IndexBuffer.GetArrayView();
		const int32 TriangleCount = Indices.Num() / 3;
		const int32 TriangleSampleLimit = FMath::Clamp(TriangleLimit, 0, 100000);
		for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount && OutTriangles->Num() < TriangleSampleLimit; ++TriangleIndex)
		{
			const int32 I0 = static_cast<int32>(Indices[TriangleIndex * 3 + 0]);
			const int32 I1 = static_cast<int32>(Indices[TriangleIndex * 3 + 1]);
			const int32 I2 = static_cast<int32>(Indices[TriangleIndex * 3 + 2]);
			const FVector P0 = Transform.TransformPosition(StaticMeshVertexPosition(LodResource, I0));
			const FVector P1 = Transform.TransformPosition(StaticMeshVertexPosition(LodResource, I1));
			const FVector P2 = Transform.TransformPosition(StaticMeshVertexPosition(LodResource, I2));
			const FVector Center = (P0 + P1 + P2) / 3.0;

			TArray<TSharedPtr<FJsonValue>> VertexIndices;
			VertexIndices.Add(MakeShared<FJsonValueNumber>(I0));
			VertexIndices.Add(MakeShared<FJsonValueNumber>(I1));
			VertexIndices.Add(MakeShared<FJsonValueNumber>(I2));

			TArray<TSharedPtr<FJsonValue>> Positions;
			Positions.Add(MakeShared<FJsonValueArray>(VectorToJsonArray(P0)));
			Positions.Add(MakeShared<FJsonValueArray>(VectorToJsonArray(P1)));
			Positions.Add(MakeShared<FJsonValueArray>(VectorToJsonArray(P2)));

			TSharedPtr<FJsonObject> TriangleJson = MakeShared<FJsonObject>();
			TriangleJson->SetNumberField(TEXT("index"), TriangleIndex);
			TriangleJson->SetArrayField(TEXT("vertices"), VertexIndices);
			TriangleJson->SetArrayField(TEXT("positions"), Positions);
			TriangleJson->SetArrayField(TEXT("center"), VectorToJsonArray(Center));
			TriangleJson->SetNumberField(TEXT("centerRadius"), FVector::Dist(Center, RadialOrigin));
			if (!ComponentName.IsEmpty())
			{
				TriangleJson->SetStringField(TEXT("component"), ComponentName);
			}
			OutTriangles->Add(MakeShared<FJsonValueObject>(TriangleJson));
		}
	}

	static TSharedPtr<FJsonObject> StaticMeshSocketToJson(const UStaticMeshSocket* Socket)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		if (!Socket)
		{
			return Item;
		}
		Item->SetStringField(TEXT("name"), Socket->SocketName.ToString());
		Item->SetArrayField(TEXT("relativeLocation"), VectorToJsonArray(Socket->RelativeLocation));
		Item->SetArrayField(TEXT("relativeRotation"), RotatorToJsonArray(Socket->RelativeRotation));
		Item->SetArrayField(TEXT("relativeScale"), VectorToJsonArray(Socket->RelativeScale));
		Item->SetStringField(TEXT("tag"), Socket->Tag);
		return Item;
	}

	static TSharedPtr<FJsonObject> SkeletalMeshSocketToJson(const USkeletalMeshSocket* Socket, const FString& Source)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		if (!Socket)
		{
			return Item;
		}
		Item->SetStringField(TEXT("name"), Socket->SocketName.ToString());
		Item->SetStringField(TEXT("boneName"), Socket->BoneName.ToString());
		Item->SetArrayField(TEXT("relativeLocation"), VectorToJsonArray(Socket->RelativeLocation));
		Item->SetArrayField(TEXT("relativeRotation"), RotatorToJsonArray(Socket->RelativeRotation));
		Item->SetArrayField(TEXT("relativeScale"), VectorToJsonArray(Socket->RelativeScale));
		Item->SetBoolField(TEXT("forceAlwaysAnimated"), Socket->bForceAlwaysAnimated);
		Item->SetStringField(TEXT("source"), Source);
		return Item;
	}

	static bool SaveAssetObject(UObject* Asset, bool bSave)
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
		return UEditorLoadingAndSavingUtils::SavePackages({ Asset->GetOutermost() }, false);
	}

	static bool TryReadVectorArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FVector& OutValue)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(FieldName, Values) || !Values || Values->Num() < 3)
		{
			return false;
		}
		OutValue = FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
		return true;
	}

	static bool TryReadRotatorArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FRotator& OutValue)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(FieldName, Values) || !Values || Values->Num() < 3)
		{
			return false;
		}
		OutValue = FRotator((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
		return true;
	}

	static TSharedPtr<FJsonObject> BoxToJson(const FBox& Box)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		const bool bValid = Box.IsValid != 0;
		Result->SetBoolField(TEXT("valid"), bValid);
		if (bValid)
		{
			Result->SetArrayField(TEXT("min"), VectorToJsonArray(Box.Min));
			Result->SetArrayField(TEXT("max"), VectorToJsonArray(Box.Max));
			Result->SetArrayField(TEXT("origin"), VectorToJsonArray(Box.GetCenter()));
			Result->SetArrayField(TEXT("boxExtent"), VectorToJsonArray(Box.GetExtent()));
			Result->SetArrayField(TEXT("size"), VectorToJsonArray(Box.GetSize()));
		}
		return Result;
	}

	static TSharedPtr<FJsonObject> TransformToJson(const FTransform& Transform)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("location"), VectorToJsonArray(Transform.GetLocation()));
		Result->SetArrayField(TEXT("rotation"), RotatorToJsonArray(Transform.Rotator()));
		Result->SetArrayField(TEXT("scale"), VectorToJsonArray(Transform.GetScale3D()));
		return Result;
	}

	static UWorld* GetEditorWorld(FString& OutError)
	{
		if (!GEditor)
		{
			OutError = TEXT("GEditor is not available.");
			return nullptr;
		}

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			OutError = TEXT("Editor world is not available.");
			return nullptr;
		}
		return World;
	}

	static TSharedPtr<FJsonObject> ActorBoundsToJson(const AActor* Actor)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!Actor)
		{
			return Result;
		}

		const FBox WorldBox = Actor->GetComponentsBoundingBox(true, true);
		const FBox LocalBox = Actor->CalculateComponentsBoundingBoxInLocalSpace(true, true);
		Result->SetObjectField(TEXT("world"), BoxToJson(WorldBox));
		Result->SetObjectField(TEXT("local"), BoxToJson(LocalBox));
		return Result;
	}

	static TSharedPtr<FJsonObject> ComponentToJson(const UActorComponent* Component, bool bIncludeBounds)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!Component)
		{
			return Result;
		}

		Result->SetStringField(TEXT("name"), Component->GetName());
		Result->SetStringField(TEXT("pathName"), Component->GetPathName());
		Result->SetStringField(TEXT("class"), Component->GetClass()->GetPathName());
		Result->SetBoolField(TEXT("registered"), Component->IsRegistered());
		Result->SetBoolField(TEXT("createdByConstructionScript"), Component->IsCreatedByConstructionScript());

		if (const USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
		{
			Result->SetStringField(TEXT("attachParent"), SceneComponent->GetAttachParent() ? SceneComponent->GetAttachParent()->GetName() : FString());
			Result->SetObjectField(TEXT("worldTransform"), TransformToJson(SceneComponent->GetComponentTransform()));
			Result->SetObjectField(TEXT("relativeTransform"), TransformToJson(SceneComponent->GetRelativeTransform()));
		}
		if (bIncludeBounds)
		{
			if (const UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(Component))
			{
				Result->SetObjectField(TEXT("bounds"), BoundsToJson(PrimitiveComponent->Bounds));
			}
		}
		return Result;
	}

	static TSharedPtr<FJsonObject> ActorToJson(AActor* Actor, bool bIncludeComponents, bool bIncludeBounds)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!Actor)
		{
			return Result;
		}

		Result->SetStringField(TEXT("name"), Actor->GetName());
		Result->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Result->SetStringField(TEXT("pathName"), Actor->GetPathName());
		Result->SetStringField(TEXT("actorGuid"), Actor->GetActorGuid().ToString(EGuidFormats::DigitsWithHyphens));
		Result->SetStringField(TEXT("actorInstanceGuid"), Actor->GetActorInstanceGuid().ToString(EGuidFormats::DigitsWithHyphens));
		Result->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
		Result->SetStringField(TEXT("blueprint"), Actor->GetClass()->ClassGeneratedBy ? Actor->GetClass()->ClassGeneratedBy->GetPathName() : FString());
		Result->SetStringField(TEXT("level"), Actor->GetLevel() ? Actor->GetLevel()->GetPathName() : FString());
		Result->SetStringField(TEXT("folderPath"), Actor->GetFolderPath().ToString());
		Result->SetStringField(TEXT("attachParent"), Actor->GetAttachParentActor() ? Actor->GetAttachParentActor()->GetPathName() : FString());
		Result->SetObjectField(TEXT("transform"), TransformToJson(Actor->GetActorTransform()));
		Result->SetBoolField(TEXT("hidden"), Actor->IsHidden());
		Result->SetBoolField(TEXT("hiddenEd"), Actor->IsHiddenEd());
		Result->SetBoolField(TEXT("selected"), GEditor && GEditor->GetSelectedActors() ? GEditor->GetSelectedActors()->IsSelected(Actor) : false);

		if (bIncludeBounds)
		{
			Result->SetObjectField(TEXT("bounds"), ActorBoundsToJson(Actor));
		}

		if (bIncludeComponents)
		{
			TArray<TSharedPtr<FJsonValue>> Components;
			TArray<UActorComponent*> ActorComponents;
			Actor->GetComponents(ActorComponents);
			for (UActorComponent* Component : ActorComponents)
			{
				Components.Add(MakeShared<FJsonValueObject>(ComponentToJson(Component, bIncludeBounds)));
			}
			Result->SetArrayField(TEXT("components"), Components);
			Result->SetNumberField(TEXT("componentCount"), Components.Num());
		}
		return Result;
	}

	static AActor* FindEditorActor(const TSharedPtr<FJsonObject>& Params, FString& OutError)
	{
		UWorld* World = GetEditorWorld(OutError);
		if (!World)
		{
			return nullptr;
		}

		FString ActorRef;
		FString ActorGuidText;
		FString PathName;
		FString Label;
		FString Name;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("actor"), ActorRef);
			Params->TryGetStringField(TEXT("actorGuid"), ActorGuidText);
			Params->TryGetStringField(TEXT("pathName"), PathName);
			Params->TryGetStringField(TEXT("actorPath"), PathName);
			Params->TryGetStringField(TEXT("label"), Label);
			Params->TryGetStringField(TEXT("name"), Name);
		}

		if (ActorRef.IsEmpty() && ActorGuidText.IsEmpty() && PathName.IsEmpty() && Label.IsEmpty() && Name.IsEmpty())
		{
			OutError = TEXT("Missing actor reference. Use params.actorGuid, params.pathName, params.label, params.name, or params.actor.");
			return nullptr;
		}

		FGuid ExplicitGuid;
		const bool bHasExplicitGuid = FGuid::Parse(ActorGuidText, ExplicitGuid);
		FGuid GenericGuid;
		const bool bActorRefIsGuid = FGuid::Parse(ActorRef, GenericGuid);

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}

			if (bHasExplicitGuid && (Actor->GetActorGuid() == ExplicitGuid || Actor->GetActorInstanceGuid() == ExplicitGuid))
			{
				return Actor;
			}
			if (!PathName.IsEmpty() && Actor->GetPathName() == PathName)
			{
				return Actor;
			}
			if (!Label.IsEmpty() && Actor->GetActorLabel() == Label)
			{
				return Actor;
			}
			if (!Name.IsEmpty() && Actor->GetName() == Name)
			{
				return Actor;
			}
			if (!ActorRef.IsEmpty())
			{
				if ((bActorRefIsGuid && (Actor->GetActorGuid() == GenericGuid || Actor->GetActorInstanceGuid() == GenericGuid)) ||
					Actor->GetPathName() == ActorRef ||
					Actor->GetActorLabel() == ActorRef ||
					Actor->GetName() == ActorRef)
				{
					return Actor;
				}
			}
		}

		OutError = FString::Printf(TEXT("Editor actor not found: %s%s%s%s%s"),
			*ActorRef,
			*ActorGuidText,
			*PathName,
			*Label,
			*Name);
		return nullptr;
	}

	static UActorComponent* FindActorComponent(AActor* Actor, const FString& ComponentRef)
	{
		if (!Actor || ComponentRef.IsEmpty())
		{
			return nullptr;
		}

		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (!Component)
			{
				continue;
			}
			if (Component->GetName() == ComponentRef ||
				Component->GetPathName() == ComponentRef ||
				Component->GetClass()->GetName() == ComponentRef ||
				Component->GetClass()->GetPathName() == ComponentRef)
			{
				return Component;
			}
		}
		return nullptr;
	}
}

	struct FWebFrameMcpModuleFiles
	{
		FName OwnerName;
		FString PluginName;
		FString DisplayName;
		FString BaseDir;
		FString DocumentationPath;
		FString DocumentationText;
		FString ConfigPath;
		FString ConfigText;
	};

	struct FWebFrameMcpConfigSettingRow
	{
		FString Key;
		FString TypeLabel;
		TSharedPtr<FJsonValue> OriginalValue;
		TSharedPtr<SEditableTextBox> ValueTextBox;
		TSharedPtr<SCheckBox> BoolCheckBox;
	};

	static FString PluginNameFromCommandOwner(FName OwnerName)
	{
		const FString OwnerString = OwnerName.ToString();
		return OwnerString == TEXT("WebFrameMcpCore") ? TEXT("WebFrameMcp") : OwnerString;
	}

	static FString DefaultModuleDisplayNameFromCommandOwner(FName OwnerName)
	{
		const FString OwnerString = OwnerName.ToString();
		if (OwnerString == TEXT("WebFrameMcpCore"))
		{
			return TEXT("核心");
		}
		if (OwnerString == TEXT("WebFrameMcpBlueprint"))
		{
			return TEXT("普通蓝图");
		}
		if (OwnerString == TEXT("WebFrameMcpMaterial"))
		{
			return TEXT("材质");
		}
		if (OwnerString == TEXT("WebFrameMcpWorld"))
		{
			return TEXT("编辑器世界");
		}
		if (OwnerString == TEXT("WebFrameMcpMesh"))
		{
			return TEXT("模型");
		}
		if (OwnerString == TEXT("WebFrameMcpNiagara"))
		{
			return TEXT("Niagara");
		}

		FString DisplayName = OwnerString;
		DisplayName.ReplaceInline(TEXT("WebFrame"), TEXT(""));
		DisplayName.ReplaceInline(TEXT("Mcp"), TEXT(""));
		DisplayName.ReplaceInline(TEXT("MCP"), TEXT(""));
		return DisplayName.IsEmpty() ? OwnerString : DisplayName;
	}

	static FString ModuleConfigIdFromCommandOwner(FName OwnerName)
	{
		const FString OwnerString = OwnerName.ToString();
		if (OwnerString == TEXT("WebFrameMcpCore"))
		{
			return TEXT("core");
		}
		if (OwnerString == TEXT("WebFrameMcpBlueprint"))
		{
			return TEXT("blueprint");
		}
		if (OwnerString == TEXT("WebFrameMcpMaterial"))
		{
			return TEXT("material");
		}
		if (OwnerString == TEXT("WebFrameMcpWorld"))
		{
			return TEXT("world");
		}
		if (OwnerString == TEXT("WebFrameMcpMesh"))
		{
			return TEXT("mesh");
		}
		if (OwnerString == TEXT("WebFrameMcpNiagara"))
		{
			return TEXT("niagara");
		}

		FString ConfigId = OwnerString;
		ConfigId.ReplaceInline(TEXT("WebFrameMcp"), TEXT(""));
		ConfigId.ToLowerInline();
		return ConfigId.IsEmpty() ? OwnerString.ToLower() : ConfigId;
	}

	static FString SanitizeModuleDisplayName(const FString& InDisplayName, FName OwnerName)
	{
		FString DisplayName = InDisplayName;
		DisplayName.ReplaceInline(TEXT("WebFrame"), TEXT(""));
		DisplayName.ReplaceInline(TEXT("MCP Core"), TEXT("核心"));
		DisplayName.ReplaceInline(TEXT("Core"), TEXT("核心"));
		DisplayName.TrimStartAndEndInline();
		return DisplayName.IsEmpty() || DisplayName == TEXT("MCP")
			? DefaultModuleDisplayNameFromCommandOwner(OwnerName)
			: DisplayName;
	}

	static bool LoadJsonObjectFromString(const FString& JsonText, TSharedPtr<FJsonObject>& OutObject)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	static FString JsonValueToEditableText(const TSharedPtr<FJsonValue>& Value)
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
			return FString::SanitizeFloat(Value->AsNumber());
		}
		if (Value->Type == EJson::Boolean)
		{
			return Value->AsBool() ? TEXT("true") : TEXT("false");
		}

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer);
		return Out;
	}

	static FString JsonValueTypeLabel(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("空");
		}
		switch (Value->Type)
		{
		case EJson::String:
			return TEXT("文本");
		case EJson::Number:
			return TEXT("数字");
		case EJson::Boolean:
			return TEXT("开关");
		case EJson::Array:
			return TEXT("列表");
		case EJson::Object:
			return TEXT("对象");
		default:
			return TEXT("空");
		}
	}

	static FWebFrameMcpModuleFiles LoadModuleFiles(FName OwnerName)
	{
		FWebFrameMcpModuleFiles Files;
		Files.OwnerName = OwnerName;
		Files.PluginName = PluginNameFromCommandOwner(OwnerName);
		Files.DisplayName = DefaultModuleDisplayNameFromCommandOwner(OwnerName);

		if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Files.PluginName))
		{
			Files.BaseDir = Plugin->GetBaseDir();
			if (!Plugin->GetDescriptor().FriendlyName.IsEmpty())
			{
				Files.DisplayName = SanitizeModuleDisplayName(Plugin->GetDescriptor().FriendlyName, OwnerName);
			}
			Files.DocumentationPath = FPaths::Combine(Files.BaseDir, TEXT("Docs"), TEXT("McpInterface.md"));
			Files.ConfigPath = FPaths::Combine(Files.BaseDir, TEXT("Config"), TEXT("McpModuleConfig.json"));
		}
		else
		{
			Files.DocumentationText = FString::Printf(TEXT("Plugin not found for MCP owner: %s"), *OwnerName.ToString());
			return Files;
		}

		if (!FFileHelper::LoadFileToString(Files.DocumentationText, *Files.DocumentationPath))
		{
			Files.DocumentationText = FString::Printf(TEXT("Documentation file not found: %s"), *Files.DocumentationPath);
		}
		if (!FFileHelper::LoadFileToString(Files.ConfigText, *Files.ConfigPath))
		{
			Files.ConfigText = FString::Printf(TEXT("{\n  \"module\": \"%s\",\n  \"displayName\": \"%s\",\n  \"version\": 1,\n  \"settings\": {}\n}\n"),
				*ModuleConfigIdFromCommandOwner(OwnerName),
				*Files.DisplayName);
		}
		TSharedPtr<FJsonObject> ConfigObject;
		if (LoadJsonObjectFromString(Files.ConfigText, ConfigObject))
		{
			FString ConfigDisplayName;
			if (ConfigObject->TryGetStringField(TEXT("displayName"), ConfigDisplayName))
			{
				Files.DisplayName = SanitizeModuleDisplayName(ConfigDisplayName, OwnerName);
			}
		}
		return Files;
	}

class SWebFrameMcpPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWebFrameMcpPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, FWebFrameMcpModule* InOwner)
	{
		Owner = InOwner;

		ChildSlot
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8.0f)
			[
				SNew(SBorder)
				.Padding(8.0f)
				.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("ServerLabel", "WebSocket 服务"))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(SButton)
						.Text_Lambda([this]()
						{
							return Owner && Owner->IsServerRunning()
								? LOCTEXT("StopServer", "关闭")
								: LOCTEXT("StartServer", "开启");
						})
						.OnClicked(this, &SWebFrameMcpPanel::OnToggleServerClicked)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("PortLabel", "端口"))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 12.0f, 0.0f)
					[
						SAssignNew(PortTextBox, SEditableTextBox)
						.MinDesiredWidth(90.0f)
						.Text(FText::FromString(FString::FromInt(Owner ? Owner->GetPort() : 30303)))
						.IsEnabled_Lambda([this]()
						{
							return Owner && !Owner->IsServerRunning();
						})
						.OnTextCommitted(this, &SWebFrameMcpPanel::OnPortCommitted)
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text_Lambda([this]()
						{
							return Owner && Owner->IsServerRunning()
								? FText::Format(LOCTEXT("ServerRunning", "运行中 ws://127.0.0.1:{0}"), FText::FromString(FString::FromInt(Owner->GetPort())))
								: LOCTEXT("ServerStopped", "Stopped");
						})
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(8.0f, 0.0f, 4.0f, 0.0f)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "SimpleButton")
						.ContentPadding(FMargin(6.0f))
						.ToolTipText(LOCTEXT("SettingsTooltip", "设置"))
						.OnClicked(this, &SWebFrameMcpPanel::OnSettingsClicked)
						[
							SNew(SImage)
							.Image(FAppStyle::GetBrush("Icons.Settings"))
						]
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "SimpleButton")
						.ContentPadding(FMargin(6.0f))
						.ToolTipText(LOCTEXT("HelpTooltip", "说明"))
						.OnClicked(this, &SWebFrameMcpPanel::OnHelpClicked)
						[
							SNew(SImage)
							.Image(FAppStyle::GetBrush("Icons.Help"))
						]
					]
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(8.0f, 0.0f, 8.0f, 8.0f)
			[
				SNew(SBorder)
				.Padding(10.0f)
				.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("ConsoleTitle", "MCP Call Console"))
						.Font(FAppStyle::GetFontStyle("HeadingSmall"))
					]
					+ SVerticalBox::Slot()
					.FillHeight(0.48f)
					.Padding(0.0f, 0.0f, 0.0f, 12.0f)
					[
						SNew(SBorder)
						.Padding(8.0f)
						.BorderImage(&ConsoleBorderBrush)
						[
							SAssignNew(ConsoleBox, SScrollBox)
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("BlueprintChangeTitle", "操作记录"))
							.Font(FAppStyle::GetFontStyle("HeadingSmall"))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SButton)
							.Text(LOCTEXT("ClearChangeRecords", "清理保存记录"))
							.OnClicked(this, &SWebFrameMcpPanel::OnClearChangesClicked)
						]
					]
					+ SVerticalBox::Slot()
					.FillHeight(0.52f)
					[
						SNew(SBorder)
						.Padding(8.0f)
						.BorderImage(&ListBorderBrush)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(0.0f, 0.0f, 0.0f, 6.0f)
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot()
								.FillWidth(0.35f)
								[
									SNew(STextBlock)
									.Text(LOCTEXT("ChangeObjectHeader", "操作对象"))
									.Font(FAppStyle::GetFontStyle("NormalFontBold"))
								]
								+ SHorizontalBox::Slot()
								.FillWidth(0.65f)
								[
									SNew(STextBlock)
									.Text(LOCTEXT("ChangeSummaryHeader", "修改内容"))
									.Font(FAppStyle::GetFontStyle("NormalFontBold"))
								]
							]
							+ SVerticalBox::Slot()
							.FillHeight(1.0f)
							[
								SAssignNew(BlueprintChangeBox, SScrollBox)
							]
						]
					]
				]
			]
		];

		if (Owner)
		{
			for (const FString& Line : Owner->GetConsoleLines())
			{
				AppendLine(Line);
			}
			RefreshBlueprintChanges();
		}
	}

	void AppendLine(const FString& Line)
	{
		if (!ConsoleBox.IsValid())
		{
			return;
		}

		ConsoleBox->AddSlot()
		[
			SNew(STextBlock)
			.Text(FText::FromString(Line))
			.Font(FAppStyle::GetFontStyle("Log.Normal"))
			.ColorAndOpacity(FSlateColor(FLinearColor(0.78f, 0.95f, 0.82f)))
			.AutoWrapText(true)
		];
		ConsoleBox->ScrollToEnd();
	}

	void RefreshBlueprintChanges()
	{
		if (!BlueprintChangeBox.IsValid())
		{
			return;
		}

		BlueprintChangeBox->ClearChildren();
		if (!Owner)
		{
			return;
		}

		for (const TSharedPtr<FWebFrameMcpBlueprintChange>& Change : Owner->GetBlueprintChanges())
		{
			if (!Change.IsValid())
			{
				continue;
			}

			TSharedPtr<SVerticalBox> DetailBox;
			for (const TPair<FString, FString>& Detail : Change->Details)
			{
				// Filled after DetailBox exists.
			}

			TSharedRef<SExpandableArea> Area =
				SNew(SExpandableArea)
				.InitiallyCollapsed(true)
				.HeaderContent()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(0.35f)
					.VAlign(VAlign_Center)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "SimpleButton")
						.Text(FText::FromString(Change->ObjectName))
						.ToolTipText(FText::FromString(Change->AssetPath))
						.OnClicked(this, &SWebFrameMcpPanel::OnOpenRecordTargetClicked, Change->AssetPath)
					]
					+ SHorizontalBox::Slot()
					.FillWidth(0.65f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Change->Summary))
						.AutoWrapText(true)
					]
				]
				.BodyContent()
				[
					SAssignNew(DetailBox, SVerticalBox)
				];

			for (const TPair<FString, FString>& Detail : Change->Details)
			{
				DetailBox->AddSlot()
				.AutoHeight()
				.Padding(18.0f, 3.0f, 0.0f, 3.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(0.35f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Detail.Key))
					]
					+ SHorizontalBox::Slot()
					.FillWidth(0.65f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Detail.Value))
						.AutoWrapText(true)
					]
				];
			}

			BlueprintChangeBox->AddSlot()
			.Padding(0.0f, 2.0f, 0.0f, 4.0f)
			[
				Area
			];
		}
	}

private:
	void PopulateSettingsModuleList(const TSharedRef<SVerticalBox>& ModuleListBox) const
	{
		ModuleListBox->ClearChildren();
		if (!Owner)
		{
			ModuleListBox->AddSlot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SettingsModuleOwnerMissing", "Core module is not initialized; cannot read MCP module list."))
				.AutoWrapText(true)
			];
			return;
		}

		const TArray<FName> ModuleOwners = Owner->GetRegisteredCommandOwners();
		if (ModuleOwners.Num() == 0)
		{
			ModuleListBox->AddSlot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SettingsNoModules", "No registered MCP modules."))
				.AutoWrapText(true)
			];
			return;
		}

		ModuleListBox->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(SBorder)
			.Padding(FMargin(10.0f, 6.0f))
			.BorderImage(&TableHeaderBrush)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(0.58f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SettingsModuleTableNameHeader", "模块"))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(0.22f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SettingsModuleTableCommandHeader", "命令数"))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(0.20f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SettingsModuleTableEnabledHeader", "启用"))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
			]
		];

		for (FName ModuleOwner : ModuleOwners)
		{
			const FString ModuleName = LoadModuleFiles(ModuleOwner).DisplayName;
			const int32 CommandCount = Owner->GetRegisteredCommandCountByOwner(ModuleOwner);

			ModuleListBox->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 2.0f)
			[
				SNew(SBorder)
				.Padding(FMargin(10.0f, 6.0f))
				.BorderImage(&TableRowBrush)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(0.58f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(ModuleName))
						.AutoWrapText(true)
					]
					+ SHorizontalBox::Slot()
					.FillWidth(0.22f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::AsNumber(CommandCount))
					]
					+ SHorizontalBox::Slot()
					.FillWidth(0.20f)
					.VAlign(VAlign_Center)
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([this, ModuleOwner]()
						{
							return Owner && Owner->IsCommandOwnerEnabled(ModuleOwner)
								? ECheckBoxState::Checked
								: ECheckBoxState::Unchecked;
						})
						.OnCheckStateChanged_Lambda([this, ModuleOwner](ECheckBoxState NewState)
						{
							if (Owner)
							{
								Owner->SetCommandOwnerEnabled(ModuleOwner, NewState == ECheckBoxState::Checked);
							}
						})
					]
				]
			];
		}
	}

	void PopulatePathPermissionRows(FName ModuleOwner, const TSharedRef<SVerticalBox>& RowsBox) const
	{
		RowsBox->ClearChildren();
		if (!Owner)
		{
			return;
		}

		const TArray<FWebFrameMcpPathPermission>& Permissions = Owner->GetPathPermissions(ModuleOwner);
		if (Permissions.Num() == 0)
		{
			RowsBox->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SettingsPathPermissionEmpty", "未添加路径规则。当前模块不限制 UE 路径访问和修改。"))
				.AutoWrapText(true)
			];
			return;
		}

		RowsBox->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(SBorder)
			.Padding(FMargin(10.0f, 6.0f))
			.BorderImage(&TableHeaderBrush)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(0.66f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SettingsPathPermissionPathHeader", "路径"))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(0.14f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SettingsPathPermissionStateHeader", "当前权限"))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(0.20f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SettingsPathPermissionActionHeader", "操作"))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
			]
		];

		for (int32 Index = 0; Index < Permissions.Num(); ++Index)
		{
			const FWebFrameMcpPathPermission Permission = Permissions[Index];

			RowsBox->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 2.0f)
			[
				SNew(SBorder)
				.Padding(FMargin(10.0f, 6.0f))
				.BorderImage(&TableRowBrush)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(0.66f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Permission.Path))
						.AutoWrapText(true)
					]
					+ SHorizontalBox::Slot()
					.FillWidth(0.14f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(!Permission.bAllowAccess
							? LOCTEXT("SettingsPathPermissionForbiddenState", "禁止")
							: (Permission.bAllowModify
								? LOCTEXT("SettingsPathPermissionWritableState", "可修改")
								: LOCTEXT("SettingsPathPermissionReadonlyState", "只读")))
					]
					+ SHorizontalBox::Slot()
					.FillWidth(0.20f)
					.VAlign(VAlign_Center)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("SettingsPathPermissionCycle", "修改"))
							.OnClicked_Lambda([this, ModuleOwner, Index, RowsBox]()
							{
								if (Owner)
								{
									const TArray<FWebFrameMcpPathPermission>& CurrentPermissions = Owner->GetPathPermissions(ModuleOwner);
									if (CurrentPermissions.IsValidIndex(Index))
									{
										const FWebFrameMcpPathPermission& CurrentPermission = CurrentPermissions[Index];
										if (!CurrentPermission.bAllowAccess)
										{
											Owner->SetPathPermission(ModuleOwner, Index, true, true);
										}
										else if (CurrentPermission.bAllowModify)
										{
											Owner->SetPathPermission(ModuleOwner, Index, true, false);
										}
										else
										{
											Owner->SetPathPermission(ModuleOwner, Index, false, false);
										}
										PopulatePathPermissionRows(ModuleOwner, RowsBox);
									}
								}
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SButton)
							.Text(LOCTEXT("SettingsPathPermissionRemove", "删除"))
							.OnClicked_Lambda([this, ModuleOwner, Index, RowsBox]()
							{
								if (Owner)
								{
									Owner->RemovePathPermission(ModuleOwner, Index);
									PopulatePathPermissionRows(ModuleOwner, RowsBox);
								}
								return FReply::Handled();
							})
						]
					]
				]
			];
		}
	}

	TSharedRef<SWidget> BuildPathPermissionContent() const
	{
		TSharedRef<SVerticalBox> SectionsBox = SNew(SVerticalBox);
		if (!Owner)
		{
			SectionsBox->AddSlot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SettingsPathPermissionNoOwner", "Core module is not initialized; cannot configure path permissions."))
				.AutoWrapText(true)
			];
		}
		else
		{
			for (FName ModuleOwner : Owner->GetRegisteredCommandOwners())
			{
				TSharedRef<SVerticalBox> RowsBox = SNew(SVerticalBox);
				PopulatePathPermissionRows(ModuleOwner, RowsBox);
				TSharedPtr<SEditableTextBox> PathTextBox;

				SectionsBox->AddSlot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 10.0f)
				[
					SNew(SBorder)
					.Padding(10.0f)
					.BorderImage(&ListBorderBrush)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 0.0f, 0.0f, 6.0f)
						[
							SNew(STextBlock)
							.Text(FText::FromString(LoadModuleFiles(ModuleOwner).DisplayName))
							.Font(FAppStyle::GetFontStyle("HeadingSmall"))
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 0.0f, 0.0f, 8.0f)
						[
							RowsBox
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.FillWidth(1.0f)
							.VAlign(VAlign_Center)
							[
								SAssignNew(PathTextBox, SEditableTextBox)
								.MinDesiredWidth(260.0f)
								.HintText(LOCTEXT("SettingsPathPermissionHint", "/Game/..."))
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							.Padding(8.0f, 0.0f, 0.0f, 0.0f)
							[
								SNew(SButton)
								.Text(LOCTEXT("SettingsPathPermissionAddWritable", "添加可修改"))
								.OnClicked_Lambda([this, ModuleOwner, RowsBox, PathTextBox]()
								{
									if (Owner && PathTextBox.IsValid())
									{
										Owner->AddPathPermission(ModuleOwner, PathTextBox->GetText().ToString(), true, true);
										PathTextBox->SetText(FText::GetEmpty());
										PopulatePathPermissionRows(ModuleOwner, RowsBox);
									}
									return FReply::Handled();
								})
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							.Padding(6.0f, 0.0f, 0.0f, 0.0f)
							[
								SNew(SButton)
								.Text(LOCTEXT("SettingsPathPermissionAddReadonly", "添加只读"))
								.OnClicked_Lambda([this, ModuleOwner, RowsBox, PathTextBox]()
								{
									if (Owner && PathTextBox.IsValid())
									{
										Owner->AddPathPermission(ModuleOwner, PathTextBox->GetText().ToString(), true, false);
										PathTextBox->SetText(FText::GetEmpty());
										PopulatePathPermissionRows(ModuleOwner, RowsBox);
									}
									return FReply::Handled();
								})
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							.Padding(6.0f, 0.0f, 0.0f, 0.0f)
							[
								SNew(SButton)
								.Text(LOCTEXT("SettingsPathPermissionAddForbidden", "添加禁止"))
								.OnClicked_Lambda([this, ModuleOwner, RowsBox, PathTextBox]()
								{
									if (Owner && PathTextBox.IsValid())
									{
										Owner->AddPathPermission(ModuleOwner, PathTextBox->GetText().ToString(), false, false);
										PathTextBox->SetText(FText::GetEmpty());
										PopulatePathPermissionRows(ModuleOwner, RowsBox);
									}
									return FReply::Handled();
								})
							]
						]
					]
				];
			}
		}

		return SNew(SBorder)
			.Padding(12.0f)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				.Padding(0.0f, 0.0f, 0.0f, 10.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SettingsPathPermissionTitle", "路径权限"))
					.Font(GetSettingsTitleFont())
					.AutoWrapText(true)
				]
				+ SScrollBox::Slot()
				.Padding(0.0f, 0.0f, 0.0f, 10.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SettingsPathPermissionInfo", "每个 MCP 模块独立配置 UE 路径规则。未添加规则时不限制；添加规则后，列表中每个路径可以设为可修改、只读或禁止。点击条目右侧“修改”会按可修改、只读、禁止循环切换。支持 /Game/... 和 /All/Game/... 路径。"))
					.AutoWrapText(true)
				]
				+ SScrollBox::Slot()
				[
					SectionsBox
				]
			];
	}

	TSharedRef<SWidget> BuildHelpContent() const
	{
		return SNew(SBorder)
			.Padding(10.0f)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				.Padding(0.0f, 0.0f, 0.0f, 10.0f)
				[
					BuildHelpSection(FText::FromString(TEXT("支持功能")), {
						TEXT("核心：WebSocket MCP 服务、端口控制、调用日志、操作记录、模块开关、路径权限和资产重命名。"),
						TEXT("普通蓝图：资产查询/创建/描述、变量、函数、宏、自定义事件、注释、节点、Pin 默认值、连线和编译。"),
						TEXT("材质：资产创建/描述、材质属性、常用表达式节点、表达式属性、节点连线、输出连接和编译。"),
						TEXT("世界：大纲对象查询、Actor 放置、Transform 修改、实例属性修改、Bounds/尺寸查询、PostProcessVolume 操作。"),
						TEXT("模型：静态模型/静态模型组件信息、几何采样、材质槽、Socket 查询和编辑。"),
						TEXT("Niagara：系统/发射器创建、描述、发射器句柄、用户参数、模块插入、图节点、Pin 默认值、连线和编译。"),
					})
				]
				+ SScrollBox::Slot()
				.Padding(0.0f, 0.0f, 0.0f, 10.0f)
				[
					BuildHelpSection(FText::FromString(TEXT("待增加功能")), {
						TEXT("材质函数、复杂材质模板、更多材质节点类型和更完整的类型检查。"),
						TEXT("Niagara Scratch Pad、完整 Stack 脚本编辑、复杂参数绑定和高级验证。"),
						TEXT("材质实例、动画蓝图、行为树、PCG Graph 等后续模块。"),
						TEXT("对未加载关卡/外部包中的对象进行更完整的离线查询和修改。"),
					})
				]
				+ SScrollBox::Slot()
				[
					BuildHelpSection(FText::FromString(TEXT("注意事项")), {
						TEXT("路径权限为空时表示该模块不限制 UE 内容路径；添加规则后只允许匹配路径访问，修改命令还需要修改权限。"),
						TEXT("模块挂载开关只控制 MCP 命令是否响应；模块文档和配置文件仍会加载并显示。"),
						TEXT("普通蓝图和材质节点连线会尽量做类型校验，但复杂引脚仍建议在编辑器中编译确认。"),
						TEXT("世界对象操作当前面向已加载编辑器世界；未加载资产或关卡暂不作为首要支持目标。"),
					})
				]
			];
	}

	TSharedRef<SWidget> BuildHelpSection(const FText& Title, std::initializer_list<const TCHAR*> Lines) const
	{
		TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
		Box->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(STextBlock)
			.Text(Title)
			.Font(FAppStyle::GetFontStyle("HeadingSmall"))
		];

		for (const TCHAR* Line : Lines)
		{
			Box->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 2.0f, 0.0f, 2.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("- %s"), Line)))
				.AutoWrapText(true)
			];
		}

		return SNew(SBorder)
			.Padding(10.0f)
			.BorderImage(&ListBorderBrush)
			[
				Box
			];
	}

	FSlateFontInfo GetSettingsTitleFont() const
	{
		FSlateFontInfo TitleFont = FAppStyle::GetFontStyle("HeadingSmall");
		TitleFont.Size = FMath::Max(8, TitleFont.Size - 4);
		return TitleFont;
	}

	TSharedRef<SWidget> BuildSettingsTabButton(const FText& Label, int32 TabIndex)
	{
		FSlateFontInfo TabFont = FAppStyle::GetFontStyle("NormalFont");
		TabFont.Size = FMath::Max(8, TabFont.Size - 4);
		return SNew(SButton)
			.ContentPadding(FMargin(10.0f, 6.0f))
			.HAlign(HAlign_Fill)
			.ButtonColorAndOpacity_Lambda([this, TabIndex]()
			{
				return ActiveSettingsTabIndex == TabIndex
					? FSlateColor(FLinearColor(0.08f, 0.28f, 0.55f, 1.0f))
					: FSlateColor(FLinearColor(0.02f, 0.025f, 0.03f, 1.0f));
			})
			.OnClicked(this, &SWebFrameMcpPanel::OnSettingsTabClicked, TabIndex)
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(TabFont)
				.AutoWrapText(true)
			];
	}

	TSharedRef<SWidget> BuildGeneralSettingsContent(const TSharedRef<SVerticalBox>& ModuleListBox, const FString& LogDir)
	{
		return SNew(SBorder)
			.Padding(12.0f)
			.BorderImage(&ListBorderBrush)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				.Padding(0.0f, 0.0f, 0.0f, 10.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SettingsGeneralTitleV2", "通用设置"))
					.Font(GetSettingsTitleFont())
					.AutoWrapText(true)
				]
				+ SScrollBox::Slot()
				.Padding(0.0f, 2.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						return Owner && Owner->IsServerRunning()
							? FText::Format(LOCTEXT("SettingsServerRunningV2", "当前状态：运行中，地址 ws://127.0.0.1:{0}"), FText::AsNumber(Owner->GetPort()))
							: LOCTEXT("SettingsServerStoppedV2", "Current status: stopped");
					})
					.AutoWrapText(true)
				]
				+ SScrollBox::Slot()
				.Padding(0.0f, 2.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SettingsPortInfoV2", "Port configuration stays on the main page. Stop the service before changing the port."))
					.AutoWrapText(true)
				]
				+ SScrollBox::Slot()
				.Padding(0.0f, 2.0f)
				[
					SNew(STextBlock)
					.Text(FText::Format(LOCTEXT("SettingsLogDirV2", "日志目录：{0}"), FText::FromString(LogDir)))
					.AutoWrapText(true)
				]
				+ SScrollBox::Slot()
				.Padding(0.0f, 12.0f, 0.0f, 10.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SettingsModuleTitleV2", "MCP 模块挂载"))
					.Font(GetSettingsTitleFont())
					.AutoWrapText(true)
				]
				+ SScrollBox::Slot()
				.Padding(0.0f, 2.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SettingsModuleInfoV2", "When unchecked, this module stops responding to MCP commands immediately. Re-enable it to restore commands without restarting the editor."))
					.AutoWrapText(true)
				]
				+ SScrollBox::Slot()
				.Padding(0.0f, 8.0f, 0.0f, 4.0f)
				[
					ModuleListBox
				]
				+ SScrollBox::Slot()
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SButton)
						.Text(LOCTEXT("SettingsRefreshModulesV2", "刷新模块列表"))
						.OnClicked_Lambda([this, ModuleListBox]()
						{
							PopulateSettingsModuleList(ModuleListBox);
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					.Padding(10.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("SettingsModuleRefreshInfoV2", "Module switches take effect immediately. Refresh only rereads currently registered modules."))
						.AutoWrapText(true)
					]
				]
			];
	}

	TSharedRef<SWidget> BuildModuleSettingsContent(FName ModuleOwner)
	{
		const FWebFrameMcpModuleFiles Files = LoadModuleFiles(ModuleOwner);
		TSharedPtr<FJsonObject> ConfigObject;
		LoadJsonObjectFromString(Files.ConfigText, ConfigObject);

		TArray<TSharedPtr<FWebFrameMcpConfigSettingRow>> SettingRows;
		if (ConfigObject.IsValid())
		{
			const TSharedPtr<FJsonObject>* SettingsObject = nullptr;
			if (ConfigObject->TryGetObjectField(TEXT("settings"), SettingsObject) && SettingsObject && SettingsObject->IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*SettingsObject)->Values)
				{
					TSharedPtr<FWebFrameMcpConfigSettingRow> Row = MakeShared<FWebFrameMcpConfigSettingRow>();
					Row->Key = Field.Key;
					Row->OriginalValue = Field.Value;
					Row->TypeLabel = JsonValueTypeLabel(Field.Value);
					SettingRows.Add(Row);
				}
				SettingRows.Sort([](const TSharedPtr<FWebFrameMcpConfigSettingRow>& A, const TSharedPtr<FWebFrameMcpConfigSettingRow>& B)
				{
					return A.IsValid() && B.IsValid() && A->Key < B->Key;
				});
			}
		}

		TSharedRef<SVerticalBox> RowsBox = SNew(SVerticalBox);
		RowsBox->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			SNew(SBorder)
			.Padding(FMargin(10.0f, 6.0f))
			.BorderImage(&TableHeaderBrush)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(0.38f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SettingsModuleConfigNameHeader", "配置项"))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(0.16f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SettingsModuleConfigTypeHeader", "类型"))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(0.46f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SettingsModuleConfigValueHeader", "值"))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
			]
		];

		if (SettingRows.Num() == 0)
		{
			RowsBox->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 6.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SettingsModuleConfigEmpty", "这个模块当前没有可编辑配置项。"))
				.AutoWrapText(true)
			];
		}
		else
		{
			for (const TSharedPtr<FWebFrameMcpConfigSettingRow>& Row : SettingRows)
			{
				RowsBox->AddSlot()
				.AutoHeight()
				.Padding(0.0f, 3.0f)
				[
					SNew(SBorder)
					.Padding(FMargin(10.0f, 6.0f))
					.BorderImage(&TableRowBrush)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(0.38f)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(FText::FromString(Row->Key))
							.AutoWrapText(true)
						]
						+ SHorizontalBox::Slot()
						.FillWidth(0.16f)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(FText::FromString(Row->TypeLabel))
						]
						+ SHorizontalBox::Slot()
						.FillWidth(0.46f)
						.VAlign(VAlign_Center)
						[
							Row->OriginalValue.IsValid() && Row->OriginalValue->Type == EJson::Boolean
								? StaticCastSharedRef<SWidget>(
									SAssignNew(Row->BoolCheckBox, SCheckBox)
									.IsChecked(Row->OriginalValue->AsBool() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked))
								: StaticCastSharedRef<SWidget>(
									SAssignNew(Row->ValueTextBox, SEditableTextBox)
									.Text(FText::FromString(JsonValueToEditableText(Row->OriginalValue))))
						]
					]
				];
			}
		}

		return SNew(SBorder)
			.Padding(12.0f)
			.BorderImage(&ListBorderBrush)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 10.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Files.DisplayName))
						.Font(GetSettingsTitleFont())
						.AutoWrapText(true)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SButton)
						.Text(LOCTEXT("SettingsModuleDocButton", "说明文档"))
						.OnClicked(this, &SWebFrameMcpPanel::OnModuleDocumentationClicked, ModuleOwner)
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 6.0f)
				[
					SNew(STextBlock)
					.Text(FText::Format(LOCTEXT("SettingsModuleConfigPath", "配置文件：{0}"), FText::FromString(Files.ConfigPath)))
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				.Padding(0.0f, 0.0f, 0.0f, 8.0f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						RowsBox
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("SettingsModuleConfigSave", "保存配置"))
						.OnClicked_Lambda([this, ModuleOwner, SettingRows]()
						{
							const FWebFrameMcpModuleFiles Files = LoadModuleFiles(ModuleOwner);
							TSharedPtr<FJsonObject> ConfigObject;
							if (!LoadJsonObjectFromString(Files.ConfigText, ConfigObject))
							{
								ConfigObject = MakeShared<FJsonObject>();
							}

							if (!ConfigObject->HasField(TEXT("module")))
							{
								ConfigObject->SetStringField(TEXT("module"), ModuleConfigIdFromCommandOwner(ModuleOwner));
							}
							if (!ConfigObject->HasField(TEXT("displayName")))
							{
								ConfigObject->SetStringField(TEXT("displayName"), Files.DisplayName);
							}
							if (!ConfigObject->HasField(TEXT("version")))
							{
								ConfigObject->SetNumberField(TEXT("version"), 1);
							}

							TSharedPtr<FJsonObject> SettingsObject = MakeShared<FJsonObject>();
							for (const TSharedPtr<FWebFrameMcpConfigSettingRow>& Row : SettingRows)
							{
								if (!Row.IsValid())
								{
									continue;
								}

								if (Row->OriginalValue.IsValid() && Row->OriginalValue->Type == EJson::Boolean)
								{
									SettingsObject->SetBoolField(Row->Key, Row->BoolCheckBox.IsValid() && Row->BoolCheckBox->IsChecked());
									continue;
								}

								const FString TextValue = Row->ValueTextBox.IsValid() ? Row->ValueTextBox->GetText().ToString() : FString();
								if (Row->OriginalValue.IsValid() && Row->OriginalValue->Type == EJson::Number)
								{
									double NumberValue = 0.0;
									if (LexTryParseString(NumberValue, *TextValue))
									{
										SettingsObject->SetNumberField(Row->Key, NumberValue);
									}
									else
									{
										SettingsObject->SetNumberField(Row->Key, Row->OriginalValue->AsNumber());
									}
									continue;
								}

								SettingsObject->SetStringField(Row->Key, TextValue);
							}
							ConfigObject->SetObjectField(TEXT("settings"), SettingsObject);

							FString OutputText;
							const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutputText);
							FJsonSerializer::Serialize(ConfigObject.ToSharedRef(), Writer);

							IFileManager::Get().MakeDirectory(*FPaths::GetPath(Files.ConfigPath), true);
							FFileHelper::SaveStringToFile(OutputText, *Files.ConfigPath, FFileHelper::EEncodingOptions::ForceUTF8);
							if (Owner)
							{
								Owner->AddConsoleLine(FString::Printf(TEXT("[%s] Saved MCP module config: %s"),
									*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")),
									*Files.ConfigPath));
							}
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("SettingsModuleConfigReload", "重载配置"))
						.OnClicked_Lambda([ModuleOwner, SettingRows]()
						{
							const FWebFrameMcpModuleFiles Files = LoadModuleFiles(ModuleOwner);
							TSharedPtr<FJsonObject> ConfigObject;
							if (LoadJsonObjectFromString(Files.ConfigText, ConfigObject))
							{
								const TSharedPtr<FJsonObject>* SettingsObject = nullptr;
								if (ConfigObject->TryGetObjectField(TEXT("settings"), SettingsObject) && SettingsObject && SettingsObject->IsValid())
								{
									for (const TSharedPtr<FWebFrameMcpConfigSettingRow>& Row : SettingRows)
									{
										if (!Row.IsValid())
										{
											continue;
										}
										const TSharedPtr<FJsonValue>* NewValue = (*SettingsObject)->Values.Find(Row->Key);
										if (!NewValue || !NewValue->IsValid())
										{
											continue;
										}
										Row->OriginalValue = *NewValue;
										Row->TypeLabel = JsonValueTypeLabel(*NewValue);
										if ((*NewValue)->Type == EJson::Boolean && Row->BoolCheckBox.IsValid())
										{
											Row->BoolCheckBox->SetIsChecked((*NewValue)->AsBool() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked);
										}
										else if (Row->ValueTextBox.IsValid())
										{
											Row->ValueTextBox->SetText(FText::FromString(JsonValueToEditableText(*NewValue)));
										}
									}
								}
							}
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					.Padding(10.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("SettingsModuleConfigInfo", "这里会把模块配置文件中的 settings 项转成表格。保存时由插件重新写入合法 JSON。"))
						.AutoWrapText(true)
					]
				]
			];
	}

	TSharedRef<SWidget> BuildSettingsContentV2()
	{
		const FString LogDir = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("WebFrameMcp"), TEXT("Saved"), TEXT("Logs"));
		TSharedRef<SVerticalBox> ModuleListBox = SNew(SVerticalBox);
		PopulateSettingsModuleList(ModuleListBox);

		SettingsModuleOwners = Owner ? Owner->GetRegisteredCommandOwners() : TArray<FName>();
		ActiveSettingsTabIndex = 0;

		TSharedRef<SVerticalBox> TabBox = SNew(SVerticalBox);
		TabBox->AddSlot()
		.AutoHeight()
		[
			BuildSettingsTabButton(LOCTEXT("SettingsGeneralTabV2", "通用"), 0)
		];
		TabBox->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			BuildSettingsTabButton(LOCTEXT("SettingsPathPermissionTabV2", "路径权限"), 1)
		];

		for (int32 Index = 0; Index < SettingsModuleOwners.Num(); ++Index)
		{
			const FName ModuleOwner = SettingsModuleOwners[Index];
			const int32 TabIndex = Index + 2;
			TabBox->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				BuildSettingsTabButton(FText::FromString(LoadModuleFiles(ModuleOwner).DisplayName), TabIndex)
			];
		}

		SAssignNew(SettingsContentSwitcher, SWidgetSwitcher);
		SettingsContentSwitcher->AddSlot()
		[
			BuildGeneralSettingsContent(ModuleListBox, LogDir)
		];
		SettingsContentSwitcher->AddSlot()
		[
			BuildPathPermissionContent()
		];
		for (FName ModuleOwner : SettingsModuleOwners)
		{
			SettingsContentSwitcher->AddSlot()
			[
				BuildModuleSettingsContent(ModuleOwner)
			];
		}

		return SNew(SBorder)
			.Padding(10.0f)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			[
				SNew(SSplitter)
				+ SSplitter::Slot()
				.Value(0.24f)
				[
					SNew(SBorder)
					.Padding(8.0f)
					.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
					[
						TabBox
					]
				]
				+ SSplitter::Slot()
				.Value(0.76f)
				[
					SettingsContentSwitcher.ToSharedRef()
				]
			];
	}

	TSharedRef<SWidget> BuildModuleDocumentationContent(FName ModuleOwner) const
	{
		const FWebFrameMcpModuleFiles Files = LoadModuleFiles(ModuleOwner);
		return SNew(SBorder)
			.Padding(10.0f)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 8.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Files.DocumentationPath))
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SNew(SMultiLineEditableTextBox)
					.Text(FText::FromString(Files.DocumentationText))
					.Font(FAppStyle::GetFontStyle("Log.Normal"))
					.IsReadOnly(true)
					.AutoWrapText(true)
				]
			];
	}

	FReply OnModuleDocumentationClicked(FName ModuleOwner)
	{
		const FWebFrameMcpModuleFiles Files = LoadModuleFiles(ModuleOwner);
		TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(FText::Format(LOCTEXT("ModuleDocWindowTitle", "{0} MCP 说明文档"), FText::FromString(Files.DisplayName)))
			.ClientSize(FVector2D(860.0f, 700.0f))
			.SupportsMaximize(true)
			.SupportsMinimize(false)
			[
				BuildModuleDocumentationContent(ModuleOwner)
			];

		FSlateApplication::Get().AddWindow(Window);
		return FReply::Handled();
	}

	FReply OnSettingsTabClicked(int32 TabIndex)
	{
		ActiveSettingsTabIndex = TabIndex;
		if (SettingsContentSwitcher.IsValid())
		{
			SettingsContentSwitcher->SetActiveWidgetIndex(ActiveSettingsTabIndex);
		}
		return FReply::Handled();
	}

	FReply OnSettingsClicked()
	{
		TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(LOCTEXT("SettingsWindowTitle", "MCP 设置"))
			.ClientSize(FVector2D(860.0f, 620.0f))
			.SupportsMaximize(true)
			.SupportsMinimize(false)
			[
				BuildSettingsContentV2()
			];

		FSlateApplication::Get().AddWindow(Window);
		return FReply::Handled();
	}

	FReply OnHelpClicked()
	{
		TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(LOCTEXT("HelpWindowTitle", "MCP 说明"))
			.ClientSize(FVector2D(840.0f, 680.0f))
			.SupportsMaximize(true)
			.SupportsMinimize(false)
			[
				BuildHelpContent()
			];

		FSlateApplication::Get().AddWindow(Window);
		return FReply::Handled();
	}

	FReply OnToggleServerClicked()
	{
		if (Owner)
		{
			Owner->ToggleServer();
		}
		return FReply::Handled();
	}

	FReply OnClearChangesClicked()
	{
		if (Owner)
		{
			Owner->ClearBlueprintChangeRecords();
		}
		return FReply::Handled();
	}

	FReply OnOpenRecordTargetClicked(FString AssetPath)
	{
		if (GEditor)
		{
			UObject* Object = StaticFindObject(UObject::StaticClass(), nullptr, *AssetPath);
			if (!Object)
			{
				Object = LoadObject<UObject>(nullptr, *AssetPath);
			}
			if (AActor* Actor = Cast<AActor>(Object))
			{
				GEditor->SelectNone(false, true);
				GEditor->SelectActor(Actor, true, true);
			}
			else if (Object)
			{
				GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Object);
			}
		}
		return FReply::Handled();
	}

	void OnPortCommitted(const FText& NewText, ETextCommit::Type CommitType)
	{
		if (!Owner || Owner->IsServerRunning())
		{
			return;
		}

		FString PortText = NewText.ToString();
		PortText.ReplaceInline(TEXT(","), TEXT(""));
		PortText.ReplaceInline(TEXT(" "), TEXT(""));
		int32 ParsedPort = 0;
		if (LexTryParseString(ParsedPort, *PortText))
		{
			Owner->SetPort(ParsedPort);
		}
	}

	FWebFrameMcpModule* Owner = nullptr;
	TSharedPtr<SScrollBox> ConsoleBox;
	TSharedPtr<SScrollBox> BlueprintChangeBox;
	TSharedPtr<SEditableTextBox> PortTextBox;
	TSharedPtr<SWidgetSwitcher> SettingsContentSwitcher;
	TArray<FName> SettingsModuleOwners;
	int32 ActiveSettingsTabIndex = 0;
	FSlateRoundedBoxBrush ConsoleBorderBrush = FSlateRoundedBoxBrush(FLinearColor::Black, 6.0f, FLinearColor(0.25f, 0.45f, 0.55f), 1.0f);
	FSlateRoundedBoxBrush ListBorderBrush = FSlateRoundedBoxBrush(FLinearColor(0.015f, 0.018f, 0.022f), 6.0f, FLinearColor(0.18f, 0.22f, 0.28f), 1.0f);
	FSlateRoundedBoxBrush TableHeaderBrush = FSlateRoundedBoxBrush(FLinearColor(0.035f, 0.04f, 0.048f), 5.0f, FLinearColor(0.28f, 0.34f, 0.42f), 1.0f);
	FSlateRoundedBoxBrush TableRowBrush = FSlateRoundedBoxBrush(FLinearColor(0.018f, 0.022f, 0.028f), 4.0f, FLinearColor(0.13f, 0.16f, 0.20f), 1.0f);
};

void FWebFrameMcpModule::StartupModule()
{
	StyleSet = MakeShared<FSlateStyleSet>(WebFrameMcp::StyleName);
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("WebFrameMcp")))
	{
		StyleSet->SetContentRoot(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources")));
		StyleSet->Set(TEXT("WebFrameMcp.Icon16"), new FSlateImageBrush(StyleSet->RootToContentDir(TEXT("Icon128.png")), FVector2D(16.0f, 16.0f)));
		StyleSet->Set(TEXT("WebFrameMcp.Icon40"), new FSlateImageBrush(StyleSet->RootToContentDir(TEXT("Icon128.png")), FVector2D(40.0f, 40.0f)));
		FSlateStyleRegistry::RegisterSlateStyle(*StyleSet);
	}

	LoadUserSettings();

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		WebFrameMcp::TabName,
		FOnSpawnTab::CreateRaw(this, &FWebFrameMcpModule::SpawnPanelTab))
		.SetDisplayName(LOCTEXT("TabTitle", "MCP"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

	RegisterToolbar();

	RegisterCommand({
		TEXT("WebFrameMcpCore"),
		TEXT("asset.rename"),
		TEXT("Rename an editor asset and keep redirector flow in UE asset tools."),
		[this](const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			return RenameAsset(Params, OutError);
		}
	});

	auto RecordOperationHandler = [this](const TSharedPtr<FJsonObject>& Params, FString& OutError) -> TSharedPtr<FJsonObject>
	{
		FString AssetPath;
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		if (AssetPath.IsEmpty())
		{
			OutError = TEXT("Missing params.asset.");
			return nullptr;
		}

		FString Summary;
		Params->TryGetStringField(TEXT("summary"), Summary);
		TArray<TPair<FString, FString>> Details;
		const TArray<TSharedPtr<FJsonValue>>* DetailValues = nullptr;
		if (Params->TryGetArrayField(TEXT("details"), DetailValues) && DetailValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *DetailValues)
			{
				const TSharedPtr<FJsonObject>* DetailObject = nullptr;
				if (Value.IsValid() && Value->TryGetObject(DetailObject) && DetailObject && DetailObject->IsValid())
				{
					FString Object;
					FString Content;
					(*DetailObject)->TryGetStringField(TEXT("object"), Object);
					(*DetailObject)->TryGetStringField(TEXT("content"), Content);
					if (!Object.IsEmpty() || !Content.IsEmpty())
					{
						Details.Add(TPair<FString, FString>(Object, Content));
					}
				}
			}
		}
		RecordBlueprintChange(AssetPath, Summary, Details);
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset"), AssetPath);
		Result->SetStringField(TEXT("recorded"), TEXT("true"));
		return Result;
	};

	RegisterCommand({ TEXT("WebFrameMcpCore"), TEXT("mcp.record_operation"), TEXT("Record an operation in the shared operation list."), RecordOperationHandler });
	RegisterCommand({ TEXT("WebFrameMcpCore"), TEXT("blueprint.record_change"), TEXT("Compatibility alias for mcp.record_operation."), RecordOperationHandler });

	if (bStartServerOnStartup)
	{
		StartServer();
	}
}

void FWebFrameMcpModule::ShutdownModule()
{
	StopServer();
	UnregisterToolbar();
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(WebFrameMcp::TabName);
	if (StyleSet.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet);
		StyleSet.Reset();
	}
	PanelWidget.Reset();
}

void FWebFrameMcpModule::OpenPanel()
{
	FGlobalTabmanager::Get()->TryInvokeTab(WebFrameMcp::TabName);
}

void FWebFrameMcpModule::ClearBlueprintChangeRecords()
{
	BlueprintChanges.Empty();
	if (PanelWidget.IsValid())
	{
		PanelWidget->RefreshBlueprintChanges();
	}
}

void FWebFrameMcpModule::RegisterCommand(const FWebFrameMcpCommandSpec& Spec)
{
	if (Spec.Method.IsEmpty() || !Spec.Handler)
	{
		return;
	}
	CommandOwnerEnabled.FindOrAdd(Spec.Owner) = IsCommandOwnerEnabled(Spec.Owner);
	RegisteredCommands.Add(Spec.Method, Spec);
}

void FWebFrameMcpModule::UnregisterCommandsByOwner(FName Owner)
{
	for (TMap<FString, FWebFrameMcpCommandSpec>::TIterator It(RegisteredCommands); It; ++It)
	{
		if (It.Value().Owner == Owner)
		{
			It.RemoveCurrent();
		}
	}
}

TArray<FName> FWebFrameMcpModule::GetRegisteredCommandOwners() const
{
	TArray<FName> Owners;
	for (const TPair<FString, FWebFrameMcpCommandSpec>& Pair : RegisteredCommands)
	{
		if (!Owners.Contains(Pair.Value.Owner))
		{
			Owners.Add(Pair.Value.Owner);
		}
	}

	Owners.Sort([](const FName& A, const FName& B)
	{
		return A.ToString() < B.ToString();
	});
	return Owners;
}

int32 FWebFrameMcpModule::GetRegisteredCommandCountByOwner(FName Owner) const
{
	int32 Count = 0;
	for (const TPair<FString, FWebFrameMcpCommandSpec>& Pair : RegisteredCommands)
	{
		if (Pair.Value.Owner == Owner)
		{
			++Count;
		}
	}
	return Count;
}

bool FWebFrameMcpModule::IsCommandOwnerEnabled(FName Owner) const
{
	if (const bool* bEnabled = CommandOwnerEnabled.Find(Owner))
	{
		return *bEnabled;
	}
	return true;
}

void FWebFrameMcpModule::SetCommandOwnerEnabled(FName Owner, bool bEnabled)
{
	CommandOwnerEnabled.FindOrAdd(Owner) = bEnabled;
	AddConsoleLine(FString::Printf(TEXT("[%s] MCP module %s %s."),
		*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")),
		*Owner.ToString(),
		bEnabled ? TEXT("enabled") : TEXT("disabled")));
	SaveUserSettings();
}

const TArray<FWebFrameMcpPathPermission>& FWebFrameMcpModule::GetPathPermissions(FName Owner) const
{
	static const TArray<FWebFrameMcpPathPermission> EmptyPermissions;
	if (const TArray<FWebFrameMcpPathPermission>* Permissions = PathPermissionsByOwner.Find(Owner))
	{
		return *Permissions;
	}
	return EmptyPermissions;
}

void FWebFrameMcpModule::AddPathPermission(FName Owner, const FString& Path, bool bAllowAccess, bool bAllowModify)
{
	const FString NormalizedPath = WebFrameMcp::NormalizeMcpPermissionPath(Path);
	if (!WebFrameMcp::IsMcpPermissionPath(NormalizedPath))
	{
		AddConsoleLine(FString::Printf(TEXT("[%s] Invalid MCP path permission ignored: %s"),
			*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")),
			*Path));
		return;
	}

	TArray<FWebFrameMcpPathPermission>& Permissions = PathPermissionsByOwner.FindOrAdd(Owner);
	for (FWebFrameMcpPathPermission& Permission : Permissions)
	{
		if (WebFrameMcp::NormalizeMcpPermissionPath(Permission.Path) == NormalizedPath)
		{
			Permission.Path = NormalizedPath;
			Permission.bAllowAccess = bAllowAccess;
			Permission.bAllowModify = bAllowModify;
			SaveUserSettings();
			return;
		}
	}

	FWebFrameMcpPathPermission Permission;
	Permission.Path = NormalizedPath;
	Permission.bAllowAccess = bAllowAccess;
	Permission.bAllowModify = bAllowModify;
	Permissions.Add(Permission);
	SaveUserSettings();
}

void FWebFrameMcpModule::RemovePathPermission(FName Owner, int32 Index)
{
	if (TArray<FWebFrameMcpPathPermission>* Permissions = PathPermissionsByOwner.Find(Owner))
	{
		if (Permissions->IsValidIndex(Index))
		{
			Permissions->RemoveAt(Index);
			SaveUserSettings();
		}
	}
}

void FWebFrameMcpModule::SetPathPermission(FName Owner, int32 Index, bool bAllowAccess, bool bAllowModify)
{
	if (TArray<FWebFrameMcpPathPermission>* Permissions = PathPermissionsByOwner.Find(Owner))
	{
		if (Permissions->IsValidIndex(Index))
		{
			(*Permissions)[Index].bAllowAccess = bAllowAccess;
			(*Permissions)[Index].bAllowModify = bAllowModify;
			SaveUserSettings();
		}
	}
}

void FWebFrameMcpModule::RecordSuccessfulCommandIfNeeded(const FString& Method, const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& Result)
{
	const FString LowerMethod = Method.ToLower();
	if (LowerMethod.IsEmpty() ||
		LowerMethod == TEXT("ping") ||
		LowerMethod == TEXT("mcp.ping") ||
		LowerMethod == TEXT("mcp.status") ||
		LowerMethod == TEXT("mcp.record_operation") ||
		LowerMethod == TEXT("blueprint.record_change") ||
		LowerMethod.Contains(TEXT(".list")) ||
		LowerMethod.Contains(TEXT(".describe")) ||
		LowerMethod.Contains(TEXT(".variables")) ||
		LowerMethod.Contains(TEXT(".get")) ||
		LowerMethod.Contains(TEXT(".can")))
	{
		return;
	}
	if (Result.IsValid())
	{
		bool bOperationRecorded = false;
		if (Result->TryGetBoolField(TEXT("operationRecorded"), bOperationRecorded) && bOperationRecorded)
		{
			return;
		}
	}

	if (LowerMethod.StartsWith(TEXT("blueprint.")) ||
		LowerMethod.StartsWith(TEXT("material.")) ||
		LowerMethod.StartsWith(TEXT("mesh.")) ||
		LowerMethod.StartsWith(TEXT("world.")) ||
		LowerMethod == TEXT("asset.rename"))
	{
		return;
	}

	auto TryGetPath = [](const TSharedPtr<FJsonObject>& Object, std::initializer_list<const TCHAR*> Names, FString& OutPath) -> bool
	{
		if (!Object.IsValid())
		{
			return false;
		}
		for (const TCHAR* Name : Names)
		{
			if (Object->TryGetStringField(Name, OutPath) && !OutPath.IsEmpty())
			{
				return true;
			}
		}
		return false;
	};

	FString TargetPath;
	if (!TryGetPath(Result, { TEXT("asset"), TEXT("targetAsset"), TEXT("actor"), TEXT("world"), TEXT("pathName") }, TargetPath))
	{
		TryGetPath(Params, { TEXT("asset"), TEXT("targetAsset"), TEXT("actor"), TEXT("pathName"), TEXT("class"), TEXT("emitterAsset") }, TargetPath);
	}
	if (TargetPath.IsEmpty())
	{
		return;
	}

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(Method, TEXT("MCP command completed successfully.")));
	RecordBlueprintChange(TargetPath, FString::Printf(TEXT("MCP %s"), *Method), Details);
}

FString FWebFrameMcpModule::ResolveSettingsPath() const
{
	return FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("WebFrameMcp"), TEXT("Saved"), TEXT("Settings"), TEXT("McpSettings.json"));
}

void FWebFrameMcpModule::LoadUserSettings()
{
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *ResolveSettingsPath()))
	{
		return;
	}

	TSharedPtr<FJsonObject> RootObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		return;
	}

	double SavedPort = 0.0;
	if (RootObject->TryGetNumberField(TEXT("port"), SavedPort))
	{
		Port = FMath::Clamp(static_cast<int32>(SavedPort), 1, 65535);
	}
	RootObject->TryGetBoolField(TEXT("startServerOnStartup"), bStartServerOnStartup);

	CommandOwnerEnabled.Empty();
	const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
	if (RootObject->TryGetArrayField(TEXT("modules"), Modules))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Modules)
		{
			const TSharedPtr<FJsonObject>* ModuleObject = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(ModuleObject) || !ModuleObject || !ModuleObject->IsValid())
			{
				continue;
			}

			FString OwnerName;
			bool bEnabled = true;
			(*ModuleObject)->TryGetStringField(TEXT("owner"), OwnerName);
			(*ModuleObject)->TryGetBoolField(TEXT("enabled"), bEnabled);
			if (!OwnerName.IsEmpty())
			{
				CommandOwnerEnabled.Add(FName(*OwnerName), bEnabled);
			}
		}
	}

	PathPermissionsByOwner.Empty();
	const TArray<TSharedPtr<FJsonValue>>* PathPermissionModules = nullptr;
	if (RootObject->TryGetArrayField(TEXT("pathPermissions"), PathPermissionModules))
	{
		for (const TSharedPtr<FJsonValue>& Value : *PathPermissionModules)
		{
			const TSharedPtr<FJsonObject>* ModuleObject = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(ModuleObject) || !ModuleObject || !ModuleObject->IsValid())
			{
				continue;
			}

			FString OwnerName;
			(*ModuleObject)->TryGetStringField(TEXT("owner"), OwnerName);
			if (OwnerName.IsEmpty())
			{
				continue;
			}

			TArray<FWebFrameMcpPathPermission>& Permissions = PathPermissionsByOwner.FindOrAdd(FName(*OwnerName));
			const TArray<TSharedPtr<FJsonValue>>* Rules = nullptr;
			if ((*ModuleObject)->TryGetArrayField(TEXT("rules"), Rules))
			{
				for (const TSharedPtr<FJsonValue>& RuleValue : *Rules)
				{
					const TSharedPtr<FJsonObject>* RuleObject = nullptr;
					if (!RuleValue.IsValid() || !RuleValue->TryGetObject(RuleObject) || !RuleObject || !RuleObject->IsValid())
					{
						continue;
					}

					FWebFrameMcpPathPermission Permission;
					(*RuleObject)->TryGetStringField(TEXT("path"), Permission.Path);
					(*RuleObject)->TryGetBoolField(TEXT("allowAccess"), Permission.bAllowAccess);
					(*RuleObject)->TryGetBoolField(TEXT("allowModify"), Permission.bAllowModify);
					Permission.Path = WebFrameMcp::NormalizeMcpPermissionPath(Permission.Path);
					if (WebFrameMcp::IsMcpPermissionPath(Permission.Path))
					{
						Permissions.Add(Permission);
					}
				}
			}
		}
	}
}

void FWebFrameMcpModule::SaveUserSettings() const
{
	TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetNumberField(TEXT("port"), Port);
	RootObject->SetBoolField(TEXT("startServerOnStartup"), bStartServerOnStartup);

	TArray<TSharedPtr<FJsonValue>> Modules;
	for (const TPair<FName, bool>& Pair : CommandOwnerEnabled)
	{
		TSharedPtr<FJsonObject> ModuleObject = MakeShared<FJsonObject>();
		ModuleObject->SetStringField(TEXT("owner"), Pair.Key.ToString());
		ModuleObject->SetBoolField(TEXT("enabled"), Pair.Value);
		Modules.Add(MakeShared<FJsonValueObject>(ModuleObject));
	}
	RootObject->SetArrayField(TEXT("modules"), Modules);

	TArray<TSharedPtr<FJsonValue>> PathPermissionModules;
	for (const TPair<FName, TArray<FWebFrameMcpPathPermission>>& Pair : PathPermissionsByOwner)
	{
		TSharedPtr<FJsonObject> ModuleObject = MakeShared<FJsonObject>();
		ModuleObject->SetStringField(TEXT("owner"), Pair.Key.ToString());

		TArray<TSharedPtr<FJsonValue>> Rules;
		for (const FWebFrameMcpPathPermission& Permission : Pair.Value)
		{
			TSharedPtr<FJsonObject> RuleObject = MakeShared<FJsonObject>();
			RuleObject->SetStringField(TEXT("path"), Permission.Path);
			RuleObject->SetBoolField(TEXT("allowAccess"), Permission.bAllowAccess);
			RuleObject->SetBoolField(TEXT("allowModify"), Permission.bAllowModify);
			Rules.Add(MakeShared<FJsonValueObject>(RuleObject));
		}

		ModuleObject->SetArrayField(TEXT("rules"), Rules);
		PathPermissionModules.Add(MakeShared<FJsonValueObject>(ModuleObject));
	}
	RootObject->SetArrayField(TEXT("pathPermissions"), PathPermissionModules);

	FString JsonText;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
	if (FJsonSerializer::Serialize(RootObject.ToSharedRef(), Writer))
	{
		const FString SettingsPath = ResolveSettingsPath();
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(SettingsPath), true);
	FFileHelper::SaveStringToFile(JsonText, *SettingsPath, FFileHelper::EEncodingOptions::ForceUTF8);
	}
}

bool FWebFrameMcpModule::ValidatePathPermissionsForCommand(const FWebFrameMcpCommandSpec& Command, const TSharedPtr<FJsonObject>& Params, FString& OutError) const
{
	const TArray<FWebFrameMcpPathPermission>& Permissions = GetPathPermissions(Command.Owner);
	if (Permissions.Num() == 0)
	{
		return true;
	}

	TArray<FString> Paths;
	WebFrameMcp::CollectMcpPermissionPathsFromJsonObject(Params, Paths);
	if (Paths.Num() == 0)
	{
		return true;
	}

	const FString Method = Command.Method.ToLower();
	const bool bModifyCommand =
		!(Method.Contains(TEXT(".list")) ||
		  Method.Contains(TEXT(".describe")) ||
		  Method.Contains(TEXT(".get")) ||
		  Method.Contains(TEXT(".can")) ||
		  Method.Contains(TEXT(".status")));

	for (const FString& RawPath : Paths)
	{
		const FString Path = WebFrameMcp::NormalizeMcpPermissionPath(RawPath);
		const FWebFrameMcpPathPermission* BestMatch = nullptr;
		int32 BestLength = INDEX_NONE;

		for (const FWebFrameMcpPathPermission& Permission : Permissions)
		{
			const FString RulePath = WebFrameMcp::NormalizeMcpPermissionPath(Permission.Path);
			const bool bMatches = Path == RulePath || Path.StartsWith(RulePath + TEXT("/")) || Path.StartsWith(RulePath + TEXT("."));
			if (bMatches && RulePath.Len() > BestLength)
			{
				BestMatch = &Permission;
				BestLength = RulePath.Len();
			}
		}

		if (!BestMatch)
		{
			OutError = FString::Printf(TEXT("MCP module %s has no path permission for %s."),
				*Command.Owner.ToString(),
				*Path);
			return false;
		}
		if (!BestMatch->bAllowAccess)
		{
			OutError = FString::Printf(TEXT("MCP module %s cannot access path %s."),
				*Command.Owner.ToString(),
				*Path);
			return false;
		}
		if (bModifyCommand && !BestMatch->bAllowModify)
		{
			OutError = FString::Printf(TEXT("MCP module %s cannot modify path %s."),
				*Command.Owner.ToString(),
				*Path);
			return false;
		}
	}

	return true;
}

void FWebFrameMcpModule::RecordBlueprintChange(const FString& AssetPath, const FString& Summary, const TArray<TPair<FString, FString>>& Details)
{
	TArray<TPair<FString, FString>> NormalizedDetails = Details;
	if (NormalizedDetails.Num() == 0)
	{
		NormalizedDetails.Add(TPair<FString, FString>(
			Summary.IsEmpty() ? TEXT("修改") : Summary,
			FString()));
	}

	for (TSharedPtr<FWebFrameMcpBlueprintChange>& ExistingRecord : BlueprintChanges)
	{
		if (!ExistingRecord.IsValid() || ExistingRecord->AssetPath != AssetPath)
		{
			continue;
		}

		ExistingRecord->Details.Append(NormalizedDetails);
		ExistingRecord->Summary = ExistingRecord->Details.Num() > 1
			? TEXT("多个修改")
			: (Summary.IsEmpty() ? ExistingRecord->Summary : Summary);

		if (ExistingRecord->Details.Num() > 500)
		{
			ExistingRecord->Details.RemoveAt(0, ExistingRecord->Details.Num() - 500);
		}

		if (PanelWidget.IsValid())
		{
			PanelWidget->RefreshBlueprintChanges();
		}
		return;
	}

	TSharedPtr<FWebFrameMcpBlueprintChange> Record = MakeShared<FWebFrameMcpBlueprintChange>();
	Record->AssetPath = AssetPath;
	Record->ObjectName = WebFrameMcp::AssetNameFromPath(AssetPath);
	Record->Summary = Summary.IsEmpty() ? TEXT("多个修改") : Summary;
	Record->Details = NormalizedDetails;
	if (NormalizedDetails.Num() > 1)
	{
		Record->Summary = TEXT("多个修改");
	}
	else if (Summary.IsEmpty())
	{
		Record->Summary = TEXT("修改");
	}
	BlueprintChanges.Add(Record);

	if (BlueprintChanges.Num() > 200)
	{
		BlueprintChanges.RemoveAt(0, BlueprintChanges.Num() - 200);
	}

	if (PanelWidget.IsValid())
	{
		PanelWidget->RefreshBlueprintChanges();
	}
}

TSharedRef<SDockTab> FWebFrameMcpModule::SpawnPanelTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SAssignNew(PanelWidget, SWebFrameMcpPanel, this)
		];
}

void FWebFrameMcpModule::RegisterToolbar()
{
	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
	ToolbarExtender = MakeShared<FExtender>();
	ToolbarExtender->AddToolBarExtension(
		"Play",
		EExtensionHook::After,
		nullptr,
		FToolBarExtensionDelegate::CreateLambda([this](FToolBarBuilder& Builder)
		{
			Builder.AddToolBarButton(
				FUIAction(FExecuteAction::CreateRaw(this, &FWebFrameMcpModule::OpenPanel)),
				NAME_None,
				LOCTEXT("ToolbarButton", "MCP"),
				LOCTEXT("ToolbarTooltip", "Open the MCP control panel."),
				FSlateIcon(WebFrameMcp::StyleName, "WebFrameMcp.Icon40", "WebFrameMcp.Icon16"));
		}));
	LevelEditorModule.GetToolBarExtensibilityManager()->AddExtender(ToolbarExtender);
}

void FWebFrameMcpModule::UnregisterToolbar()
{
	if (!ToolbarExtender.IsValid())
	{
		return;
	}
	if (FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
	{
		FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
		LevelEditorModule.GetToolBarExtensibilityManager()->RemoveExtender(ToolbarExtender);
	}
	ToolbarExtender.Reset();
}

void FWebFrameMcpModule::SetPort(int32 InPort)
{
	if (bServerRunning)
	{
		AddConsoleLine(FString::Printf(TEXT("[%s] Cannot change MCP WebSocket port while the server is running."),
			*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S"))));
		return;
	}
	Port = FMath::Clamp(InPort, 1, 65535);
	SaveUserSettings();
}

void FWebFrameMcpModule::ToggleServer()
{
	if (bServerRunning)
	{
		StopServer();
		bStartServerOnStartup = false;
		SaveUserSettings();
		const FString Line = FString::Printf(TEXT("[%s] MCP WebSocket server stopped."),
			*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")));
		AddConsoleLine(Line);
		return;
	}

	if (!StartServer())
	{
		const FString Line = FString::Printf(TEXT("[%s] Failed to start MCP WebSocket server on port %d."),
			*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")),
			Port);
		AddConsoleLine(Line);
		return;
	}

	bStartServerOnStartup = true;
	SaveUserSettings();
}

bool FWebFrameMcpModule::StartServer()
{
	if (bServerRunning)
	{
		return true;
	}

	IWebSocketNetworkingModule* WsModule = FModuleManager::Get().LoadModulePtr<IWebSocketNetworkingModule>(TEXT("WebSocketNetworking"));
	if (!WsModule)
	{
		UE_LOG(LogWebFrameMcp, Error, TEXT("WebSocketNetworking module is not available."));
		return false;
	}

	WebSocketServer = WsModule->CreateServer();
	if (!WebSocketServer)
	{
		UE_LOG(LogWebFrameMcp, Error, TEXT("Failed to create WebSocket server."));
		return false;
	}

	if (!WebSocketServer->Init(static_cast<uint32>(Port), FWebSocketClientConnectedCallBack::CreateRaw(this, &FWebFrameMcpModule::OnClientConnected)))
	{
		WebSocketServer.Reset();
		UE_LOG(LogWebFrameMcp, Error, TEXT("Failed to bind MCP WebSocket server on port %d."), Port);
		return false;
	}

	bServerRunning = true;
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FWebFrameMcpModule::TickServer));
	const FString Line = FString::Printf(TEXT("[%s] MCP WebSocket server listening on ws://127.0.0.1:%d"),
		*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")),
		Port);
	AddConsoleLine(Line);
	UE_LOG(LogWebFrameMcp, Log, TEXT("%s"), *Line);
	return true;
}

void FWebFrameMcpModule::StopServer()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	Clients.Empty();
	WebSocketServer.Reset();
	bServerRunning = false;
}

bool FWebFrameMcpModule::TickServer(float DeltaTime)
{
	if (WebSocketServer)
	{
		WebSocketServer->Tick();
	}
	return true;
}

void FWebFrameMcpModule::OnClientConnected(INetworkingWebSocket* Peer)
{
	if (!Peer)
	{
		return;
	}

	Clients.Add(Peer);
	Peer->SetReceiveCallBack(FWebSocketPacketReceivedCallBack::CreateLambda(
		[this, Peer](void* Data, int32 Size)
		{
			HandleClientMessage(Peer, Data, Size);
		}));
	Peer->SetSocketClosedCallBack(FWebSocketInfoCallBack::CreateLambda([this, Peer]()
	{
		Clients.Remove(Peer);
	}));

	TSharedPtr<FJsonObject> Hello = MakeShared<FJsonObject>();
	Hello->SetStringField(TEXT("op"), TEXT("hello"));
	Hello->SetStringField(TEXT("server"), TEXT("WebFrameMcp"));
	Hello->SetNumberField(TEXT("port"), Port);
	WebFrameMcp::SendJson(Peer, WebFrameMcp::JsonToString(Hello));

	const FString Line = FString::Printf(TEXT("[%s] External MCP client connected. clients=%d"),
		*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")),
		Clients.Num());
	AddConsoleLine(Line);
	WriteExternalCommandLog(Line);
}

void FWebFrameMcpModule::HandleClientMessage(INetworkingWebSocket* Peer, void* Data, int32 Size)
{
	if (!Data || Size <= 0)
	{
		return;
	}

	TArray<uint8> Buffer;
	Buffer.Append(static_cast<uint8*>(Data), Size);
	Buffer.Add(0);
	const FString Message(UTF8_TO_TCHAR(Buffer.GetData()));

	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Message), Root) || !Root.IsValid())
	{
		SendResponse(Peer, FString(), false, nullptr, TEXT("Invalid JSON message."));
		return;
	}

	FString Id;
	Root->TryGetStringField(TEXT("id"), Id);
	FString Method;
	if (!Root->TryGetStringField(TEXT("method"), Method))
	{
		Root->TryGetStringField(TEXT("op"), Method);
	}

	TSharedPtr<FJsonObject> Params;
	const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
	if (Root->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr)
	{
		Params = *ParamsPtr;
	}
	if (!Params.IsValid())
	{
		Params = MakeShared<FJsonObject>();
	}

	const FString LogLine = FString::Printf(TEXT("[%s] id=%s method=%s payload=%s"),
		*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")),
		*Id,
		*Method,
		*Message);
	AddConsoleLine(LogLine);
	WriteExternalCommandLog(LogLine);

	FString Error;
	TSharedPtr<FJsonObject> Result = DispatchCommand(Method, Params, Error);
	const bool bOk = Result.IsValid() && Error.IsEmpty();
	if (bOk)
	{
		RecordSuccessfulCommandIfNeeded(Method, Params, Result);
	}
	SendResponse(Peer, Id, bOk, Result, Error);
}

void FWebFrameMcpModule::SendResponse(INetworkingWebSocket* Peer, const FString& Id, bool bOk, const TSharedPtr<FJsonObject>& Result, const FString& Error)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	if (!Id.IsEmpty())
	{
		Response->SetStringField(TEXT("id"), Id);
	}
	Response->SetBoolField(TEXT("ok"), bOk);
	if (Result.IsValid())
	{
		Response->SetObjectField(TEXT("result"), Result);
	}
	if (!Error.IsEmpty())
	{
		Response->SetStringField(TEXT("error"), Error);
	}
	WebFrameMcp::SendJson(Peer, WebFrameMcp::JsonToString(Response));
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::DispatchCommand(const FString& Method, const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	if (Method == TEXT("editor.play"))
	{
		const FString Action = Params->GetStringField(TEXT("action"));
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		if (Action == TEXT("start"))
		{
			if (GEditor->PlayWorld) { OutError = TEXT("Play session already running"); return nullptr; }
			FRequestPlaySessionParams PlayParams;
			PlayParams.WorldType = EPlaySessionWorldType::PlayInEditor;
			GEditor->RequestPlaySession(PlayParams);
		}
		else if (Action == TEXT("stop")) { GEditor->RequestEndPlayMap(); }
		Result->SetBoolField(TEXT("playing"), GEditor->PlayWorld != nullptr);
		return Result;
	}
	if (Method == TEXT("world.play.actor"))
	{
		UWorld* PlayWorld = GEditor->PlayWorld;
		if (!PlayWorld) { OutError = TEXT("No play world"); return nullptr; }
		FString Name; Params->TryGetStringField(TEXT("name"), Name);
		FString ClassPath; Params->TryGetStringField(TEXT("class"), ClassPath);
		UClass* ActorClass = ClassPath.IsEmpty() ? nullptr : LoadObject<UClass>(nullptr, *ClassPath);
		AActor* Actor = nullptr;
		for (TActorIterator<AActor> It(PlayWorld); It; ++It)
		{ if ((!Name.IsEmpty() && (It->GetName() == Name || It->GetActorLabel() == Name)) || (ActorClass && It->IsA(ActorClass))) { Actor = *It; break; } }
		if (!Actor) { OutError = TEXT("Runtime actor not found"); return nullptr; }
		UObject* Object = Actor;
		FString ComponentName;
		if (Params->TryGetStringField(TEXT("component"), ComponentName))
		{
			TInlineComponentArray<UActorComponent*> Components(Actor);
			Object = nullptr;
			for (UActorComponent* C : Components) if (C->GetName() == ComponentName) { Object = C; break; }
			if (!Object) { OutError = TEXT("Component not found"); return nullptr; }
		}
		FString Function;
		if (Params->TryGetStringField(TEXT("invoke"), Function))
		{
			UFunction* Fn = Object->FindFunction(FName(*Function));
			if (!Fn || Fn->ParmsSize) { OutError = TEXT("Function missing or requires parameters"); return nullptr; }
			Object->ProcessEvent(Fn, nullptr);
		}
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("actor"), Actor->GetPathName());
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (Params->TryGetArrayField(TEXT("properties"), Properties))
		{
			for (const auto& V : *Properties)
			{
				const FString PropName = V->AsString();
				FProperty* Prop = Object->GetClass()->FindPropertyByName(FName(*PropName));
				if (!Prop) { OutError = FString::Printf(TEXT("Property missing: %s"), *PropName); return nullptr; }
				FString Export; Prop->ExportText_InContainer(0, Export, Object, Object, Object, PPF_None);
				Result->SetStringField(PropName, Export);
			}
		}
		return Result;
	}
	if (Method == TEXT("asset.property.get") || Method == TEXT("asset.property.set"))
	{
		FString AssetPath = Params->GetStringField(TEXT("asset"));
		UObject* Object = LoadObject<UObject>(nullptr, *AssetPath);
		if (!Object) { OutError = TEXT("Asset not found"); return nullptr; }
		FString PropertyName = Params->GetStringField(TEXT("property"));
		FProperty* Property = Object->GetClass()->FindPropertyByName(FName(*PropertyName));
		if (!Property) { OutError = TEXT("Property not found"); return nullptr; }
		FString Value;
		Property->ExportText_InContainer(0, Value, Object, Object, Object, PPF_None);
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("oldValue"), Value);
		if (Method == TEXT("asset.property.set"))
		{
			Object->Modify();
			const FString NewValue = Params->GetStringField(TEXT("value"));
			if (!Property->ImportText_InContainer(*NewValue, Object, Object, PPF_None))
			{ OutError = TEXT("Property import failed"); return nullptr; }
			Object->PostEditChange();
			Object->MarkPackageDirty();
			bool bSave = false; Params->TryGetBoolField(TEXT("save"), bSave);
			if (bSave && !UEditorLoadingAndSavingUtils::SavePackages({ Object->GetOutermost() }, false))
			{ OutError = TEXT("Asset save failed"); return nullptr; }
			Value.Reset(); Property->ExportText_InContainer(0, Value, Object, Object, Object, PPF_None);
		}
		Result->SetStringField(TEXT("value"), Value);
		Result->SetStringField(TEXT("class"), Object->GetClass()->GetPathName());
		return Result;
	}
	if (Method == TEXT("blueprint.node.annotate"))
	{
		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Params->GetStringField(TEXT("asset")));
		UEdGraphNode* Node = Blueprint ? WebFrameMcp::FindNodeByGuid(Blueprint, Params->GetStringField(TEXT("nodeId"))) : nullptr;
		if (!Node) { OutError = TEXT("Node not found"); return nullptr; }
		Node->Modify(); Node->NodeComment = Params->GetStringField(TEXT("text"));
		Node->bCommentBubbleVisible = true;
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		Node->GetGraph()->NotifyGraphChanged();
		return WebFrameMcp::NodeToJson(Node);
	}
	if (Method == TEXT("ping") || Method == TEXT("mcp.ping"))
	{
		TSharedPtr<FJsonObject> Pong = MakeShared<FJsonObject>();
		Pong->SetStringField(TEXT("message"), TEXT("pong"));
		return Pong;
	}
	if (Method == TEXT("mcp.status"))
	{
		return BuildStatusPayload();
	}
	if (Method == TEXT("world.actor.delete"))
	{
		return DeleteWorldActor(Params, OutError);
	}
	if (const FWebFrameMcpCommandSpec* Command = RegisteredCommands.Find(Method))
	{
		if (!IsCommandOwnerEnabled(Command->Owner))
		{
			OutError = FString::Printf(TEXT("MCP module is disabled: %s"), *Command->Owner.ToString());
			return nullptr;
		}
		if (!ValidatePathPermissionsForCommand(*Command, Params, OutError))
		{
			return nullptr;
		}
		if (Command->Handler)
		{
			return Command->Handler(Params, OutError);
		}
		OutError = FString::Printf(TEXT("MCP method has no handler: %s"), *Method);
		return nullptr;
	}
#if 0
	if (Method == TEXT("world.actor.list"))
	{
		return ListWorldActors(Params, OutError);
	}
	if (Method == TEXT("world.actor.describe"))
	{
		return DescribeWorldActor(Params, OutError);
	}
	if (Method == TEXT("world.actor.spawn"))
	{
		return SpawnWorldActor(Params, OutError);
	}
	if (Method == TEXT("world.actor.set_transform"))
	{
		return SetWorldActorTransform(Params, OutError);
	}
	if (Method == TEXT("world.actor.set_property"))
	{
		return SetWorldActorProperty(Params, OutError);
	}
	if (Method == TEXT("world.actor.get_bounds"))
	{
		return GetWorldActorBounds(Params, OutError);
	}
	if (Method == TEXT("world.actor.mesh_geometry") || Method == TEXT("world.actor.mesh_geometry.describe"))
	{
		return DescribeWorldActorMeshGeometry(Params, OutError);
	}
	if (Method == TEXT("world.postprocess.add_material"))
	{
		return AddPostProcessMaterial(Params, OutError);
	}
	if (Method == TEXT("world.postprocess.spawn_volume"))
	{
		return SpawnPostProcessVolume(Params, OutError);
	}
	if (Method == TEXT("asset.rename"))
	{
		return RenameAsset(Params, OutError);
	}
	if (Method == TEXT("blueprint.list"))
	{
		return ListAssets(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("Blueprint")), Params, OutError);
	}
	if (Method == TEXT("blueprint.create"))
	{
		return CreateBlueprintAsset(Params, OutError);
	}
	if (Method == TEXT("blueprint.can_create_child"))
	{
		return CanCreateBlueprintChild(Params, OutError);
	}
	if (Method == TEXT("blueprint.describe"))
	{
		FString AssetPath;
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		return DescribeBlueprintAsset(AssetPath, OutError);
	}
	if (Method == TEXT("blueprint.variables"))
	{
		return ListBlueprintVariables(Params, OutError);
	}
	if (Method == TEXT("blueprint.variable.add"))
	{
		return AddBlueprintVariable(Params, OutError);
	}
	if (Method == TEXT("blueprint.set_variable_default"))
	{
		return SetBlueprintVariableDefault(Params, OutError);
	}
	if (Method == TEXT("blueprint.set_class_default"))
	{
		return SetBlueprintClassDefault(Params, OutError);
	}
	if (Method == TEXT("blueprint.set_component_default"))
	{
		return SetBlueprintComponentDefault(Params, OutError);
	}
	if (Method == TEXT("blueprint.set_pin_default"))
	{
		return SetBlueprintPinDefault(Params, OutError);
	}
	if (Method == TEXT("blueprint.graph.add_function"))
	{
		return AddBlueprintFunctionGraph(Params, OutError);
	}
	if (Method == TEXT("blueprint.graph.add_macro"))
	{
		return AddBlueprintMacroGraph(Params, OutError);
	}
	if (Method == TEXT("blueprint.node.call_function"))
	{
		return CreateBlueprintCallFunctionNode(Params, OutError);
	}
	if (Method == TEXT("blueprint.node.custom_event"))
	{
		return CreateBlueprintCustomEventNode(Params, OutError);
	}
	if (Method == TEXT("blueprint.node.comment"))
	{
		return CreateBlueprintCommentNode(Params, OutError);
	}
	if (Method == TEXT("blueprint.node.variable_get"))
	{
		return CreateBlueprintVariableNode(Params, false, OutError);
	}
	if (Method == TEXT("blueprint.node.variable_set"))
	{
		return CreateBlueprintVariableNode(Params, true, OutError);
	}
	if (Method == TEXT("blueprint.node.connect"))
	{
		return ConnectBlueprintNodePins(Params, OutError);
	}
	if (Method == TEXT("blueprint.compile"))
	{
		FString AssetPath;
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		return CompileBlueprintAsset(AssetPath, OutError);
	}
	if (Method == TEXT("blueprint.record_change"))
	{
		FString AssetPath;
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		if (AssetPath.IsEmpty())
		{
			OutError = TEXT("Missing params.asset.");
			return nullptr;
		}

		FString Summary;
		Params->TryGetStringField(TEXT("summary"), Summary);
		TArray<TPair<FString, FString>> Details;
		const TArray<TSharedPtr<FJsonValue>>* DetailValues = nullptr;
		if (Params->TryGetArrayField(TEXT("details"), DetailValues) && DetailValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *DetailValues)
			{
				const TSharedPtr<FJsonObject>* DetailObject = nullptr;
				if (Value.IsValid() && Value->TryGetObject(DetailObject) && DetailObject && DetailObject->IsValid())
				{
					FString Object;
					FString Content;
					(*DetailObject)->TryGetStringField(TEXT("object"), Object);
					(*DetailObject)->TryGetStringField(TEXT("content"), Content);
					if (!Object.IsEmpty() || !Content.IsEmpty())
					{
						Details.Add(TPair<FString, FString>(Object, Content));
					}
				}
			}
		}
		RecordBlueprintChange(AssetPath, Summary, Details);
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset"), AssetPath);
		Result->SetStringField(TEXT("recorded"), TEXT("true"));
		return Result;
	}
	if (Method == TEXT("material.list"))
	{
		return ListAssets(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("Material")), Params, OutError);
	}
	if (Method == TEXT("material.create"))
	{
		return CreateMaterialAsset(Params, OutError);
	}
	if (Method == TEXT("material.describe"))
	{
		FString AssetPath;
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		return DescribeMaterialAsset(AssetPath, OutError);
	}
	if (Method == TEXT("material.set_settings"))
	{
		return SetMaterialSettings(Params, OutError);
	}
	if (Method == TEXT("material.node.add"))
	{
		return AddMaterialExpressionNode(Params, OutError);
	}
	if (Method == TEXT("material.node.set_property"))
	{
		return SetMaterialExpressionProperty(Params, OutError);
	}
	if (Method == TEXT("material.node.connect"))
	{
		return ConnectMaterialExpressionNodes(Params, OutError);
	}
	if (Method == TEXT("material.connect_property"))
	{
		return ConnectMaterialExpressionToProperty(Params, OutError);
	}
	if (Method == TEXT("mesh.list"))
	{
		FString Type;
		Params->TryGetStringField(TEXT("type"), Type);
		if (Type.Equals(TEXT("static"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("staticmesh"), ESearchCase::IgnoreCase))
		{
			return ListAssets(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("StaticMesh")), Params, OutError);
		}
		if (Type.Equals(TEXT("skeletal"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("skeletalmesh"), ESearchCase::IgnoreCase))
		{
			return ListAssets(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("SkeletalMesh")), Params, OutError);
		}

		TSharedPtr<FJsonObject> StaticResult = ListAssets(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("StaticMesh")), Params, OutError);
		if (!StaticResult.IsValid())
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> SkeletalResult = ListAssets(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("SkeletalMesh")), Params, OutError);
		if (!SkeletalResult.IsValid())
		{
			return nullptr;
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
	}
	if (Method == TEXT("mesh.describe"))
	{
		return DescribeMeshAsset(Params, OutError);
	}
	if (Method == TEXT("mesh.geometry.describe"))
	{
		return DescribeMeshGeometry(Params, OutError);
	}
	if (Method == TEXT("mesh.set_material_slot"))
	{
		return SetMeshMaterialSlot(Params, OutError);
	}
	if (Method == TEXT("mesh.add_socket"))
	{
		return AddMeshSocket(Params, OutError);
	}
	if (Method == TEXT("mesh.update_socket"))
	{
		return UpdateMeshSocket(Params, OutError);
	}
	if (Method == TEXT("mesh.remove_socket"))
	{
		return RemoveMeshSocket(Params, OutError);
	}
#endif

	OutError = FString::Printf(TEXT("Unknown MCP method: %s"), *Method);
	return nullptr;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::BuildStatusPayload() const
{
	TSharedPtr<FJsonObject> Status = MakeShared<FJsonObject>();
	Status->SetStringField(TEXT("name"), TEXT("WebFrameMcp"));
	Status->SetNumberField(TEXT("port"), Port);
	Status->SetBoolField(TEXT("serverRunning"), bServerRunning);
	Status->SetNumberField(TEXT("clients"), Clients.Num());
	Status->SetStringField(TEXT("logDirectory"), FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("WebFrameMcp"), TEXT("Saved"), TEXT("Logs")));
	TArray<TSharedPtr<FJsonValue>> Commands;
	for (const TPair<FString, FWebFrameMcpCommandSpec>& Pair : RegisteredCommands)
	{
		TSharedPtr<FJsonObject> Command = MakeShared<FJsonObject>();
		Command->SetStringField(TEXT("method"), Pair.Key);
		Command->SetStringField(TEXT("owner"), Pair.Value.Owner.ToString());
		Command->SetBoolField(TEXT("ownerEnabled"), IsCommandOwnerEnabled(Pair.Value.Owner));
		Command->SetStringField(TEXT("description"), Pair.Value.Description);
		Commands.Add(MakeShared<FJsonValueObject>(Command));
	}
	Status->SetArrayField(TEXT("commands"), Commands);

	TArray<TSharedPtr<FJsonValue>> Modules;
	for (FName OwnerName : GetRegisteredCommandOwners())
	{
		TSharedPtr<FJsonObject> Module = MakeShared<FJsonObject>();
		Module->SetStringField(TEXT("owner"), OwnerName.ToString());
		Module->SetBoolField(TEXT("enabled"), IsCommandOwnerEnabled(OwnerName));
		Module->SetNumberField(TEXT("commandCount"), GetRegisteredCommandCountByOwner(OwnerName));
		Modules.Add(MakeShared<FJsonValueObject>(Module));
	}
	Status->SetArrayField(TEXT("modules"), Modules);
	return Status;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::ListWorldActors(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	UWorld* World = WebFrameMcp::GetEditorWorld(OutError);
	if (!World)
	{
		return nullptr;
	}

	bool bIncludeComponents = false;
	bool bIncludeBounds = true;
	bool bSelectedOnly = false;
	int32 Limit = 500;
	int32 Offset = 0;
	FString ClassContains;
	FString LabelContains;
	FString NameContains;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("includeComponents"), bIncludeComponents);
		Params->TryGetBoolField(TEXT("includeBounds"), bIncludeBounds);
		Params->TryGetBoolField(TEXT("selectedOnly"), bSelectedOnly);
		Params->TryGetNumberField(TEXT("limit"), Limit);
		Params->TryGetNumberField(TEXT("offset"), Offset);
		Params->TryGetStringField(TEXT("classContains"), ClassContains);
		Params->TryGetStringField(TEXT("labelContains"), LabelContains);
		Params->TryGetStringField(TEXT("nameContains"), NameContains);
	}
	Limit = FMath::Clamp(Limit, 1, 5000);
	Offset = FMath::Max(Offset, 0);

	TArray<TSharedPtr<FJsonValue>> Actors;
	int32 MatchedCount = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}
		if (bSelectedOnly && (!GEditor || !GEditor->GetSelectedActors() || !GEditor->GetSelectedActors()->IsSelected(Actor)))
		{
			continue;
		}
		if (!ClassContains.IsEmpty() && !Actor->GetClass()->GetPathName().Contains(ClassContains))
		{
			continue;
		}
		if (!LabelContains.IsEmpty() && !Actor->GetActorLabel().Contains(LabelContains))
		{
			continue;
		}
		if (!NameContains.IsEmpty() && !Actor->GetName().Contains(NameContains))
		{
			continue;
		}

		if (MatchedCount++ < Offset)
		{
			continue;
		}
		if (Actors.Num() >= Limit)
		{
			continue;
		}
		Actors.Add(MakeShared<FJsonValueObject>(WebFrameMcp::ActorToJson(Actor, bIncludeComponents, bIncludeBounds)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("world"), World->GetPathName());
	Result->SetArrayField(TEXT("actors"), Actors);
	Result->SetNumberField(TEXT("count"), Actors.Num());
	Result->SetNumberField(TEXT("total"), MatchedCount);
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetNumberField(TEXT("offset"), Offset);
	Result->SetBoolField(TEXT("hasMore"), MatchedCount > Offset + Actors.Num());
	Result->SetStringField(TEXT("scope"), TEXT("loaded editor-world actors only"));
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::DescribeWorldActor(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	AActor* Actor = WebFrameMcp::FindEditorActor(Params, OutError);
	if (!Actor)
	{
		return nullptr;
	}

	bool bIncludeComponents = true;
	bool bIncludeBounds = true;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("includeComponents"), bIncludeComponents);
		Params->TryGetBoolField(TEXT("includeBounds"), bIncludeBounds);
	}

	return WebFrameMcp::ActorToJson(Actor, bIncludeComponents, bIncludeBounds);
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::SpawnWorldActor(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	UWorld* World = WebFrameMcp::GetEditorWorld(OutError);
	if (!World)
	{
		return nullptr;
	}

	FString ClassPath;
	FString Label;
	bool bSelect = true;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("class"), ClassPath);
		Params->TryGetStringField(TEXT("classPath"), ClassPath);
		Params->TryGetStringField(TEXT("label"), Label);
		Params->TryGetBoolField(TEXT("select"), bSelect);
	}
	if (ClassPath.IsEmpty())
	{
		OutError = TEXT("Missing params.class.");
		return nullptr;
	}

	UClass* ActorClass = LoadClass<AActor>(nullptr, *ClassPath);
	if (!ActorClass)
	{
		if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ClassPath))
		{
			ActorClass = Blueprint->GeneratedClass;
		}
	}
	if (!ActorClass || !ActorClass->IsChildOf(AActor::StaticClass()))
	{
		OutError = FString::Printf(TEXT("Actor class not found or invalid: %s"), *ClassPath);
		return nullptr;
	}

	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	FVector Scale = FVector::OneVector;
	WebFrameMcp::TryReadVectorArray(Params, TEXT("location"), Location);
	WebFrameMcp::TryReadRotatorArray(Params, TEXT("rotation"), Rotation);
	WebFrameMcp::TryReadVectorArray(Params, TEXT("scale"), Scale);
	const FTransform SpawnTransform(Rotation, Location, Scale);

	const FScopedTransaction Transaction(LOCTEXT("WebFrameMcpSpawnActor", "WebFrame MCP Spawn Actor"));
	AActor* Actor = GEditor->AddActor(World->GetCurrentLevel(), ActorClass, SpawnTransform, false, RF_Transactional, bSelect);
	if (!Actor)
	{
		OutError = FString::Printf(TEXT("Failed to spawn actor class: %s"), *ClassPath);
		return nullptr;
	}
	Actor->Modify();
	if (!Label.IsEmpty())
	{
		Actor->SetActorLabel(Label);
	}
	Actor->SetActorTransform(SpawnTransform, false, nullptr, ETeleportType::TeleportPhysics);
	Actor->MarkPackageDirty();
	if (bSelect && GEditor)
	{
		GEditor->SelectNone(false, true, false);
		GEditor->SelectActor(Actor, true, true);
	}
	if (GEditor)
	{
		GEditor->RedrawLevelEditingViewports();
	}

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(Actor->GetActorLabel(), FString::Printf(TEXT("Spawn actor class %s."), *ClassPath)));
	RecordBlueprintChange(Actor->GetPathName(), TEXT("Spawn world actor"), Details);

	return WebFrameMcp::ActorToJson(Actor, true, true);
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::SetWorldActorTransform(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	AActor* Actor = WebFrameMcp::FindEditorActor(Params, OutError);
	if (!Actor)
	{
		return nullptr;
	}

	FTransform NewTransform = Actor->GetActorTransform();
	FVector VectorValue;
	FRotator RotatorValue;
	bool bTouched = false;
	if (WebFrameMcp::TryReadVectorArray(Params, TEXT("location"), VectorValue))
	{
		NewTransform.SetLocation(VectorValue);
		bTouched = true;
	}
	if (WebFrameMcp::TryReadRotatorArray(Params, TEXT("rotation"), RotatorValue))
	{
		NewTransform.SetRotation(RotatorValue.Quaternion());
		bTouched = true;
	}
	if (WebFrameMcp::TryReadVectorArray(Params, TEXT("scale"), VectorValue))
	{
		NewTransform.SetScale3D(VectorValue);
		bTouched = true;
	}
	if (!bTouched)
	{
		OutError = TEXT("Missing transform fields. Provide params.location, params.rotation, or params.scale.");
		return nullptr;
	}

	const FTransform OldTransform = Actor->GetActorTransform();
	const FScopedTransaction Transaction(LOCTEXT("WebFrameMcpSetActorTransform", "WebFrame MCP Set Actor Transform"));
	Actor->Modify();
	if (USceneComponent* RootComponent = Actor->GetRootComponent())
	{
		RootComponent->Modify();
	}
	Actor->SetActorTransform(NewTransform, false, nullptr, ETeleportType::TeleportPhysics);
	Actor->PostEditMove(true);
	Actor->MarkPackageDirty();
	if (GEditor)
	{
		GEditor->RedrawLevelEditingViewports();
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), Actor->GetPathName());
	Result->SetObjectField(TEXT("oldTransform"), WebFrameMcp::TransformToJson(OldTransform));
	Result->SetObjectField(TEXT("transform"), WebFrameMcp::TransformToJson(Actor->GetActorTransform()));
	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(Actor->GetActorLabel(), TEXT("Set actor transform.")));
	RecordBlueprintChange(Actor->GetPathName(), TEXT("Set world actor transform"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::DeleteWorldActor(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	AActor* Actor = WebFrameMcp::FindEditorActor(Params, OutError);
	if (!Actor)
	{
		return nullptr;
	}

	if (Actor->IsA<AWorldSettings>())
	{
		OutError = TEXT("Refusing to delete WorldSettings.");
		return nullptr;
	}

	const FString ActorPath = Actor->GetPathName();
	const FString ActorLabel = Actor->GetActorLabel();
	const FString ActorClass = Actor->GetClass()->GetPathName();

	if (!GEditor)
	{
		OutError = TEXT("GEditor is unavailable.");
		return nullptr;
	}

	UEditorActorSubsystem* EditorActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
	if (!EditorActorSubsystem)
	{
		OutError = TEXT("EditorActorSubsystem is unavailable.");
		return nullptr;
	}

	const FScopedTransaction Transaction(LOCTEXT("WebFrameMcpDeleteActor", "WebFrame MCP Delete Actor"));
	const bool bDestroyed = EditorActorSubsystem->DestroyActor(Actor);
	if (!bDestroyed)
	{
		OutError = FString::Printf(TEXT("Failed to delete actor: %s"), *ActorPath);
		return nullptr;
	}

	GEditor->RedrawLevelEditingViewports();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), ActorPath);
	Result->SetStringField(TEXT("label"), ActorLabel);
	Result->SetStringField(TEXT("class"), ActorClass);
	Result->SetBoolField(TEXT("deleted"), true);

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(ActorLabel, FString::Printf(TEXT("Deleted actor %s."), *ActorClass)));
	RecordBlueprintChange(ActorPath, TEXT("Delete world actor"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::SetWorldActorProperty(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	AActor* Actor = WebFrameMcp::FindEditorActor(Params, OutError);
	if (!Actor)
	{
		return nullptr;
	}

	FString PropertyName;
	FString Value;
	FString ComponentRef;
	bool bRerunConstructionScripts = false;
	bool bHasValue = false;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("property"), PropertyName);
		if (TSharedPtr<FJsonValue> ValueField = Params->TryGetField(TEXT("value")))
		{
			bHasValue = WebFrameMcp::JsonValueToString(ValueField, Value);
		}
		Params->TryGetStringField(TEXT("component"), ComponentRef);
		if (ComponentRef.IsEmpty())
		{
			Params->TryGetStringField(TEXT("componentName"), ComponentRef);
		}
		Params->TryGetBoolField(TEXT("rerunConstructionScripts"), bRerunConstructionScripts);
	}
	if (PropertyName.IsEmpty())
	{
		OutError = TEXT("Missing params.property.");
		return nullptr;
	}
	if (PropertyName == TEXT("__delete__"))
	{
		return DeleteWorldActor(Params, OutError);
	}
	if (!bHasValue)
	{
		OutError = TEXT("Missing params.value.");
		return nullptr;
	}

	UObject* TargetObject = Actor;
	FString TargetKind = TEXT("actor");
	if (!ComponentRef.IsEmpty())
	{
		TargetObject = WebFrameMcp::FindActorComponent(Actor, ComponentRef);
		if (!TargetObject)
		{
			OutError = FString::Printf(TEXT("Actor component not found: %s"), *ComponentRef);
			return nullptr;
		}
		TargetKind = TEXT("component");
	}

	FString OldValue;
	const FScopedTransaction Transaction(LOCTEXT("WebFrameMcpSetActorProperty", "WebFrame MCP Set Actor Property"));
	if (!WebFrameMcp::SetObjectPropertyFromString(TargetObject, PropertyName, Value, OldValue, OutError))
	{
		return nullptr;
	}
	TargetObject->PostEditChange();
	if (UActorComponent* Component = Cast<UActorComponent>(TargetObject))
	{
		Component->MarkPackageDirty();
		if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
		{
			SceneComponent->UpdateComponentToWorld();
		}
		if (UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(Component))
		{
			PrimitiveComponent->MarkRenderStateDirty();
		}
	}
	Actor->MarkPackageDirty();
	if (bRerunConstructionScripts)
	{
		Actor->RerunConstructionScripts();
	}
	if (GEditor)
	{
		GEditor->RedrawLevelEditingViewports();
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), Actor->GetPathName());
	Result->SetStringField(TEXT("targetKind"), TargetKind);
	Result->SetStringField(TEXT("target"), TargetObject->GetPathName());
	Result->SetStringField(TEXT("property"), PropertyName);
	Result->SetStringField(TEXT("oldValue"), OldValue);
	Result->SetStringField(TEXT("value"), Value);
	Result->SetBoolField(TEXT("rerunConstructionScripts"), bRerunConstructionScripts);
	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(
		TargetObject->GetName(),
		FString::Printf(TEXT("Set property %s: %s -> %s."), *PropertyName, *OldValue, *Value)));
	RecordBlueprintChange(Actor->GetPathName(), TEXT("Set world actor property"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::SaveCurrentWorldLevel(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	UWorld* World = WebFrameMcp::GetEditorWorld(OutError);
	if (!World)
	{
		return nullptr;
	}

	const FString WorldPath = World->GetPathName();
	const bool bSaved = FEditorFileUtils::SaveCurrentLevel();
	if (!bSaved)
	{
		OutError = FString::Printf(TEXT("Failed to save current level: %s"), *WorldPath);
		return nullptr;
	}

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(World->GetName(), TEXT("Saved current editor level.")));
	RecordBlueprintChange(WorldPath, TEXT("Save current level"), Details);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("world"), WorldPath);
	Result->SetBoolField(TEXT("saved"), true);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::GetWorldActorBounds(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	AActor* Actor = WebFrameMcp::FindEditorActor(Params, OutError);
	if (!Actor)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), Actor->GetPathName());
	Result->SetObjectField(TEXT("bounds"), WebFrameMcp::ActorBoundsToJson(Actor));
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::DescribeWorldActorMeshGeometry(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	AActor* Actor = WebFrameMcp::FindEditorActor(Params, OutError);
	if (!Actor)
	{
		return nullptr;
	}

	int32 LodIndex = 0;
	bool bIncludeVertices = false;
	bool bIncludeTriangles = false;
	int32 VertexLimit = 0;
	int32 TriangleLimit = 0;
	FString ComponentRef;
	FString ComponentContains;
	FVector RadialOrigin = FVector::ZeroVector;
	if (Params.IsValid())
	{
		Params->TryGetNumberField(TEXT("lodIndex"), LodIndex);
		Params->TryGetBoolField(TEXT("includeVertices"), bIncludeVertices);
		Params->TryGetBoolField(TEXT("includeTriangles"), bIncludeTriangles);
		Params->TryGetNumberField(TEXT("vertexLimit"), VertexLimit);
		Params->TryGetNumberField(TEXT("triangleLimit"), TriangleLimit);
		Params->TryGetStringField(TEXT("component"), ComponentRef);
		Params->TryGetStringField(TEXT("componentContains"), ComponentContains);
		WebFrameMcp::TryReadVectorArray(Params, TEXT("origin"), RadialOrigin);
		WebFrameMcp::TryReadVectorArray(Params, TEXT("radialOrigin"), RadialOrigin);
	}
	LodIndex = FMath::Max(0, LodIndex);
	VertexLimit = FMath::Clamp(VertexLimit, 0, 100000);
	TriangleLimit = FMath::Clamp(TriangleLimit, 0, 100000);

	TArray<UStaticMeshComponent*> StaticMeshComponents;
	Actor->GetComponents<UStaticMeshComponent>(StaticMeshComponents);

	TArray<TSharedPtr<FJsonValue>> ComponentResults;
	TArray<TSharedPtr<FJsonValue>> VertexSamples;
	TArray<TSharedPtr<FJsonValue>> TriangleSamples;
	WebFrameMcp::FGeometryRadialBounds AggregateRadialBounds;
	int32 MatchedComponentCount = 0;
	int64 TotalVertexCount = 0;
	int64 TotalTriangleCount = 0;

	for (UStaticMeshComponent* Component : StaticMeshComponents)
	{
		if (!Component)
		{
			continue;
		}
		if (!ComponentRef.IsEmpty() && Component->GetName() != ComponentRef && Component->GetPathName() != ComponentRef)
		{
			continue;
		}
		if (!ComponentContains.IsEmpty() && !Component->GetName().Contains(ComponentContains) && !Component->GetPathName().Contains(ComponentContains))
		{
			continue;
		}

		UStaticMesh* StaticMesh = Component->GetStaticMesh();
		const FStaticMeshLODResources* LodResource = nullptr;
		FString LodError;
		if (!WebFrameMcp::ReadStaticMeshLod(StaticMesh, LodIndex, LodResource, LodError))
		{
			continue;
		}

		++MatchedComponentCount;
		const int32 VertexCount = LodResource->GetNumVertices();
		const FIndexArrayView Indices = LodResource->IndexBuffer.GetArrayView();
		const int32 TriangleCount = Indices.Num() / 3;
		TotalVertexCount += VertexCount;
		TotalTriangleCount += TriangleCount;

		WebFrameMcp::FGeometryRadialBounds ComponentRadialBounds;
		WebFrameMcp::AddGeometrySamples(
			*LodResource,
			Component->GetComponentTransform(),
			RadialOrigin,
			bIncludeVertices,
			VertexLimit,
			bIncludeTriangles,
			TriangleLimit,
			Component->GetName(),
			ComponentRadialBounds,
			bIncludeVertices ? &VertexSamples : nullptr,
			bIncludeTriangles ? &TriangleSamples : nullptr);

		if (ComponentRadialBounds.bValid)
		{
			AggregateRadialBounds.AddPosition(ComponentRadialBounds.MinPosition, RadialOrigin, ComponentRadialBounds.MinVertexIndex, Component->GetName());
			AggregateRadialBounds.AddPosition(ComponentRadialBounds.MaxPosition, RadialOrigin, ComponentRadialBounds.MaxVertexIndex, Component->GetName());
		}

		TSharedPtr<FJsonObject> ComponentJson = MakeShared<FJsonObject>();
		ComponentJson->SetStringField(TEXT("name"), Component->GetName());
		ComponentJson->SetStringField(TEXT("pathName"), Component->GetPathName());
		ComponentJson->SetStringField(TEXT("mesh"), StaticMesh ? StaticMesh->GetPathName() : FString());
		ComponentJson->SetObjectField(TEXT("worldTransform"), WebFrameMcp::TransformToJson(Component->GetComponentTransform()));
		ComponentJson->SetObjectField(TEXT("bounds"), WebFrameMcp::BoundsToJson(Component->Bounds));
		ComponentJson->SetNumberField(TEXT("vertexCount"), VertexCount);
		ComponentJson->SetNumberField(TEXT("triangleCount"), TriangleCount);
		ComponentJson->SetObjectField(TEXT("radialBounds"), WebFrameMcp::RadialBoundsToJson(ComponentRadialBounds, RadialOrigin));
		ComponentResults.Add(MakeShared<FJsonValueObject>(ComponentJson));
	}

	if (MatchedComponentCount == 0)
	{
		OutError = FString::Printf(TEXT("No StaticMeshComponent geometry found for actor: %s"), *Actor->GetActorLabel());
		return nullptr;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), Actor->GetPathName());
	Result->SetStringField(TEXT("label"), Actor->GetActorLabel());
	Result->SetNumberField(TEXT("lodIndex"), LodIndex);
	Result->SetNumberField(TEXT("componentCount"), MatchedComponentCount);
	Result->SetNumberField(TEXT("vertexCount"), static_cast<double>(TotalVertexCount));
	Result->SetNumberField(TEXT("triangleCount"), static_cast<double>(TotalTriangleCount));
	Result->SetObjectField(TEXT("radialBounds"), WebFrameMcp::RadialBoundsToJson(AggregateRadialBounds, RadialOrigin));
	Result->SetArrayField(TEXT("components"), ComponentResults);
	Result->SetStringField(TEXT("space"), TEXT("world"));
	Result->SetStringField(TEXT("scope"), TEXT("loaded editor-world StaticMeshComponents only"));
	if (bIncludeVertices)
	{
		Result->SetArrayField(TEXT("vertices"), VertexSamples);
		Result->SetNumberField(TEXT("vertexSampleCount"), VertexSamples.Num());
		Result->SetNumberField(TEXT("vertexLimit"), VertexLimit);
	}
	if (bIncludeTriangles)
	{
		Result->SetArrayField(TEXT("triangles"), TriangleSamples);
		Result->SetNumberField(TEXT("triangleSampleCount"), TriangleSamples.Num());
		Result->SetNumberField(TEXT("triangleLimit"), TriangleLimit);
	}
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::AddPostProcessMaterial(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	AActor* Actor = WebFrameMcp::FindEditorActor(Params, OutError);
	if (!Actor)
	{
		return nullptr;
	}

	APostProcessVolume* Volume = Cast<APostProcessVolume>(Actor);
	if (!Volume)
	{
		OutError = FString::Printf(TEXT("Actor is not a PostProcessVolume: %s"), *Actor->GetPathName());
		return nullptr;
	}

	FString MaterialPath;
	float Weight = 1.0f;
	bool bReplaceExisting = true;
	bool bUnbound = Volume->bUnbound != 0;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("material"), MaterialPath);
		Params->TryGetNumberField(TEXT("weight"), Weight);
		Params->TryGetBoolField(TEXT("replaceExisting"), bReplaceExisting);
		Params->TryGetBoolField(TEXT("unbound"), bUnbound);
	}
	if (MaterialPath.IsEmpty())
	{
		OutError = TEXT("Missing params.material.");
		return nullptr;
	}

	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!Material)
	{
		OutError = FString::Printf(TEXT("Post process material not found: %s"), *MaterialPath);
		return nullptr;
	}

	const FScopedTransaction Transaction(LOCTEXT("WebFrameMcpAddPostProcessMaterial", "WebFrame MCP Add Post Process Material"));
	Volume->Modify();
	Volume->bUnbound = bUnbound ? 1 : 0;

	bool bUpdatedExisting = false;
	for (FWeightedBlendable& Blendable : Volume->Settings.WeightedBlendables.Array)
	{
		if (Blendable.Object == Material)
		{
			if (bReplaceExisting)
			{
				Blendable.Weight = Weight;
			}
			bUpdatedExisting = true;
			break;
		}
	}
	if (!bUpdatedExisting)
	{
		Volume->Settings.WeightedBlendables.Array.Emplace(Weight, Material);
	}

	Volume->PostEditChange();
	Volume->MarkPackageDirty();
	if (GEditor)
	{
		GEditor->RedrawLevelEditingViewports();
	}

	TSharedPtr<FJsonObject> Result = WebFrameMcp::ActorToJson(Volume, true, true);
	Result->SetStringField(TEXT("material"), Material->GetPathName());
	Result->SetNumberField(TEXT("weight"), Weight);
	Result->SetBoolField(TEXT("updatedExisting"), bUpdatedExisting);
	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(Volume->GetActorLabel(), FString::Printf(TEXT("Add post process material %s."), *Material->GetPathName())));
	RecordBlueprintChange(Volume->GetPathName(), TEXT("Add post process material"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::SpawnPostProcessVolume(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	UWorld* World = WebFrameMcp::GetEditorWorld(OutError);
	if (!World)
	{
		return nullptr;
	}

	FString Label = TEXT("MCP_PostProcessVolume");
	FString MaterialPath;
	float Weight = 1.0f;
	bool bUnbound = true;
	bool bSelect = true;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("label"), Label);
		Params->TryGetStringField(TEXT("material"), MaterialPath);
		Params->TryGetNumberField(TEXT("weight"), Weight);
		Params->TryGetBoolField(TEXT("unbound"), bUnbound);
		Params->TryGetBoolField(TEXT("select"), bSelect);
	}

	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	FVector Scale = FVector::OneVector;
	WebFrameMcp::TryReadVectorArray(Params, TEXT("location"), Location);
	WebFrameMcp::TryReadRotatorArray(Params, TEXT("rotation"), Rotation);
	WebFrameMcp::TryReadVectorArray(Params, TEXT("scale"), Scale);
	const FTransform SpawnTransform(Rotation, Location, Scale);

	const FScopedTransaction Transaction(LOCTEXT("WebFrameMcpSpawnPostProcessVolume", "WebFrame MCP Spawn Post Process Volume"));
	APostProcessVolume* Volume = Cast<APostProcessVolume>(GEditor->AddActor(World->GetCurrentLevel(), APostProcessVolume::StaticClass(), SpawnTransform, false, RF_Transactional, bSelect));
	if (!Volume)
	{
		OutError = TEXT("Failed to spawn PostProcessVolume.");
		return nullptr;
	}

	Volume->Modify();
	Volume->SetActorLabel(Label);
	Volume->SetActorTransform(SpawnTransform, false, nullptr, ETeleportType::TeleportPhysics);
	Volume->bUnbound = bUnbound ? 1 : 0;

	UMaterialInterface* Material = nullptr;
	if (!MaterialPath.IsEmpty())
	{
		Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
		if (!Material)
		{
			OutError = FString::Printf(TEXT("Post process material not found: %s"), *MaterialPath);
			return nullptr;
		}
		Volume->Settings.WeightedBlendables.Array.Emplace(Weight, Material);
	}

	Volume->PostEditChange();
	Volume->MarkPackageDirty();
	if (bSelect && GEditor)
	{
		GEditor->SelectNone(false, true, false);
		GEditor->SelectActor(Volume, true, true);
	}
	if (GEditor)
	{
		GEditor->RedrawLevelEditingViewports();
	}

	TSharedPtr<FJsonObject> Result = WebFrameMcp::ActorToJson(Volume, true, true);
	Result->SetBoolField(TEXT("unbound"), Volume->bUnbound != 0);
	if (Material)
	{
		Result->SetStringField(TEXT("material"), Material->GetPathName());
		Result->SetNumberField(TEXT("weight"), Weight);
	}
	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(Volume->GetActorLabel(), Material ? FString::Printf(TEXT("Spawn unbound=%s with material %s."), Volume->bUnbound ? TEXT("true") : TEXT("false"), *Material->GetPathName()) : FString::Printf(TEXT("Spawn unbound=%s."), Volume->bUnbound ? TEXT("true") : TEXT("false"))));
	RecordBlueprintChange(Volume->GetPathName(), TEXT("Spawn post process volume"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::RenameAsset(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString NewName;
	FString NewPath;
	bool bSave = true;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("newName"), NewName);
		Params->TryGetStringField(TEXT("newPath"), NewPath);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}
	if (NewName.IsEmpty())
	{
		OutError = TEXT("Missing params.newName.");
		return nullptr;
	}

	UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
	if (!Asset)
	{
		OutError = FString::Printf(TEXT("Asset not found: %s"), *AssetPath);
		return nullptr;
	}

	if (NewPath.IsEmpty())
	{
		NewPath = FPackageName::GetLongPackagePath(Asset->GetOutermost()->GetName());
	}
	NewPath = WebFrameMcp::NormalizeGamePath(NewPath);
	if (!WebFrameMcp::IsAllowedGamePath(NewPath))
	{
		OutError = FString::Printf(TEXT("Asset rename path must be under /Game. Got: %s"), *NewPath);
		return nullptr;
	}

	const FString OldPath = Asset->GetPathName();
	const FString NewObjectPath = NewPath / NewName + TEXT(".") + NewName;
	if (LoadObject<UObject>(nullptr, *NewObjectPath))
	{
		OutError = FString::Printf(TEXT("Target asset already exists: %s"), *NewObjectPath);
		return nullptr;
	}

	TArray<FAssetRenameData> RenameData;
	RenameData.Emplace(Asset, NewPath, NewName);
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	if (!AssetToolsModule.Get().RenameAssets(RenameData))
	{
		OutError = FString::Printf(TEXT("Failed to rename asset: %s -> %s"), *OldPath, *NewObjectPath);
		return nullptr;
	}

	UObject* RenamedAsset = LoadObject<UObject>(nullptr, *NewObjectPath);
	if (!RenamedAsset)
	{
		OutError = FString::Printf(TEXT("Rename succeeded but renamed asset was not found: %s"), *NewObjectPath);
		return nullptr;
	}

	if (bSave)
	{
		UEditorLoadingAndSavingUtils::SavePackages({ RenamedAsset->GetOutermost() }, false);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("oldAsset"), OldPath);
	Result->SetStringField(TEXT("asset"), RenamedAsset->GetPathName());
	Result->SetStringField(TEXT("package"), RenamedAsset->GetOutermost()->GetName());
	Result->SetBoolField(TEXT("saved"), bSave);
	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(WebFrameMcp::AssetNameFromPath(OldPath), FString::Printf(TEXT("Rename to %s."), *RenamedAsset->GetPathName())));
	RecordBlueprintChange(RenamedAsset->GetPathName(), TEXT("Rename asset"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::ListAssets(const FTopLevelAssetPath& ClassPathName, const TSharedPtr<FJsonObject>& Params, FString& OutError) const
{
	FString Path = TEXT("/Game");
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("path"), Path);
	}
	Path = WebFrameMcp::NormalizeGamePath(Path);
	if (!WebFrameMcp::IsAllowedGamePath(Path))
	{
		OutError = FString::Printf(TEXT("Asset list path must be under /Game. Got: %s"), *Path);
		return nullptr;
	}

	bool bRecursive = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("recursive"), bRecursive);
	}

	int32 Limit = 100;
	int32 Offset = 0;
	if (Params.IsValid())
	{
		Params->TryGetNumberField(TEXT("limit"), Limit);
		Params->TryGetNumberField(TEXT("offset"), Offset);
	}
	Limit = FMath::Clamp(Limit, 1, 1000);
	Offset = FMath::Max(Offset, 0);

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	FARFilter Filter;
	Filter.ClassPaths.Add(ClassPathName);
	Filter.bRecursiveClasses = true;
	Filter.PackagePaths.Add(*Path);
	Filter.bRecursivePaths = bRecursive;

	TArray<FAssetData> Assets;
	AssetRegistryModule.Get().GetAssets(Filter, Assets);
	Assets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString();
	});

	TArray<TSharedPtr<FJsonValue>> Items;
	for (int32 Index = Offset; Index < Assets.Num() && Items.Num() < Limit; ++Index)
	{
		const FAssetData& Asset = Assets[Index];
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Item->SetStringField(TEXT("path"), Asset.GetSoftObjectPath().ToString());
		Item->SetStringField(TEXT("class"), Asset.AssetClassPath.ToString());
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("assets"), Items);
	Result->SetStringField(TEXT("path"), Path);
	Result->SetBoolField(TEXT("recursive"), bRecursive);
	Result->SetNumberField(TEXT("count"), Items.Num());
	Result->SetNumberField(TEXT("total"), Assets.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetNumberField(TEXT("offset"), Offset);
	Result->SetBoolField(TEXT("hasMore"), Offset + Items.Num() < Assets.Num());
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::CanCreateBlueprintChild(const TSharedPtr<FJsonObject>& Params, FString& OutError) const
{
	FString ParentClassPath;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("parentClass"), ParentClassPath);
	}
	if (ParentClassPath.IsEmpty())
	{
		OutError = TEXT("Missing params.parentClass.");
		return nullptr;
	}

	UClass* ParentClass = LoadObject<UClass>(nullptr, *ParentClassPath);
	if (!ParentClass)
	{
		OutError = FString::Printf(TEXT("Parent class not found: %s"), *ParentClassPath);
		return nullptr;
	}

	const bool bCanCreate = FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("parentClass"), ParentClass->GetPathName());
	Result->SetStringField(TEXT("className"), ParentClass->GetName());
	Result->SetBoolField(TEXT("canCreate"), bCanCreate);
	Result->SetBoolField(TEXT("isNative"), ParentClass->HasAnyClassFlags(CLASS_Native));
	Result->SetBoolField(TEXT("isBlueprintGenerated"), ParentClass->ClassGeneratedBy != nullptr);
	Result->SetBoolField(TEXT("isAbstract"), ParentClass->HasAnyClassFlags(CLASS_Abstract));
	Result->SetBoolField(TEXT("isDeprecated"), ParentClass->HasAnyClassFlags(CLASS_Deprecated));
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::CreateBlueprintAsset(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString Name;
	Params->TryGetStringField(TEXT("name"), Name);
	if (Name.IsEmpty())
	{
		Name = TEXT("BP_MCPPawn");
	}

	FString PackagePath;
	Params->TryGetStringField(TEXT("path"), PackagePath);
	if (PackagePath.IsEmpty())
	{
		PackagePath = TEXT("/Game/MCP");
	}
	PackagePath.RemoveFromEnd(TEXT("/"));
	if (!WebFrameMcp::IsAllowedGamePath(PackagePath))
	{
		OutError = FString::Printf(TEXT("Blueprint path must be under /Game. Got: %s"), *PackagePath);
		return nullptr;
	}

	FString ParentClassPath;
	Params->TryGetStringField(TEXT("parentClass"), ParentClassPath);
	UClass* ParentClass = APawn::StaticClass();
	if (!ParentClassPath.IsEmpty())
	{
		ParentClass = LoadObject<UClass>(nullptr, *ParentClassPath);
		if (!ParentClass)
		{
			OutError = FString::Printf(TEXT("Parent class not found: %s"), *ParentClassPath);
			return nullptr;
		}
	}

	if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
	{
		OutError = FString::Printf(TEXT("Cannot create a Blueprint based on class: %s"), *ParentClass->GetPathName());
		return nullptr;
	}

	const FString PackageName = PackagePath / Name;
	if (FindPackage(nullptr, *PackageName) || LoadPackage(nullptr, *PackageName, LOAD_NoWarn))
	{
		OutError = FString::Printf(TEXT("Asset already exists: %s.%s"), *PackageName, *Name);
		return nullptr;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Failed to create package: %s"), *PackageName);
		return nullptr;
	}

	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		*Name,
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		NAME_None);

	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Failed to create Blueprint: %s"), *PackageName);
		return nullptr;
	}

	FAssetRegistryModule::AssetCreated(Blueprint);
	Package->MarkPackageDirty();
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	bool bSave = true;
	Params->TryGetBoolField(TEXT("save"), bSave);
	if (bSave)
	{
		if (!UEditorLoadingAndSavingUtils::SavePackages({ Package }, false))
		{
			OutError = FString::Printf(TEXT("Blueprint created but failed to save: %s"), *Blueprint->GetPathName());
			return nullptr;
		}
	}

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(
		TEXT("资产"),
		FString::Printf(TEXT("Created Pawn Blueprint with parent class: %s."), *ParentClass->GetName())));
	RecordBlueprintChange(Blueprint->GetPathName(), TEXT("创建蓝图"), Details);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("package"), PackageName);
	Result->SetStringField(TEXT("parentClass"), ParentClass->GetPathName());
	Result->SetBoolField(TEXT("saved"), bSave);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::DescribeBlueprintAsset(const FString& AssetPath, FString& OutError)
{
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath);
		return nullptr;
	}

	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);

	TArray<TSharedPtr<FJsonValue>> GraphItems;
	for (const UEdGraph* Graph : Graphs)
	{
		GraphItems.Add(MakeShared<FJsonValueObject>(WebFrameMcp::GraphToJson(Graph)));
	}

	TArray<TSharedPtr<FJsonValue>> Variables;
	for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		Variables.Add(MakeShared<FJsonValueObject>(WebFrameMcp::VariableToJson(Variable)));
	}

	auto GraphSummary = [](const TArray<TObjectPtr<UEdGraph>>& InGraphs)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const UEdGraph* Graph : InGraphs)
		{
			if (!Graph)
			{
				continue;
			}
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Graph->GetName());
			Item->SetStringField(TEXT("path"), Graph->GetPathName());
			Item->SetNumberField(TEXT("nodeCount"), Graph->Nodes.Num());
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		return Items;
	};

	TArray<TSharedPtr<FJsonValue>> Interfaces;
	for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("interface"), Interface.Interface ? Interface.Interface->GetPathName() : FString());
		TArray<TSharedPtr<FJsonValue>> InterfaceGraphs;
		for (const UEdGraph* Graph : Interface.Graphs)
		{
			if (!Graph)
			{
				continue;
			}
			TSharedPtr<FJsonObject> GraphItem = MakeShared<FJsonObject>();
			GraphItem->SetStringField(TEXT("name"), Graph->GetName());
			GraphItem->SetStringField(TEXT("path"), Graph->GetPathName());
			GraphItem->SetNumberField(TEXT("nodeCount"), Graph->Nodes.Num());
			InterfaceGraphs.Add(MakeShared<FJsonValueObject>(GraphItem));
		}
		Item->SetArrayField(TEXT("graphs"), InterfaceGraphs);
		Interfaces.Add(MakeShared<FJsonValueObject>(Item));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("class"), Blueprint->GetClass()->GetPathName());
	Result->SetStringField(TEXT("parentClass"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString());
	Result->SetStringField(TEXT("generatedClass"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : FString());
	Result->SetArrayField(TEXT("variables"), Variables);
	Result->SetArrayField(TEXT("ubergraphPages"), GraphSummary(Blueprint->UbergraphPages));
	Result->SetArrayField(TEXT("functions"), GraphSummary(Blueprint->FunctionGraphs));
	Result->SetArrayField(TEXT("macros"), GraphSummary(Blueprint->MacroGraphs));
	Result->SetArrayField(TEXT("delegateSignatures"), GraphSummary(Blueprint->DelegateSignatureGraphs));
	Result->SetArrayField(TEXT("interfaces"), Interfaces);
	Result->SetArrayField(TEXT("graphs"), GraphItems);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::ListBlueprintVariables(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath);
		return nullptr;
	}

	TArray<TSharedPtr<FJsonValue>> Variables;
	for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		Variables.Add(MakeShared<FJsonValueObject>(WebFrameMcp::VariableToJson(Variable)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetArrayField(TEXT("variables"), Variables);
	Result->SetNumberField(TEXT("count"), Variables.Num());
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::AddBlueprintVariable(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString VariableName;
	FString DefaultValue;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("name"), VariableName);
		Params->TryGetStringField(TEXT("defaultValue"), DefaultValue);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}
	if (VariableName.IsEmpty())
	{
		OutError = TEXT("Missing params.name.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath);
		return nullptr;
	}

	const FName VarName(*VariableName);
	if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName) != INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("Variable already exists: %s"), *VariableName);
		return nullptr;
	}

	const FEdGraphPinType PinType = WebFrameMcp::ReadPinType(Params);
	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarName, PinType, DefaultValue))
	{
		OutError = FString::Printf(TEXT("Failed to add variable: %s"), *VariableName);
		return nullptr;
	}

	bool bCompile = false;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	if (bSave && Blueprint->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Blueprint->GetOutermost() }, false);
	}

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(VariableName, FString::Printf(TEXT("Add variable type %s."), *WebFrameMcp::PinTypeToString(PinType))));
	RecordBlueprintChange(Blueprint->GetPathName(), TEXT("Add Blueprint variable"), Details);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("name"), VariableName);
	Result->SetStringField(TEXT("type"), WebFrameMcp::PinTypeToString(PinType));
	Result->SetStringField(TEXT("defaultValue"), DefaultValue);
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::RenameBlueprintVariable(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString OldName;
	FString NewName;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("oldName"), OldName);
		if (OldName.IsEmpty())
		{
			Params->TryGetStringField(TEXT("variable"), OldName);
		}
		Params->TryGetStringField(TEXT("newName"), NewName);
		if (NewName.IsEmpty())
		{
			Params->TryGetStringField(TEXT("name"), NewName);
		}
	}
	if (AssetPath.IsEmpty() || OldName.IsEmpty() || NewName.IsEmpty())
	{
		OutError = TEXT("Missing params.asset, params.oldName/variable, or params.newName/name.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath);
		return nullptr;
	}

	const FName OldVarName(*OldName);
	const FName NewVarName(*NewName);
	if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, OldVarName) == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("Blueprint variable not found or not declared by this blueprint: %s"), *OldName);
		return nullptr;
	}
	if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, NewVarName) != INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("Target Blueprint variable already exists: %s"), *NewName);
		return nullptr;
	}

	Blueprint->Modify();
	FBlueprintEditorUtils::RenameMemberVariable(Blueprint, OldVarName, NewVarName);

	bool bCompile = false;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	if (bSave && Blueprint->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Blueprint->GetOutermost() }, false);
	}

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(OldName, FString::Printf(TEXT("Rename variable to %s and update references."), *NewName)));
	RecordBlueprintChange(Blueprint->GetPathName(), TEXT("Rename Blueprint variable"), Details);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("oldName"), OldName);
	Result->SetStringField(TEXT("newName"), NewName);
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::RemoveBlueprintVariable(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString VariableName;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("variable"), VariableName);
		if (VariableName.IsEmpty())
		{
			Params->TryGetStringField(TEXT("name"), VariableName);
		}
	}
	if (AssetPath.IsEmpty() || VariableName.IsEmpty())
	{
		OutError = TEXT("Missing params.asset or params.variable/name.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath);
		return nullptr;
	}

	const FName VarName(*VariableName);
	const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName);
	if (VariableIndex == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("Blueprint variable not found or not declared by this blueprint: %s"), *VariableName);
		return nullptr;
	}

	const FString OldType = WebFrameMcp::PinTypeToString(Blueprint->NewVariables[VariableIndex].VarType);
	Blueprint->Modify();
	FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VarName);

	bool bCompile = false;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	if (bSave && Blueprint->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Blueprint->GetOutermost() }, false);
	}

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(VariableName, FString::Printf(TEXT("Removed variable type %s and its variable nodes."), *OldType)));
	RecordBlueprintChange(Blueprint->GetPathName(), TEXT("Remove Blueprint variable"), Details);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("variable"), VariableName);
	Result->SetStringField(TEXT("type"), OldType);
	Result->SetBoolField(TEXT("removed"), true);
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::SetBlueprintVariableDefault(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString VariableName;
	FString DefaultValue;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("variable"), VariableName);
		Params->TryGetStringField(TEXT("defaultValue"), DefaultValue);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}
	if (VariableName.IsEmpty())
	{
		OutError = TEXT("Missing params.variable.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath);
		return nullptr;
	}

	const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, *VariableName);
	if (VariableIndex == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("Blueprint variable not found or not declared by this blueprint: %s"), *VariableName);
		return nullptr;
	}

	FBPVariableDescription& Variable = Blueprint->NewVariables[VariableIndex];
	const FString OldValue = Variable.DefaultValue;
	Blueprint->Modify();
	Variable.DefaultValue = DefaultValue;
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	bool bCompile = true;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	if (bSave && Blueprint->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Blueprint->GetOutermost() }, false);
	}

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(
		VariableName,
		FString::Printf(TEXT("变量默认值：%s -> %s"), *OldValue, *DefaultValue)));
	RecordBlueprintChange(Blueprint->GetPathName(), TEXT("设置蓝图变量默认值"), Details);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("variable"), VariableName);
	Result->SetStringField(TEXT("oldDefaultValue"), OldValue);
	Result->SetStringField(TEXT("defaultValue"), DefaultValue);
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::SetBlueprintClassDefault(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString PropertyName;
	FString DefaultValue;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("property"), PropertyName);
		Params->TryGetStringField(TEXT("defaultValue"), DefaultValue);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}
	if (PropertyName.IsEmpty())
	{
		OutError = TEXT("Missing params.property.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath);
		return nullptr;
	}
	UClass* GeneratedClass = Blueprint->GeneratedClass;
	UObject* CDO = GeneratedClass ? GeneratedClass->GetDefaultObject() : nullptr;
	if (!CDO)
	{
		OutError = FString::Printf(TEXT("Blueprint has no generated class default object: %s"), *AssetPath);
		return nullptr;
	}

	FProperty* Property = FindFProperty<FProperty>(GeneratedClass, *PropertyName);
	if (!Property)
	{
		OutError = FString::Printf(TEXT("Class default property not found: %s"), *PropertyName);
		return nullptr;
	}

	FString OldValue;
	FBlueprintEditorUtils::PropertyValueToString(Property, reinterpret_cast<const uint8*>(CDO), OldValue, CDO);
	CDO->Modify();
	if (!FBlueprintEditorUtils::PropertyValueFromString(Property, DefaultValue, reinterpret_cast<uint8*>(CDO), CDO))
	{
		OutError = FString::Printf(TEXT("Invalid default value for property %s: %s"), *PropertyName, *DefaultValue);
		return nullptr;
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	bool bCompile = true;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	if (bSave && Blueprint->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Blueprint->GetOutermost() }, false);
	}

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(
		PropertyName,
		FString::Printf(TEXT("类默认属性：%s -> %s"), *OldValue, *DefaultValue)));
	RecordBlueprintChange(Blueprint->GetPathName(), TEXT("Set Blueprint default"), Details);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("property"), PropertyName);
	Result->SetStringField(TEXT("oldDefaultValue"), OldValue);
	Result->SetStringField(TEXT("defaultValue"), DefaultValue);
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::SetBlueprintComponentDefault(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString ComponentName;
	FString PropertyName;
	FString DefaultValue;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("componentName"), ComponentName);
		Params->TryGetStringField(TEXT("property"), PropertyName);
		Params->TryGetStringField(TEXT("defaultValue"), DefaultValue);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}
	if (ComponentName.IsEmpty())
	{
		OutError = TEXT("Missing params.componentName.");
		return nullptr;
	}
	if (PropertyName.IsEmpty())
	{
		OutError = TEXT("Missing params.property.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath);
		return nullptr;
	}
	UClass* GeneratedClass = Blueprint->GeneratedClass;
	UObject* CDO = GeneratedClass ? GeneratedClass->GetDefaultObject() : nullptr;
	if (!CDO)
	{
		OutError = FString::Printf(TEXT("Blueprint has no generated class default object: %s"), *AssetPath);
		return nullptr;
	}

	FObjectPropertyBase* ComponentProperty = FindFProperty<FObjectPropertyBase>(GeneratedClass, *ComponentName);
	UObject* ComponentObject = ComponentProperty ? ComponentProperty->GetObjectPropertyValue_InContainer(CDO) : nullptr;
	if (!ComponentObject)
	{
		OutError = FString::Printf(TEXT("Component default object not found: %s"), *ComponentName);
		return nullptr;
	}

	FProperty* Property = FindFProperty<FProperty>(ComponentObject->GetClass(), *PropertyName);
	if (!Property)
	{
		OutError = FString::Printf(TEXT("Component property not found: %s.%s"), *ComponentName, *PropertyName);
		return nullptr;
	}

	FString OldValue;
	FBlueprintEditorUtils::PropertyValueToString(Property, reinterpret_cast<const uint8*>(ComponentObject), OldValue, ComponentObject);
	ComponentObject->Modify();
	bool bAppliedSpecializedSetter = false;
	if (USkeletalMeshComponent* SkeletalMeshComponent = Cast<USkeletalMeshComponent>(ComponentObject);
		SkeletalMeshComponent && (PropertyName == TEXT("SkeletalMeshAsset") || PropertyName == TEXT("SkeletalMesh")))
	{
		USkeletalMesh* SkeletalMesh = LoadObject<USkeletalMesh>(nullptr, *DefaultValue);
		if (!SkeletalMesh)
		{
			OutError = FString::Printf(TEXT("Skeletal mesh not found for component property %s.%s: %s"), *ComponentName, *PropertyName, *DefaultValue);
			return nullptr;
		}
		SkeletalMeshComponent->SetSkeletalMesh(SkeletalMesh);
		SkeletalMeshComponent->MarkRenderStateDirty();
		bAppliedSpecializedSetter = true;
	}
	else if (UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(ComponentObject);
		StaticMeshComponent && PropertyName == TEXT("StaticMesh"))
	{
		UStaticMesh* StaticMesh = LoadObject<UStaticMesh>(nullptr, *DefaultValue);
		if (!StaticMesh)
		{
			OutError = FString::Printf(TEXT("Static mesh not found for component property %s.%s: %s"), *ComponentName, *PropertyName, *DefaultValue);
			return nullptr;
		}
		StaticMeshComponent->SetStaticMesh(StaticMesh);
		StaticMeshComponent->MarkRenderStateDirty();
		bAppliedSpecializedSetter = true;
	}
	else if (UMeshComponent* MeshComponent = Cast<UMeshComponent>(ComponentObject);
		MeshComponent && PropertyName.StartsWith(TEXT("OverrideMaterials[")) && PropertyName.EndsWith(TEXT("]")))
	{
		FString IndexText = PropertyName;
		IndexText.RemoveFromStart(TEXT("OverrideMaterials["));
		IndexText.RemoveFromEnd(TEXT("]"));
		const int32 MaterialIndex = FCString::Atoi(*IndexText);
		if (MaterialIndex < 0)
		{
			OutError = FString::Printf(TEXT("Invalid material slot index for component property %s.%s"), *ComponentName, *PropertyName);
			return nullptr;
		}
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *DefaultValue);
		if (!Material)
		{
			OutError = FString::Printf(TEXT("Material not found for component property %s.%s: %s"), *ComponentName, *PropertyName, *DefaultValue);
			return nullptr;
		}
		MeshComponent->SetMaterial(MaterialIndex, Material);
		MeshComponent->MarkRenderStateDirty();
		bAppliedSpecializedSetter = true;
	}

	if (!bAppliedSpecializedSetter && !FBlueprintEditorUtils::PropertyValueFromString(Property, DefaultValue, reinterpret_cast<uint8*>(ComponentObject), ComponentObject))
	{
		OutError = FString::Printf(TEXT("Invalid default value for component property %s.%s: %s"), *ComponentName, *PropertyName, *DefaultValue);
		return nullptr;
	}
	FPropertyChangedEvent PropertyChangedEvent(Property, EPropertyChangeType::ValueSet);
	ComponentObject->PostEditChangeProperty(PropertyChangedEvent);
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	bool bCompile = true;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	if (bSave && Blueprint->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Blueprint->GetOutermost() }, false);
	}

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(
		ComponentName,
		FString::Printf(TEXT("Set component property %s.%s."), *ComponentName, *PropertyName)));
	RecordBlueprintChange(Blueprint->GetPathName(), TEXT("Set Blueprint component default"), Details);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("componentName"), ComponentName);
	Result->SetStringField(TEXT("componentClass"), ComponentObject->GetClass()->GetPathName());
	Result->SetStringField(TEXT("property"), PropertyName);
	Result->SetStringField(TEXT("oldDefaultValue"), OldValue);
	Result->SetStringField(TEXT("defaultValue"), DefaultValue);
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::SetBlueprintPinDefault(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString NodeId;
	FString PinId;
	FString PinName;
	FString DefaultValue;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("nodeId"), NodeId);
		Params->TryGetStringField(TEXT("pinId"), PinId);
		Params->TryGetStringField(TEXT("pinName"), PinName);
		Params->TryGetStringField(TEXT("defaultValue"), DefaultValue);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}
	if (NodeId.IsEmpty())
	{
		OutError = TEXT("Missing params.nodeId.");
		return nullptr;
	}
	if (PinId.IsEmpty() && PinName.IsEmpty())
	{
		OutError = TEXT("Missing params.pinId or params.pinName.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath);
		return nullptr;
	}

	UEdGraphNode* Node = WebFrameMcp::FindNodeByGuid(Blueprint, NodeId);
	if (!Node)
	{
		OutError = FString::Printf(TEXT("Node not found: %s"), *NodeId);
		return nullptr;
	}

	UEdGraphPin* Pin = WebFrameMcp::FindPin(Node, PinId, PinName);
	if (!Pin)
	{
		OutError = FString::Printf(TEXT("Pin not found: %s%s%s"), *PinId, PinId.IsEmpty() || PinName.IsEmpty() ? TEXT("") : TEXT(" / "), *PinName);
		return nullptr;
	}
	if (Pin->Direction != EGPD_Input)
	{
		OutError = FString::Printf(TEXT("Only input pin defaults can be changed: %s"), *Pin->PinName.ToString());
		return nullptr;
	}
	if (Pin->LinkedTo.Num() > 0)
	{
		OutError = FString::Printf(TEXT("Cannot change default value of linked pin: %s"), *Pin->PinName.ToString());
		return nullptr;
	}

	const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Pin->GetSchema());
	if (!Schema)
	{
		OutError = FString::Printf(TEXT("Pin schema is not K2: %s"), *Pin->PinName.ToString());
		return nullptr;
	}

	const FString OldValue = Pin->GetDefaultAsString();
	const bool bObjectPin = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Interface;
	UObject* DefaultObject = bObjectPin && !DefaultValue.IsEmpty() && DefaultValue != TEXT("None") ? LoadObject<UObject>(nullptr, *DefaultValue) : nullptr;
	if (bObjectPin && !DefaultObject && !DefaultValue.IsEmpty() && DefaultValue != TEXT("None")) { OutError = TEXT("Default object not found"); return nullptr; }
	const FString ValidationError = Schema->IsPinDefaultValid(Pin, bObjectPin ? FString() : DefaultValue, DefaultObject, FText::GetEmpty());
	if (!ValidationError.IsEmpty())
	{
		OutError = ValidationError;
		return nullptr;
	}

	Node->Modify();
	if (bObjectPin) Schema->TrySetDefaultObject(*Pin, DefaultObject, true);
	else Schema->TrySetDefaultValue(*Pin, DefaultValue, true);
	if (UEdGraph* Graph = Node->GetGraph())
	{
		Graph->NotifyGraphChanged();
	}

	bool bCompile = false;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	if (bSave && Blueprint->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Blueprint->GetOutermost() }, false);
	}

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(
		Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
		FString::Printf(TEXT("Pin %s 默认值：%s -> %s"), *Pin->PinName.ToString(), *OldValue, *DefaultValue)));
	RecordBlueprintChange(Blueprint->GetPathName(), TEXT("修改节点参数"), Details);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("nodeId"), NodeId);
	Result->SetStringField(TEXT("pinId"), Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
	Result->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
	Result->SetStringField(TEXT("oldDefaultValue"), OldValue);
	Result->SetStringField(TEXT("defaultValue"), Pin->GetDefaultAsString());
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::AddBlueprintFunctionGraph(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString Name;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("name"), Name);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}
	if (Name.IsEmpty())
	{
		OutError = TEXT("Missing params.name.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath);
		return nullptr;
	}

	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, Graph, true, nullptr);

	bool bCompile = false;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	if (bSave && Blueprint->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Blueprint->GetOutermost() }, false);
	}

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(Name, TEXT("Add function graph.")));
	RecordBlueprintChange(Blueprint->GetPathName(), TEXT("Add Blueprint function graph"), Details);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetObjectField(TEXT("graph"), WebFrameMcp::GraphToJson(Graph));
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::AddBlueprintMacroGraph(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString Name;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("name"), Name);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}
	if (Name.IsEmpty())
	{
		OutError = TEXT("Missing params.name.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath);
		return nullptr;
	}

	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddMacroGraph(Blueprint, Graph, true, nullptr);

	bool bCompile = false;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	if (bSave && Blueprint->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Blueprint->GetOutermost() }, false);
	}

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(Name, TEXT("Add macro graph.")));
	RecordBlueprintChange(Blueprint->GetPathName(), TEXT("Add Blueprint macro graph"), Details);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetObjectField(TEXT("graph"), WebFrameMcp::GraphToJson(Graph));
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::CreateBlueprintCallFunctionNode(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString GraphName;
	FString GraphPath;
	FString FunctionName;
	FString FunctionOwnerClassPath;
	FString NodeKind;
	double X = 0.0;
	double Y = 0.0;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("graphName"), GraphName);
		Params->TryGetStringField(TEXT("graphPath"), GraphPath);
		Params->TryGetStringField(TEXT("functionName"), FunctionName);
		Params->TryGetStringField(TEXT("functionOwnerClass"), FunctionOwnerClassPath);
		Params->TryGetStringField(TEXT("nodeKind"), NodeKind);
		Params->TryGetNumberField(TEXT("x"), X);
		Params->TryGetNumberField(TEXT("y"), Y);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}
	if (FunctionName.IsEmpty() && NodeKind.IsEmpty())
	{
		OutError = TEXT("Missing params.functionName.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath);
		return nullptr;
	}

	UEdGraph* Graph = WebFrameMcp::FindGraph(Blueprint, GraphName, GraphPath);
	if (!Graph)
	{
		OutError = FString::Printf(TEXT("Graph not found: %s%s%s"), *GraphName, GraphName.IsEmpty() || GraphPath.IsEmpty() ? TEXT("") : TEXT(" / "), *GraphPath);
		return nullptr;
	}

	// Structural K2 nodes complement regular function calls without importing arbitrary objects.
	if (!NodeKind.IsEmpty())
	{
		UEdGraphNode* StructuralNode = nullptr;
		Graph->Modify();
		if (NodeKind == TEXT("Branch"))
		{
			StructuralNode = NewObject<UK2Node_IfThenElse>(Graph);
		}
		else if (NodeKind == TEXT("Self"))
		{
			StructuralNode = NewObject<UK2Node_Self>(Graph);
		}
		else if (NodeKind == TEXT("ForEachLoop"))
		{
			UBlueprint* Macros = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
			UEdGraph* MacroGraph = Macros ? WebFrameMcp::FindGraph(Macros, TEXT("ForEachLoop"), FString()) : nullptr;
			if (!MacroGraph)
			{
				OutError = TEXT("Engine ForEachLoop macro not found.");
				return nullptr;
			}
			UK2Node_MacroInstance* Macro = NewObject<UK2Node_MacroInstance>(Graph);
			Macro->SetMacroGraph(MacroGraph);
			StructuralNode = Macro;
		}
		else if (NodeKind == TEXT("Event") || NodeKind == TEXT("ParentCall"))
		{
			UFunction* ParentFunction = Blueprint->ParentClass ? Blueprint->ParentClass->FindFunctionByName(*FunctionName) : nullptr;
			if (!ParentFunction)
			{
				OutError = TEXT("Parent event/function not found.");
				return nullptr;
			}
			if (NodeKind == TEXT("Event"))
			{
				UK2Node_Event* Event = NewObject<UK2Node_Event>(Graph);
				Event->EventReference.SetExternalMember(*FunctionName, ParentFunction->GetOwnerClass());
				Event->bOverrideFunction = true;
				StructuralNode = Event;
			}
			else
			{
				UK2Node_CallParentFunction* Parent = NewObject<UK2Node_CallParentFunction>(Graph);
				Parent->SetFromFunction(ParentFunction);
				StructuralNode = Parent;
			}
		}
		if (!StructuralNode)
		{
			OutError = TEXT("Unsupported nodeKind. Use Branch, Self, ForEachLoop, Event, or ParentCall.");
			return nullptr;
		}
		StructuralNode->SetFlags(RF_Transactional);
		Graph->AddNode(StructuralNode, false, false);
		StructuralNode->CreateNewGuid();
		StructuralNode->PostPlacedNewNode();
		StructuralNode->AllocateDefaultPins();
		StructuralNode->NodePosX = FMath::RoundToInt(X);
		StructuralNode->NodePosY = FMath::RoundToInt(Y);
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		Graph->NotifyGraphChanged();
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
		Result->SetStringField(TEXT("graph"), Graph->GetPathName());
		Result->SetObjectField(TEXT("node"), WebFrameMcp::NodeToJson(StructuralNode));
		Result->SetStringField(TEXT("nodeKind"), NodeKind);
		return Result;
	}

	UClass* FunctionOwnerClass = Blueprint->GeneratedClass ? Blueprint->GeneratedClass : Blueprint->ParentClass;
	if (!FunctionOwnerClassPath.IsEmpty())
	{
		FunctionOwnerClass = LoadObject<UClass>(nullptr, *FunctionOwnerClassPath);
		if (!FunctionOwnerClass)
		{
			OutError = FString::Printf(TEXT("Function owner class not found: %s"), *FunctionOwnerClassPath);
			return nullptr;
		}
	}
	if (!FunctionOwnerClass)
	{
		OutError = FString::Printf(TEXT("Blueprint has no generated or parent class: %s"), *AssetPath);
		return nullptr;
	}

	UFunction* Function = FunctionOwnerClass->FindFunctionByName(*FunctionName);
	if (!Function)
	{
		OutError = FString::Printf(TEXT("Function not found: %s on %s"), *FunctionName, *FunctionOwnerClass->GetPathName());
		return nullptr;
	}

	Graph->Modify();
	UK2Node_CallFunction* Node = nullptr;
	if (Function->HasMetaData(TEXT("ArrayParm")))
	{
		FGraphNodeCreator<UK2Node_CallArrayFunction> NodeCreator(*Graph);
		UK2Node_CallArrayFunction* ArrayNode = NodeCreator.CreateNode();
		ArrayNode->SetFromFunction(Function);
		ArrayNode->NodePosX = FMath::RoundToInt(X);
		ArrayNode->NodePosY = FMath::RoundToInt(Y);
		NodeCreator.Finalize();
		Node = ArrayNode;
	}
	else
	{
		FGraphNodeCreator<UK2Node_CallFunction> NodeCreator(*Graph);
		Node = NodeCreator.CreateNode();
		Node->SetFromFunction(Function);
		Node->NodePosX = FMath::RoundToInt(X);
		Node->NodePosY = FMath::RoundToInt(Y);
		NodeCreator.Finalize();
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	Graph->NotifyGraphChanged();

	bool bCompile = false;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	if (bSave && Blueprint->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Blueprint->GetOutermost() }, false);
	}

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(
		Graph->GetName(),
		FString::Printf(TEXT("Create call function node %s."), *Function->GetName())));
	RecordBlueprintChange(Blueprint->GetPathName(), TEXT("Create Blueprint function call node"), Details);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("graph"), Graph->GetPathName());
	Result->SetStringField(TEXT("function"), Function->GetPathName());
	Result->SetObjectField(TEXT("node"), WebFrameMcp::NodeToJson(Node));
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::CreateBlueprintCustomEventNode(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString GraphName;
	FString GraphPath;
	FString EventName;
	double X = 0.0;
	double Y = 0.0;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("graphName"), GraphName);
		Params->TryGetStringField(TEXT("graphPath"), GraphPath);
		Params->TryGetStringField(TEXT("eventName"), EventName);
		Params->TryGetNumberField(TEXT("x"), X);
		Params->TryGetNumberField(TEXT("y"), Y);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}
	if (EventName.IsEmpty())
	{
		OutError = TEXT("Missing params.eventName.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath);
		return nullptr;
	}

	UEdGraph* Graph = WebFrameMcp::FindGraph(Blueprint, GraphName, GraphPath);
	if (!Graph)
	{
		OutError = FString::Printf(TEXT("Graph not found: %s%s%s"), *GraphName, GraphName.IsEmpty() || GraphPath.IsEmpty() ? TEXT("") : TEXT(" / "), *GraphPath);
		return nullptr;
	}

	Graph->Modify();
	FGraphNodeCreator<UK2Node_CustomEvent> NodeCreator(*Graph);
	UK2Node_CustomEvent* Node = NodeCreator.CreateNode();
	Node->CustomFunctionName = FName(*EventName);
	Node->NodePosX = FMath::RoundToInt(X);
	Node->NodePosY = FMath::RoundToInt(Y);
	NodeCreator.Finalize();

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	Graph->NotifyGraphChanged();

	bool bCompile = false;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	if (bSave && Blueprint->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Blueprint->GetOutermost() }, false);
	}

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(Graph->GetName(), FString::Printf(TEXT("Add custom event %s."), *EventName)));
	RecordBlueprintChange(Blueprint->GetPathName(), TEXT("Add Blueprint custom event"), Details);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("graph"), Graph->GetPathName());
	Result->SetObjectField(TEXT("node"), WebFrameMcp::NodeToJson(Node));
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::CreateBlueprintCommentNode(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString GraphName;
	FString GraphPath;
	FString Text;
	double X = 0.0;
	double Y = 0.0;
	double Width = 400.0;
	double Height = 200.0;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("graphName"), GraphName);
		Params->TryGetStringField(TEXT("graphPath"), GraphPath);
		Params->TryGetStringField(TEXT("text"), Text);
		Params->TryGetNumberField(TEXT("x"), X);
		Params->TryGetNumberField(TEXT("y"), Y);
		Params->TryGetNumberField(TEXT("width"), Width);
		Params->TryGetNumberField(TEXT("height"), Height);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath);
		return nullptr;
	}

	UEdGraph* Graph = WebFrameMcp::FindGraph(Blueprint, GraphName, GraphPath);
	if (!Graph)
	{
		OutError = FString::Printf(TEXT("Graph not found: %s%s%s"), *GraphName, GraphName.IsEmpty() || GraphPath.IsEmpty() ? TEXT("") : TEXT(" / "), *GraphPath);
		return nullptr;
	}

	Graph->Modify();
	FGraphNodeCreator<UEdGraphNode_Comment> NodeCreator(*Graph);
	UEdGraphNode_Comment* Node = NodeCreator.CreateNode();
	Node->NodePosX = FMath::RoundToInt(X);
	Node->NodePosY = FMath::RoundToInt(Y);
	Node->NodeWidth = FMath::RoundToInt(Width);
	Node->NodeHeight = FMath::RoundToInt(Height);
	NodeCreator.Finalize();
	Node->NodeComment = Text;

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	Graph->NotifyGraphChanged();

	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bSave && Blueprint->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Blueprint->GetOutermost() }, false);
	}

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(Graph->GetName(), FString::Printf(TEXT("Add comment node: %s"), *Text)));
	RecordBlueprintChange(Blueprint->GetPathName(), TEXT("Add Blueprint comment node"), Details);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("graph"), Graph->GetPathName());
	Result->SetObjectField(TEXT("node"), WebFrameMcp::NodeToJson(Node));
	Result->SetBoolField(TEXT("saved"), bSave);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::CreateBlueprintVariableNode(const TSharedPtr<FJsonObject>& Params, const bool bSetter, FString& OutError)
{
	FString AssetPath;
	FString GraphName;
	FString GraphPath;
	FString VariableName;
	double X = 0.0;
	double Y = 0.0;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("graphName"), GraphName);
		Params->TryGetStringField(TEXT("graphPath"), GraphPath);
		Params->TryGetStringField(TEXT("variable"), VariableName);
		Params->TryGetNumberField(TEXT("x"), X);
		Params->TryGetNumberField(TEXT("y"), Y);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}
	if (VariableName.IsEmpty())
	{
		OutError = TEXT("Missing params.variable.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath);
		return nullptr;
	}

	UEdGraph* Graph = WebFrameMcp::FindGraph(Blueprint, GraphName, GraphPath);
	if (!Graph)
	{
		OutError = FString::Printf(TEXT("Graph not found: %s%s%s"), *GraphName, GraphName.IsEmpty() || GraphPath.IsEmpty() ? TEXT("") : TEXT(" / "), *GraphPath);
		return nullptr;
	}

	const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
	if (!Schema)
	{
		OutError = FString::Printf(TEXT("Graph schema is not K2: %s"), *Graph->GetPathName());
		return nullptr;
	}

	UStruct* Source = Blueprint->SkeletonGeneratedClass ? Cast<UStruct>(Blueprint->SkeletonGeneratedClass) : Cast<UStruct>(Blueprint->GeneratedClass);
	if (!Source)
	{
		Source = Blueprint->ParentClass;
	}
	if (!Source)
	{
		OutError = FString::Printf(TEXT("Blueprint has no variable source class: %s"), *AssetPath);
		return nullptr;
	}

	FString OwnerClass;
	if (Params->TryGetStringField(TEXT("ownerClass"), OwnerClass))
	{
		Source = LoadObject<UClass>(nullptr, *OwnerClass);
		if (!Source) { OutError = TEXT("Variable owner class not found"); return nullptr; }
	}
	Graph->Modify();
	UEdGraphNode* Node = bSetter
		? Cast<UEdGraphNode>(Schema->SpawnVariableSetNode(FVector2D(X, Y), Graph, FName(*VariableName), Source))
		: Cast<UEdGraphNode>(Schema->SpawnVariableGetNode(FVector2D(X, Y), Graph, FName(*VariableName), Source));
	if (!Node)
	{
		OutError = FString::Printf(TEXT("Failed to create variable %s node: %s"), bSetter ? TEXT("set") : TEXT("get"), *VariableName);
		return nullptr;
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	Graph->NotifyGraphChanged();

	bool bCompile = false;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	if (bSave && Blueprint->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Blueprint->GetOutermost() }, false);
	}

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(Graph->GetName(), FString::Printf(TEXT("Add variable %s node %s."), bSetter ? TEXT("set") : TEXT("get"), *VariableName)));
	RecordBlueprintChange(Blueprint->GetPathName(), bSetter ? TEXT("Add Blueprint variable set node") : TEXT("Add Blueprint variable get node"), Details);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("graph"), Graph->GetPathName());
	Result->SetStringField(TEXT("variable"), VariableName);
	Result->SetStringField(TEXT("nodeType"), bSetter ? TEXT("set") : TEXT("get"));
	Result->SetObjectField(TEXT("node"), WebFrameMcp::NodeToJson(Node));
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::ConnectBlueprintNodePins(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString FromNodeId;
	FString FromPinId;
	FString FromPinName;
	FString ToNodeId;
	FString ToPinId;
	FString ToPinName;
	bool bBreakExisting = false;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("fromNodeId"), FromNodeId);
		Params->TryGetStringField(TEXT("fromPinId"), FromPinId);
		Params->TryGetStringField(TEXT("fromPinName"), FromPinName);
		Params->TryGetStringField(TEXT("toNodeId"), ToNodeId);
		Params->TryGetStringField(TEXT("toPinId"), ToPinId);
		Params->TryGetStringField(TEXT("toPinName"), ToPinName);
		Params->TryGetBoolField(TEXT("breakExisting"), bBreakExisting);
		if (!bBreakExisting)
		{
			Params->TryGetBoolField(TEXT("force"), bBreakExisting);
		}
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}
	if (FromNodeId.IsEmpty() || ToNodeId.IsEmpty())
	{
		OutError = TEXT("Missing params.fromNodeId or params.toNodeId.");
		return nullptr;
	}
	if ((FromPinId.IsEmpty() && FromPinName.IsEmpty()) || (ToPinId.IsEmpty() && ToPinName.IsEmpty()))
	{
		OutError = TEXT("Missing source or target pin id/name.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath);
		return nullptr;
	}

	UEdGraphNode* FromNode = WebFrameMcp::FindNodeByGuid(Blueprint, FromNodeId);
	UEdGraphNode* ToNode = WebFrameMcp::FindNodeByGuid(Blueprint, ToNodeId);
	if (!FromNode || !ToNode)
	{
		OutError = FString::Printf(TEXT("Node not found. from=%s to=%s"), *FromNodeId, *ToNodeId);
		return nullptr;
	}

	UEdGraphPin* FromPin = WebFrameMcp::FindPin(FromNode, FromPinId, FromPinName);
	UEdGraphPin* ToPin = WebFrameMcp::FindPin(ToNode, ToPinId, ToPinName);
	if (!FromPin || !ToPin)
	{
		OutError = TEXT("Source or target pin not found.");
		return nullptr;
	}

	if (FromPin->Direction == EGPD_Input && ToPin->Direction == EGPD_Output)
	{
		Swap(FromPin, ToPin);
	}
	if (FromPin->Direction != EGPD_Output || ToPin->Direction != EGPD_Input)
	{
		OutError = FString::Printf(TEXT("Connection requires output -> input pins. from=%s to=%s"), *FromPin->PinName.ToString(), *ToPin->PinName.ToString());
		return nullptr;
	}

	const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(FromPin->GetSchema());
	if (!Schema || Schema != ToPin->GetSchema())
	{
		OutError = TEXT("Pins must both use the same K2 schema.");
		return nullptr;
	}

	if (bBreakExisting)
	{
		Schema->BreakPinLinks(*ToPin, true);
	}

	const FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
	if (Response.Response == CONNECT_RESPONSE_DISALLOW)
	{
		OutError = Response.Message.ToString();
		return nullptr;
	}

	FromNode->Modify();
	ToNode->Modify();
	if (!Schema->TryCreateConnection(FromPin, ToPin))
	{
		OutError = FString::Printf(TEXT("Failed to connect %s -> %s"), *FromPin->PinName.ToString(), *ToPin->PinName.ToString());
		return nullptr;
	}

	if (UEdGraph* Graph = FromNode->GetGraph())
	{
		Graph->NotifyGraphChanged();
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	bool bCompile = false;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	if (bSave && Blueprint->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Blueprint->GetOutermost() }, false);
	}

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(
		FromNode->GetNodeTitle(ENodeTitleType::ListView).ToString(),
		FString::Printf(TEXT("Connect %s -> %s."), *FromPin->PinName.ToString(), *ToPin->PinName.ToString())));
	RecordBlueprintChange(Blueprint->GetPathName(), TEXT("Connect Blueprint node pins"), Details);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetObjectField(TEXT("fromPin"), WebFrameMcp::PinToJson(FromPin));
	Result->SetObjectField(TEXT("toPin"), WebFrameMcp::PinToJson(ToPin));
	Result->SetBoolField(TEXT("breakExisting"), bBreakExisting);
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::MoveBlueprintNode(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString NodeId;
	double X = 0.0;
	double Y = 0.0;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("nodeId"), NodeId);
		Params->TryGetNumberField(TEXT("x"), X);
		Params->TryGetNumberField(TEXT("y"), Y);
	}
	if (AssetPath.IsEmpty() || NodeId.IsEmpty())
	{
		OutError = TEXT("Missing params.asset or params.nodeId.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	UEdGraphNode* Node = WebFrameMcp::FindNodeByGuid(Blueprint, NodeId);
	if (!Blueprint || !Node)
	{
		OutError = FString::Printf(TEXT("Blueprint or node not found: %s / %s"), *AssetPath, *NodeId);
		return nullptr;
	}

	const int32 OldX = Node->NodePosX;
	const int32 OldY = Node->NodePosY;
	Node->Modify();
	Node->NodePosX = FMath::RoundToInt(X);
	Node->NodePosY = FMath::RoundToInt(Y);
	if (UEdGraph* Graph = Node->GetGraph())
	{
		Graph->NotifyGraphChanged();
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	bool bCompile = false;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	if (bSave && Blueprint->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Blueprint->GetOutermost() }, false);
	}

	const FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(NodeTitle, FString::Printf(TEXT("Move node from (%d, %d) to (%d, %d)."), OldX, OldY, Node->NodePosX, Node->NodePosY)));
	RecordBlueprintChange(Blueprint->GetPathName(), TEXT("Move Blueprint node"), Details);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Result->SetStringField(TEXT("nodeTitle"), NodeTitle);
	Result->SetNumberField(TEXT("oldX"), OldX);
	Result->SetNumberField(TEXT("oldY"), OldY);
	Result->SetNumberField(TEXT("x"), Node->NodePosX);
	Result->SetNumberField(TEXT("y"), Node->NodePosY);
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::DeleteBlueprintNode(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString NodeId;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("nodeId"), NodeId);
	}
	if (AssetPath.IsEmpty() || NodeId.IsEmpty())
	{
		OutError = TEXT("Missing params.asset or params.nodeId.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	UEdGraphNode* Node = WebFrameMcp::FindNodeByGuid(Blueprint, NodeId);
	if (!Blueprint || !Node)
	{
		OutError = FString::Printf(TEXT("Blueprint or node not found: %s / %s"), *AssetPath, *NodeId);
		return nullptr;
	}

	UEdGraph* Graph = Node->GetGraph();
	if (!Graph)
	{
		OutError = TEXT("Blueprint node has no owning graph.");
		return nullptr;
	}

	const FString GraphName = Graph->GetName();
	const FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	const FString NodeClass = Node->GetClass()->GetName();
	const int32 PinCount = Node->Pins.Num();

	Graph->Modify();
	Node->Modify();
	const bool bRemoved = Graph->RemoveNode(Node, true);
	if (!bRemoved)
	{
		OutError = FString::Printf(TEXT("Failed to remove Blueprint node: %s"), *NodeId);
		return nullptr;
	}
	Graph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	bool bCompile = false;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	if (bSave && Blueprint->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Blueprint->GetOutermost() }, false);
	}

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(GraphName, FString::Printf(TEXT("Deleted node %s (%s), %d pins."), *NodeTitle, *NodeClass, PinCount)));
	RecordBlueprintChange(Blueprint->GetPathName(), TEXT("Delete Blueprint node"), Details);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("graph"), GraphName);
	Result->SetStringField(TEXT("nodeId"), NodeId);
	Result->SetStringField(TEXT("nodeTitle"), NodeTitle);
	Result->SetStringField(TEXT("nodeClass"), NodeClass);
	Result->SetNumberField(TEXT("pinCount"), PinCount);
	Result->SetBoolField(TEXT("deleted"), true);
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::DisconnectBlueprintNodePins(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString NodeId;
	FString PinId;
	FString PinName;
	FString OtherNodeId;
	FString OtherPinId;
	FString OtherPinName;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("nodeId"), NodeId);
		Params->TryGetStringField(TEXT("pinId"), PinId);
		Params->TryGetStringField(TEXT("pinName"), PinName);
		Params->TryGetStringField(TEXT("otherNodeId"), OtherNodeId);
		Params->TryGetStringField(TEXT("otherPinId"), OtherPinId);
		Params->TryGetStringField(TEXT("otherPinName"), OtherPinName);
	}
	if (AssetPath.IsEmpty() || NodeId.IsEmpty())
	{
		OutError = TEXT("Missing params.asset or params.nodeId.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	UEdGraphNode* Node = WebFrameMcp::FindNodeByGuid(Blueprint, NodeId);
	if (!Blueprint || !Node)
	{
		OutError = FString::Printf(TEXT("Blueprint or node not found: %s / %s"), *AssetPath, *NodeId);
		return nullptr;
	}

	UEdGraph* Graph = Node->GetGraph();
	const FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	int32 DisconnectedLinks = 0;
	FString Scope;

	if (!PinId.IsEmpty() || !PinName.IsEmpty())
	{
		UEdGraphPin* Pin = WebFrameMcp::FindPin(Node, PinId, PinName);
		if (!Pin)
		{
			OutError = TEXT("Target pin not found.");
			return nullptr;
		}
		Scope = Pin->PinName.ToString();
		Node->Modify();

		if (!OtherNodeId.IsEmpty() || !OtherPinId.IsEmpty() || !OtherPinName.IsEmpty())
		{
			UEdGraphNode* OtherNode = WebFrameMcp::FindNodeByGuid(Blueprint, OtherNodeId);
			UEdGraphPin* OtherPin = WebFrameMcp::FindPin(OtherNode, OtherPinId, OtherPinName);
			if (!OtherNode || !OtherPin)
			{
				OutError = TEXT("Other node or pin not found.");
				return nullptr;
			}
			if (Pin->LinkedTo.Contains(OtherPin))
			{
				if (const UEdGraphSchema* Schema = Pin->GetSchema())
				{
					Schema->BreakSinglePinLink(Pin, OtherPin);
				}
				else
				{
					Pin->BreakLinkTo(OtherPin);
				}
				DisconnectedLinks = 1;
				OtherNode->Modify();
			}
		}
		else
		{
			DisconnectedLinks = Pin->LinkedTo.Num();
			if (const UEdGraphSchema* Schema = Pin->GetSchema())
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
		Scope = TEXT("all pins");
		Node->Modify();
		if (const UEdGraphSchema* Schema = Node->GetSchema())
		{
			Schema->BreakNodeLinks(*Node);
		}
		else
		{
			Node->BreakAllNodeLinks();
		}
	}

	if (Graph)
	{
		Graph->NotifyGraphChanged();
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	bool bCompile = false;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	if (bSave && Blueprint->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Blueprint->GetOutermost() }, false);
	}

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(NodeTitle, FString::Printf(TEXT("Disconnected %d link(s) from %s."), DisconnectedLinks, *Scope)));
	RecordBlueprintChange(Blueprint->GetPathName(), TEXT("Disconnect Blueprint node pins"), Details);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Result->SetStringField(TEXT("nodeTitle"), NodeTitle);
	Result->SetStringField(TEXT("scope"), Scope);
	Result->SetNumberField(TEXT("disconnectedLinks"), DisconnectedLinks);
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::CompileBlueprintAsset(const FString& AssetPath, FString& OutError)
{
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath);
		return nullptr;
	}

	const int32 NodeCountBefore = WebFrameMcp::CountNodes(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	const int32 NodeCountAfter = WebFrameMcp::CountNodes(Blueprint);
	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(
		TEXT("编译"),
		FString::Printf(TEXT("Blueprint compile node count %d -> %d."), NodeCountBefore, NodeCountAfter)));
	RecordBlueprintChange(AssetPath, TEXT("编译蓝图"), Details);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("status"), UEnum::GetValueAsString(Blueprint->Status));
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::CreateMaterialAsset(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString PackagePath;
	FString AssetName;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("path"), PackagePath);
		Params->TryGetStringField(TEXT("name"), AssetName);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}

	if (!AssetPath.IsEmpty())
	{
		FString PackagePart;
		FString ObjectPart;
		if (AssetPath.Split(TEXT("."), &PackagePart, &ObjectPart, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			PackagePath = FPaths::GetPath(PackagePart);
			AssetName = ObjectPart;
		}
		else
		{
			PackagePath = FPaths::GetPath(AssetPath);
			AssetName = FPaths::GetBaseFilename(AssetPath);
		}
	}

	PackagePath = WebFrameMcp::NormalizeGamePath(PackagePath);
	if (!WebFrameMcp::IsAllowedGamePath(PackagePath))
	{
		OutError = FString::Printf(TEXT("Material path must be under /Game: %s"), *PackagePath);
		return nullptr;
	}
	if (AssetName.IsEmpty())
	{
		OutError = TEXT("Missing params.name or params.asset.");
		return nullptr;
	}

	const FString FullAssetPath = PackagePath / AssetName + TEXT(".") + AssetName;
	if (LoadObject<UMaterial>(nullptr, *FullAssetPath))
	{
		OutError = FString::Printf(TEXT("Material already exists: %s"), *FullAssetPath);
		return nullptr;
	}

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
	UObject* CreatedAsset = AssetToolsModule.Get().CreateAsset(AssetName, PackagePath, UMaterial::StaticClass(), Factory);
	UMaterial* Material = Cast<UMaterial>(CreatedAsset);
	if (!Material)
	{
		OutError = FString::Printf(TEXT("Failed to create material: %s/%s"), *PackagePath, *AssetName);
		return nullptr;
	}

	FString SettingsError;
	if (Params.IsValid() && Params->HasField(TEXT("settings")))
	{
		TSharedPtr<FJsonObject> SettingsParams = MakeShared<FJsonObject>();
		SettingsParams->SetStringField(TEXT("asset"), Material->GetPathName());
		const TSharedPtr<FJsonObject>* SettingsObject = nullptr;
		if (Params->TryGetObjectField(TEXT("settings"), SettingsObject) && SettingsObject && SettingsObject->IsValid())
		{
			SettingsParams->SetObjectField(TEXT("settings"), *SettingsObject);
		}
		bool bCompile = true;
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		SettingsParams->SetBoolField(TEXT("compile"), bCompile);
		SettingsParams->SetBoolField(TEXT("save"), false);

		TSharedPtr<FJsonObject> SettingsResult = SetMaterialSettings(SettingsParams, SettingsError);
		if (!SettingsResult.IsValid() || !SettingsError.IsEmpty())
		{
			OutError = SettingsError.IsEmpty() ? TEXT("Failed to apply material settings.") : SettingsError;
			return nullptr;
		}
	}

	Material->PostEditChange();
	Material->MarkPackageDirty();
	if (bSave && Material->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Material->GetOutermost() }, false);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Material->GetPathName());
	Result->SetObjectField(TEXT("settings"), WebFrameMcp::MaterialSettingsToJson(Material));
	Result->SetBoolField(TEXT("saved"), bSave);

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(AssetName, FString::Printf(TEXT("Create material asset under %s."), *PackagePath)));
	if (Params.IsValid() && Params->HasField(TEXT("settings")))
	{
		Details.Add(TPair<FString, FString>(TEXT("settings"), TEXT("Applied initial material settings.")));
	}
	RecordBlueprintChange(Material->GetPathName(), TEXT("Create material asset"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::DescribeMaterialAsset(const FString& AssetPath, FString& OutError)
{
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
	if (!Material)
	{
		if (UMaterialInstance* Instance = LoadObject<UMaterialInstance>(nullptr, *AssetPath))
		{
			return WebFrameMcp::MaterialInstanceToJson(Instance);
		}

		OutError = FString::Printf(TEXT("Material or material instance not found: %s"), *AssetPath);
		return nullptr;
	}

	TArray<TSharedPtr<FJsonValue>> Expressions;
	for (UMaterialExpression* Expression : Material->GetExpressions())
	{
		if (!Expression)
		{
			continue;
		}
		TSharedPtr<FJsonObject> Expr = MakeShared<FJsonObject>();
		Expr->SetStringField(TEXT("name"), Expression->GetName());
		Expr->SetStringField(TEXT("class"), Expression->GetClass()->GetPathName());
		Expr->SetNumberField(TEXT("x"), Expression->MaterialExpressionEditorX);
		Expr->SetNumberField(TEXT("y"), Expression->MaterialExpressionEditorY);
		Expressions.Add(MakeShared<FJsonValueObject>(Expr));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Material->GetPathName());
	Result->SetStringField(TEXT("class"), Material->GetClass()->GetPathName());
	Result->SetObjectField(TEXT("settings"), WebFrameMcp::MaterialSettingsToJson(Material));
	Result->SetArrayField(TEXT("expressions"), Expressions);
	Result->SetArrayField(TEXT("outputs"), WebFrameMcp::MaterialOutputConnectionsToJson(Material));
	Result->SetNumberField(TEXT("expressionCount"), Expressions.Num());
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::SetMaterialSettings(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
	if (!Material)
	{
		OutError = FString::Printf(TEXT("Material not found: %s"), *AssetPath);
		return nullptr;
	}

	TSharedPtr<FJsonObject> SettingsParams = Params;
	const TSharedPtr<FJsonObject>* SettingsObject = nullptr;
	if (Params.IsValid() && Params->TryGetObjectField(TEXT("settings"), SettingsObject) && SettingsObject && SettingsObject->IsValid())
	{
		SettingsParams = *SettingsObject;
	}

	auto TryGetStringSetting = [&SettingsParams](std::initializer_list<const TCHAR*> Names, FString& OutValue) -> bool
	{
		if (!SettingsParams.IsValid())
		{
			return false;
		}
		for (const TCHAR* Name : Names)
		{
			if (SettingsParams->TryGetStringField(Name, OutValue))
			{
				return true;
			}
		}
		return false;
	};

	auto TryGetBoolSetting = [&SettingsParams](std::initializer_list<const TCHAR*> Names, bool& OutValue) -> bool
	{
		if (!SettingsParams.IsValid())
		{
			return false;
		}
		for (const TCHAR* Name : Names)
		{
			if (SettingsParams->TryGetBoolField(Name, OutValue))
			{
				return true;
			}
		}
		return false;
	};

	auto TryGetNumberSetting = [&SettingsParams](std::initializer_list<const TCHAR*> Names, double& OutValue) -> bool
	{
		if (!SettingsParams.IsValid())
		{
			return false;
		}
		for (const TCHAR* Name : Names)
		{
			if (SettingsParams->TryGetNumberField(Name, OutValue))
			{
				return true;
			}
		}
		return false;
	};

	TSharedPtr<FJsonObject> OldSettings = WebFrameMcp::MaterialSettingsToJson(Material);
	bool bTouched = false;
	bool bSetDomain = false;
	bool bSetBlendMode = false;
	bool bSetShadingModel = false;
	bool bSetTranslucencyLightingMode = false;
	bool bSetBlendableLocation = false;
	bool bSetOpacityMaskClipValue = false;
	bool bSetBlendablePriority = false;
	bool bSetTwoSided = false;
	bool bSetDitheredLODTransition = false;
	bool bSetUseMaterialAttributes = false;
	bool bSetDisableDepthTest = false;
	bool bSetOutputTranslucentVelocity = false;
	bool bSetIsBlendable = false;
	bool bSetBlendableOutputAlpha = false;
	bool bSetDisablePreExposureScale = false;
	bool bSetEnableStencilTest = false;
	bool bSetStencilCompare = false;
	bool bSetStencilRefValue = false;

	EMaterialDomain NewDomain = static_cast<EMaterialDomain>(Material->MaterialDomain.GetValue());
	EBlendMode NewBlendMode = static_cast<EBlendMode>(Material->BlendMode.GetValue());
	EMaterialShadingModel NewShadingModel = Material->GetShadingModels().IsValid() ? Material->GetShadingModels().GetFirstShadingModel() : MSM_DefaultLit;
	ETranslucencyLightingMode NewTranslucencyLightingMode = static_cast<ETranslucencyLightingMode>(Material->TranslucencyLightingMode.GetValue());
	EBlendableLocation NewBlendableLocation = static_cast<EBlendableLocation>(Material->BlendableLocation.GetValue());
	float NewOpacityMaskClipValue = Material->OpacityMaskClipValue;
	int32 NewBlendablePriority = Material->BlendablePriority;
	bool bNewTwoSided = Material->TwoSided != 0;
	bool bNewDitheredLODTransition = Material->DitheredLODTransition != 0;
	bool bNewUseMaterialAttributes = Material->bUseMaterialAttributes != 0;
	bool bNewDisableDepthTest = Material->bDisableDepthTest != 0;
	bool bNewOutputTranslucentVelocity = Material->bOutputTranslucentVelocity != 0;
	bool bNewIsBlendable = Material->bIsBlendable != 0;
	bool bNewBlendableOutputAlpha = Material->BlendableOutputAlpha != 0;
	bool bNewDisablePreExposureScale = Material->bDisablePreExposureScale != 0;
	bool bNewEnableStencilTest = Material->bEnableStencilTest != 0;
	EMaterialStencilCompare NewStencilCompare = static_cast<EMaterialStencilCompare>(Material->StencilCompare.GetValue());
	int32 NewStencilRefValue = Material->StencilRefValue;

	FString StringValue;
	if (TryGetStringSetting({ TEXT("domain"), TEXT("materialDomain") }, StringValue))
	{
		if (!WebFrameMcp::ParseMaterialDomain(StringValue, NewDomain))
		{
			OutError = FString::Printf(TEXT("Unknown material domain: %s"), *StringValue);
			return nullptr;
		}
		bSetDomain = true;
		bTouched = true;
	}

	if (TryGetStringSetting({ TEXT("blendMode"), TEXT("blend") }, StringValue))
	{
		if (!WebFrameMcp::ParseBlendMode(StringValue, NewBlendMode))
		{
			OutError = FString::Printf(TEXT("Unknown material blend mode: %s"), *StringValue);
			return nullptr;
		}
		bSetBlendMode = true;
		bTouched = true;
	}

	if (TryGetStringSetting({ TEXT("shadingModel"), TEXT("shading") }, StringValue))
	{
		if (!WebFrameMcp::ParseShadingModel(StringValue, NewShadingModel))
		{
			OutError = FString::Printf(TEXT("Unknown material shading model: %s"), *StringValue);
			return nullptr;
		}
		bSetShadingModel = true;
		bTouched = true;
	}

	if (TryGetStringSetting({ TEXT("translucencyLightingMode"), TEXT("translucencyLighting") }, StringValue))
	{
		if (!WebFrameMcp::ParseTranslucencyLightingMode(StringValue, NewTranslucencyLightingMode))
		{
			OutError = FString::Printf(TEXT("Unknown translucency lighting mode: %s"), *StringValue);
			return nullptr;
		}
		bSetTranslucencyLightingMode = true;
		bTouched = true;
	}

	if (TryGetStringSetting({ TEXT("postProcessLocation"), TEXT("blendableLocation"), TEXT("postProcessBlendableLocation") }, StringValue))
	{
		if (!WebFrameMcp::ParseBlendableLocation(StringValue, NewBlendableLocation))
		{
			OutError = FString::Printf(TEXT("Unknown post process blendable location: %s"), *StringValue);
			return nullptr;
		}
		bSetBlendableLocation = true;
		bTouched = true;
	}
	if (TryGetStringSetting({ TEXT("stencilCompare"), TEXT("postProcessStencilCompare") }, StringValue))
	{
		int64 EnumValue = INDEX_NONE;
		if (!WebFrameMcp::TryParseEnumValue(StaticEnum<EMaterialStencilCompare>(), StringValue, EnumValue))
		{
			OutError = FString::Printf(TEXT("Unknown stencil compare value: %s"), *StringValue);
			return nullptr;
		}
		NewStencilCompare = static_cast<EMaterialStencilCompare>(EnumValue);
		bSetStencilCompare = true;
		bTouched = true;
	}

	double NumberValue = 0.0;
	if (TryGetNumberSetting({ TEXT("opacityMaskClipValue"), TEXT("maskClipValue") }, NumberValue))
	{
		NewOpacityMaskClipValue = static_cast<float>(NumberValue);
		bSetOpacityMaskClipValue = true;
		bTouched = true;
	}
	if (TryGetNumberSetting({ TEXT("blendablePriority"), TEXT("postProcessPriority") }, NumberValue))
	{
		NewBlendablePriority = FMath::RoundToInt(NumberValue);
		bSetBlendablePriority = true;
		bTouched = true;
	}
	if (TryGetNumberSetting({ TEXT("stencilRefValue"), TEXT("stencilRef"), TEXT("postProcessStencilRef") }, NumberValue))
	{
		NewStencilRefValue = FMath::Clamp(FMath::RoundToInt(NumberValue), 0, 255);
		bSetStencilRefValue = true;
		bTouched = true;
	}

	bool BoolValue = false;
	if (TryGetBoolSetting({ TEXT("twoSided"), TEXT("bTwoSided") }, BoolValue))
	{
		bNewTwoSided = BoolValue;
		bSetTwoSided = true;
		bTouched = true;
	}
	if (TryGetBoolSetting({ TEXT("ditheredLODTransition"), TEXT("ditheredLodTransition") }, BoolValue))
	{
		bNewDitheredLODTransition = BoolValue;
		bSetDitheredLODTransition = true;
		bTouched = true;
	}
	if (TryGetBoolSetting({ TEXT("useMaterialAttributes"), TEXT("bUseMaterialAttributes") }, BoolValue))
	{
		bNewUseMaterialAttributes = BoolValue;
		bSetUseMaterialAttributes = true;
		bTouched = true;
	}
	if (TryGetBoolSetting({ TEXT("disableDepthTest"), TEXT("bDisableDepthTest") }, BoolValue))
	{
		bNewDisableDepthTest = BoolValue;
		bSetDisableDepthTest = true;
		bTouched = true;
	}
	if (TryGetBoolSetting({ TEXT("outputTranslucentVelocity"), TEXT("bOutputTranslucentVelocity") }, BoolValue))
	{
		bNewOutputTranslucentVelocity = BoolValue;
		bSetOutputTranslucentVelocity = true;
		bTouched = true;
	}
	if (TryGetBoolSetting({ TEXT("isBlendable"), TEXT("bIsBlendable") }, BoolValue))
	{
		bNewIsBlendable = BoolValue;
		bSetIsBlendable = true;
		bTouched = true;
	}
	if (TryGetBoolSetting({ TEXT("blendableOutputAlpha"), TEXT("outputAlpha") }, BoolValue))
	{
		bNewBlendableOutputAlpha = BoolValue;
		bSetBlendableOutputAlpha = true;
		bTouched = true;
	}
	if (TryGetBoolSetting({ TEXT("disablePreExposureScale"), TEXT("bDisablePreExposureScale") }, BoolValue))
	{
		bNewDisablePreExposureScale = BoolValue;
		bSetDisablePreExposureScale = true;
		bTouched = true;
	}
	if (TryGetBoolSetting({ TEXT("enableStencilTest"), TEXT("bEnableStencilTest"), TEXT("postProcessStencilTest") }, BoolValue))
	{
		bNewEnableStencilTest = BoolValue;
		bSetEnableStencilTest = true;
		bTouched = true;
	}

	if (!bTouched)
	{
		OutError = TEXT("No supported material settings were provided.");
		return nullptr;
	}

	Material->Modify();
	if (bSetDomain) { Material->MaterialDomain = NewDomain; }
	if (bSetBlendMode) { Material->BlendMode = NewBlendMode; }
	if (bSetShadingModel) { Material->SetShadingModel(NewShadingModel); }
	if (bSetTranslucencyLightingMode) { Material->TranslucencyLightingMode = NewTranslucencyLightingMode; }
	if (bSetBlendableLocation) { Material->BlendableLocation = NewBlendableLocation; }
	if (bSetOpacityMaskClipValue) { Material->OpacityMaskClipValue = NewOpacityMaskClipValue; }
	if (bSetBlendablePriority) { Material->BlendablePriority = NewBlendablePriority; }
	if (bSetTwoSided) { Material->TwoSided = bNewTwoSided ? 1 : 0; }
	if (bSetDitheredLODTransition) { Material->DitheredLODTransition = bNewDitheredLODTransition ? 1 : 0; }
	if (bSetUseMaterialAttributes) { Material->bUseMaterialAttributes = bNewUseMaterialAttributes ? 1 : 0; }
	if (bSetDisableDepthTest) { Material->bDisableDepthTest = bNewDisableDepthTest ? 1 : 0; }
	if (bSetOutputTranslucentVelocity) { Material->bOutputTranslucentVelocity = bNewOutputTranslucentVelocity ? 1 : 0; }
	if (bSetIsBlendable) { Material->bIsBlendable = bNewIsBlendable ? 1 : 0; }
	if (bSetBlendableOutputAlpha) { Material->BlendableOutputAlpha = bNewBlendableOutputAlpha ? 1 : 0; }
	if (bSetDisablePreExposureScale) { Material->bDisablePreExposureScale = bNewDisablePreExposureScale ? 1 : 0; }
	if (bSetEnableStencilTest) { Material->bEnableStencilTest = bNewEnableStencilTest ? 1 : 0; }
	if (bSetStencilCompare) { Material->StencilCompare = NewStencilCompare; }
	if (bSetStencilRefValue) { Material->StencilRefValue = static_cast<uint8>(NewStencilRefValue); }
	Material->PostEditChange();
	Material->MarkPackageDirty();

	bool bCompile = true;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
	}
	if (bSave && Material->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Material->GetOutermost() }, false);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Material->GetPathName());
	Result->SetObjectField(TEXT("oldSettings"), OldSettings);
	Result->SetObjectField(TEXT("settings"), WebFrameMcp::MaterialSettingsToJson(Material));
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(Material->GetName(), TEXT("Set material settings.")));
	RecordBlueprintChange(Material->GetPathName(), TEXT("Set material settings"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::CreateMaterialInstanceAsset(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString PackagePath;
	FString AssetName;
	FString ParentPath;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("path"), PackagePath);
		Params->TryGetStringField(TEXT("name"), AssetName);
		Params->TryGetStringField(TEXT("parent"), ParentPath);
		Params->TryGetStringField(TEXT("parentMaterial"), ParentPath);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}

	if (!AssetPath.IsEmpty())
	{
		FString PackagePart;
		FString ObjectPart;
		if (AssetPath.Split(TEXT("."), &PackagePart, &ObjectPart, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			PackagePath = FPaths::GetPath(PackagePart);
			AssetName = ObjectPart;
		}
		else
		{
			PackagePath = FPaths::GetPath(AssetPath);
			AssetName = FPaths::GetBaseFilename(AssetPath);
		}
	}

	PackagePath = WebFrameMcp::NormalizeGamePath(PackagePath);
	if (!WebFrameMcp::IsAllowedGamePath(PackagePath))
	{
		OutError = FString::Printf(TEXT("Material instance path must be under /Game: %s"), *PackagePath);
		return nullptr;
	}
	if (AssetName.IsEmpty())
	{
		OutError = TEXT("Missing params.name or params.asset.");
		return nullptr;
	}
	if (ParentPath.IsEmpty())
	{
		OutError = TEXT("Missing params.parent or params.parentMaterial.");
		return nullptr;
	}

	UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *ParentPath);
	if (!Parent)
	{
		OutError = FString::Printf(TEXT("Parent material interface not found: %s"), *ParentPath);
		return nullptr;
	}

	const FString FullAssetPath = PackagePath / AssetName + TEXT(".") + AssetName;
	if (LoadObject<UMaterialInterface>(nullptr, *FullAssetPath))
	{
		OutError = FString::Printf(TEXT("Material interface already exists: %s"), *FullAssetPath);
		return nullptr;
	}

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
	Factory->InitialParent = Parent;
	UObject* CreatedAsset = AssetToolsModule.Get().CreateAsset(AssetName, PackagePath, UMaterialInstanceConstant::StaticClass(), Factory);
	UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(CreatedAsset);
	if (!Instance)
	{
		OutError = FString::Printf(TEXT("Failed to create material instance: %s/%s"), *PackagePath, *AssetName);
		return nullptr;
	}

	Instance->SetParentEditorOnly(Parent);
	Instance->PostEditChange();
	Instance->MarkPackageDirty();
	if (bSave && Instance->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Instance->GetOutermost() }, false);
	}

	TSharedPtr<FJsonObject> Result = WebFrameMcp::MaterialInstanceToJson(Instance);
	Result->SetBoolField(TEXT("saved"), bSave);

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(AssetName, FString::Printf(TEXT("Create material instance with parent %s."), *ParentPath)));
	RecordBlueprintChange(Instance->GetPathName(), TEXT("Create material instance"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::SetMaterialInstanceParameter(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString ParameterName;
	FString Type;
	FString StringValue;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("parameter"), ParameterName);
		Params->TryGetStringField(TEXT("name"), ParameterName);
		Params->TryGetStringField(TEXT("type"), Type);
		Params->TryGetStringField(TEXT("value"), StringValue);
		Params->TryGetStringField(TEXT("texture"), StringValue);
		Params->TryGetStringField(TEXT("parent"), StringValue);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}
	if (Type.IsEmpty())
	{
		OutError = TEXT("Missing params.type. Use scalar, vector, texture, or parent.");
		return nullptr;
	}

	UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *AssetPath);
	if (!Instance)
	{
		OutError = FString::Printf(TEXT("MaterialInstanceConstant not found: %s"), *AssetPath);
		return nullptr;
	}

	const FString TypeKey = Type.ToLower();
	const FMaterialParameterInfo ParameterInfo(*ParameterName);
	const FScopedTransaction Transaction(LOCTEXT("WebFrameMcpSetMaterialInstanceParameter", "WebFrame MCP Set Material Instance Parameter"));
	Instance->Modify();

	if (TypeKey == TEXT("scalar") || TypeKey == TEXT("float"))
	{
		if (ParameterName.IsEmpty())
		{
			OutError = TEXT("Missing params.parameter for scalar value.");
			return nullptr;
		}
		double NumberValue = 0.0;
		if (Params.IsValid() && !Params->TryGetNumberField(TEXT("value"), NumberValue))
		{
			NumberValue = FCString::Atod(*StringValue);
		}
		Instance->SetScalarParameterValueEditorOnly(ParameterInfo, static_cast<float>(NumberValue));
	}
	else if (TypeKey == TEXT("vector") || TypeKey == TEXT("color"))
	{
		if (ParameterName.IsEmpty())
		{
			OutError = TEXT("Missing params.parameter for vector value.");
			return nullptr;
		}
		FLinearColor ColorValue = FLinearColor::White;
		if (!WebFrameMcp::TryReadLinearColor(Params, TEXT("value"), ColorValue))
		{
			OutError = TEXT("Vector value must be an array [r,g,b,a] or object {r,g,b,a}.");
			return nullptr;
		}
		Instance->SetVectorParameterValueEditorOnly(ParameterInfo, ColorValue);
	}
	else if (TypeKey == TEXT("texture") || TypeKey == TEXT("texture2d"))
	{
		if (ParameterName.IsEmpty())
		{
			OutError = TEXT("Missing params.parameter for texture value.");
			return nullptr;
		}
		if (StringValue.IsEmpty())
		{
			OutError = TEXT("Missing params.value or params.texture for texture value.");
			return nullptr;
		}
		UTexture* Texture = LoadObject<UTexture>(nullptr, *StringValue);
		if (!Texture)
		{
			OutError = FString::Printf(TEXT("Texture not found: %s"), *StringValue);
			return nullptr;
		}
		Instance->SetTextureParameterValueEditorOnly(ParameterInfo, Texture);
	}
	else if (TypeKey == TEXT("parent"))
	{
		if (StringValue.IsEmpty())
		{
			OutError = TEXT("Missing params.value or params.parent for parent value.");
			return nullptr;
		}
		UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *StringValue);
		if (!Parent)
		{
			OutError = FString::Printf(TEXT("Parent material interface not found: %s"), *StringValue);
			return nullptr;
		}
		Instance->SetParentEditorOnly(Parent);
	}
	else
	{
		OutError = FString::Printf(TEXT("Unsupported material instance parameter type: %s"), *Type);
		return nullptr;
	}

	Instance->PostEditChange();
	Instance->MarkPackageDirty();
	if (bSave && Instance->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Instance->GetOutermost() }, false);
	}

	TSharedPtr<FJsonObject> Result = WebFrameMcp::MaterialInstanceToJson(Instance);
	Result->SetStringField(TEXT("changedType"), Type);
	Result->SetStringField(TEXT("changedParameter"), ParameterName);
	Result->SetBoolField(TEXT("saved"), bSave);

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(
		ParameterName.IsEmpty() ? Type : ParameterName,
		FString::Printf(TEXT("Set material instance %s parameter."), *Type)));
	RecordBlueprintChange(Instance->GetPathName(), TEXT("Set material instance parameter"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::AddMaterialExpressionNode(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString NodeType;
	double X = 0.0;
	double Y = 0.0;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("nodeType"), NodeType);
		if (NodeType.IsEmpty())
		{
			Params->TryGetStringField(TEXT("expressionClass"), NodeType);
		}
		Params->TryGetNumberField(TEXT("x"), X);
		Params->TryGetNumberField(TEXT("y"), Y);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}
	if (NodeType.IsEmpty())
	{
		OutError = TEXT("Missing params.nodeType or params.expressionClass.");
		return nullptr;
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
	if (!Material)
	{
		OutError = FString::Printf(TEXT("Material not found: %s"), *AssetPath);
		return nullptr;
	}

	UClass* ExpressionClass = WebFrameMcp::ResolveMaterialExpressionClass(NodeType);
	if (!ExpressionClass || !ExpressionClass->IsChildOf(UMaterialExpression::StaticClass()))
	{
		OutError = FString::Printf(TEXT("Material expression class not found or invalid: %s"), *NodeType);
		return nullptr;
	}

	UMaterialExpression* Expression = UMaterialEditingLibrary::CreateMaterialExpression(
		Material,
		ExpressionClass,
		FMath::RoundToInt(X),
		FMath::RoundToInt(Y));
	if (!Expression)
	{
		OutError = FString::Printf(TEXT("Failed to create material expression: %s"), *NodeType);
		return nullptr;
	}

	const TSharedPtr<FJsonObject>* Properties = nullptr;
	if (Params.IsValid() && Params->TryGetObjectField(TEXT("properties"), Properties) && Properties && Properties->IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Properties)->Values)
		{
			FString ValueString;
			if (Pair.Value.IsValid())
			{
				if (Pair.Value->Type == EJson::String)
				{
					ValueString = Pair.Value->AsString();
				}
				else
				{
					TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ValueString);
					FJsonSerializer::Serialize(Pair.Value, TEXT(""), Writer);
				}
			}

			FString OldValue;
			if (!WebFrameMcp::SetObjectPropertyFromString(Expression, Pair.Key, ValueString, OldValue, OutError))
			{
				return nullptr;
			}
		}
	}

	Material->PostEditChange();
	Material->MarkPackageDirty();

	bool bCompile = true;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
	}
	if (bSave && Material->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Material->GetOutermost() }, false);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Material->GetPathName());
	Result->SetStringField(TEXT("name"), Expression->GetName());
	Result->SetStringField(TEXT("class"), Expression->GetClass()->GetPathName());
	Result->SetNumberField(TEXT("x"), Expression->MaterialExpressionEditorX);
	Result->SetNumberField(TEXT("y"), Expression->MaterialExpressionEditorY);
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(Expression->GetName(), FString::Printf(TEXT("Add material expression node %s at (%d, %d)."), *Expression->GetClass()->GetName(), Expression->MaterialExpressionEditorX, Expression->MaterialExpressionEditorY)));
	RecordBlueprintChange(Material->GetPathName(), TEXT("Add material expression node"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::SetMaterialExpressionProperty(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString ExpressionName;
	FString PropertyName;
	FString Value;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("expression"), ExpressionName);
		Params->TryGetStringField(TEXT("property"), PropertyName);
		Params->TryGetStringField(TEXT("value"), Value);
	}
	if (AssetPath.IsEmpty() || ExpressionName.IsEmpty() || PropertyName.IsEmpty())
	{
		OutError = TEXT("Missing params.asset, params.expression, or params.property.");
		return nullptr;
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
	UMaterialExpression* Expression = WebFrameMcp::FindMaterialExpression(Material, ExpressionName);
	if (!Material || !Expression)
	{
		OutError = FString::Printf(TEXT("Material or expression not found: %s / %s"), *AssetPath, *ExpressionName);
		return nullptr;
	}

	FString OldValue;
	if (!WebFrameMcp::SetObjectPropertyFromString(Expression, PropertyName, Value, OldValue, OutError))
	{
		return nullptr;
	}

	Material->PostEditChange();
	Material->MarkPackageDirty();
	bool bCompile = true;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
	}
	if (bSave && Material->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Material->GetOutermost() }, false);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Material->GetPathName());
	Result->SetStringField(TEXT("expression"), ExpressionName);
	Result->SetStringField(TEXT("property"), PropertyName);
	Result->SetStringField(TEXT("oldValue"), OldValue);
	Result->SetStringField(TEXT("value"), Value);
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(ExpressionName, FString::Printf(TEXT("Set expression property %s from %s to %s."), *PropertyName, *OldValue, *Value)));
	RecordBlueprintChange(Material->GetPathName(), TEXT("Set material expression property"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::ConnectMaterialExpressionNodes(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString FromExpressionName;
	FString FromOutputName;
	FString ToExpressionName;
	FString ToInputName;
	bool bForce = false;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("fromExpression"), FromExpressionName);
		Params->TryGetStringField(TEXT("fromOutput"), FromOutputName);
		Params->TryGetStringField(TEXT("toExpression"), ToExpressionName);
		Params->TryGetStringField(TEXT("toInput"), ToInputName);
		Params->TryGetBoolField(TEXT("force"), bForce);
	}
	if (AssetPath.IsEmpty() || FromExpressionName.IsEmpty() || ToExpressionName.IsEmpty())
	{
		OutError = TEXT("Missing params.asset, params.fromExpression, or params.toExpression.");
		return nullptr;
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
	UMaterialExpression* FromExpression = WebFrameMcp::FindMaterialExpression(Material, FromExpressionName);
	UMaterialExpression* ToExpression = WebFrameMcp::FindMaterialExpression(Material, ToExpressionName);
	if (!Material || !FromExpression || !ToExpression)
	{
		OutError = FString::Printf(TEXT("Material expression not found: %s -> %s"), *FromExpressionName, *ToExpressionName);
		return nullptr;
	}

	const bool bConnected = bForce
		? WebFrameMcp::ConnectMaterialExpressionsForce(FromExpression, FromOutputName, ToExpression, ToInputName)
		: WebFrameMcp::ConnectMaterialExpressionsCompat(FromExpression, FromOutputName, ToExpression, ToInputName);
	if (!bConnected)
	{
		OutError = FString::Printf(TEXT("Failed to connect material expressions: %s -> %s"), *FromExpressionName, *ToExpressionName);
		return nullptr;
	}

	Material->PostEditChange();
	Material->MarkPackageDirty();
	bool bCompile = true;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
	}
	if (bSave && Material->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Material->GetOutermost() }, false);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Material->GetPathName());
	Result->SetStringField(TEXT("fromExpression"), FromExpressionName);
	Result->SetStringField(TEXT("fromOutput"), FromOutputName);
	Result->SetStringField(TEXT("toExpression"), ToExpressionName);
	Result->SetStringField(TEXT("toInput"), ToInputName);
	Result->SetBoolField(TEXT("force"), bForce);
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(ToExpressionName, FString::Printf(TEXT("Connect %s.%s to %s.%s%s."), *FromExpressionName, *FromOutputName, *ToExpressionName, *ToInputName, bForce ? TEXT(" with force") : TEXT(""))));
	RecordBlueprintChange(Material->GetPathName(), TEXT("Connect material nodes"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::ConnectMaterialExpressionToProperty(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString FromExpressionName;
	FString FromOutputName;
	FString PropertyName;
	bool bForce = false;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("fromExpression"), FromExpressionName);
		Params->TryGetStringField(TEXT("fromOutput"), FromOutputName);
		Params->TryGetStringField(TEXT("property"), PropertyName);
		Params->TryGetBoolField(TEXT("force"), bForce);
	}
	if (AssetPath.IsEmpty() || FromExpressionName.IsEmpty() || PropertyName.IsEmpty())
	{
		OutError = TEXT("Missing params.asset, params.fromExpression, or params.property.");
		return nullptr;
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
	UMaterialExpression* FromExpression = WebFrameMcp::FindMaterialExpression(Material, FromExpressionName);
	if (!Material || !FromExpression)
	{
		OutError = FString::Printf(TEXT("Material or expression not found: %s / %s"), *AssetPath, *FromExpressionName);
		return nullptr;
	}

	EMaterialProperty Property = MP_BaseColor;
	if (!WebFrameMcp::ParseMaterialProperty(PropertyName, Property))
	{
		OutError = FString::Printf(TEXT("Unknown material property: %s"), *PropertyName);
		return nullptr;
	}

	const bool bConnected = bForce
		? WebFrameMcp::ConnectMaterialPropertyForce(Material, FromExpression, FromOutputName, Property)
		: UMaterialEditingLibrary::ConnectMaterialProperty(FromExpression, FromOutputName, Property);
	if (!bConnected)
	{
		OutError = FString::Printf(TEXT("Failed to connect material expression to property: %s -> %s"), *FromExpressionName, *PropertyName);
		return nullptr;
	}

	Material->PostEditChange();
	Material->MarkPackageDirty();
	bool bCompile = true;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
	}
	if (bSave && Material->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Material->GetOutermost() }, false);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Material->GetPathName());
	Result->SetStringField(TEXT("fromExpression"), FromExpressionName);
	Result->SetStringField(TEXT("fromOutput"), FromOutputName);
	Result->SetStringField(TEXT("property"), PropertyName);
	Result->SetBoolField(TEXT("force"), bForce);
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(PropertyName, FString::Printf(TEXT("Connect %s.%s to material property%s."), *FromExpressionName, *FromOutputName, bForce ? TEXT(" with force") : TEXT(""))));
	RecordBlueprintChange(Material->GetPathName(), TEXT("Connect material property"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::DuplicateMaterialExpressionNode(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString ExpressionName;
	double X = 0.0;
	double Y = 0.0;
	bool bHasX = false;
	bool bHasY = false;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("expression"), ExpressionName);
		bHasX = Params->TryGetNumberField(TEXT("x"), X);
		bHasY = Params->TryGetNumberField(TEXT("y"), Y);
	}
	if (AssetPath.IsEmpty() || ExpressionName.IsEmpty())
	{
		OutError = TEXT("Missing params.asset or params.expression.");
		return nullptr;
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
	UMaterialExpression* Expression = WebFrameMcp::FindMaterialExpression(Material, ExpressionName);
	if (!Material || !Expression)
	{
		OutError = FString::Printf(TEXT("Material or expression not found: %s / %s"), *AssetPath, *ExpressionName);
		return nullptr;
	}

	Material->Modify();
	Expression->Modify();
	UMaterialExpression* DuplicatedExpression = UMaterialEditingLibrary::DuplicateMaterialExpression(Material, nullptr, Expression);
	if (!DuplicatedExpression)
	{
		OutError = FString::Printf(TEXT("Failed to duplicate material expression: %s"), *ExpressionName);
		return nullptr;
	}
	DuplicatedExpression->Modify();
	if (bHasX)
	{
		DuplicatedExpression->MaterialExpressionEditorX = FMath::RoundToInt(X);
	}
	else
	{
		DuplicatedExpression->MaterialExpressionEditorX = Expression->MaterialExpressionEditorX + 220;
	}
	if (bHasY)
	{
		DuplicatedExpression->MaterialExpressionEditorY = FMath::RoundToInt(Y);
	}
	else
	{
		DuplicatedExpression->MaterialExpressionEditorY = Expression->MaterialExpressionEditorY;
	}

	Material->PostEditChange();
	Material->MarkPackageDirty();
	bool bCompile = true;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
	}
	if (bSave && Material->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Material->GetOutermost() }, false);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Material->GetPathName());
	Result->SetStringField(TEXT("sourceExpression"), Expression->GetName());
	Result->SetStringField(TEXT("expression"), DuplicatedExpression->GetName());
	Result->SetStringField(TEXT("class"), DuplicatedExpression->GetClass()->GetPathName());
	Result->SetNumberField(TEXT("x"), DuplicatedExpression->MaterialExpressionEditorX);
	Result->SetNumberField(TEXT("y"), DuplicatedExpression->MaterialExpressionEditorY);
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(DuplicatedExpression->GetName(), FString::Printf(TEXT("Duplicated material expression from %s."), *ExpressionName)));
	RecordBlueprintChange(Material->GetPathName(), TEXT("Duplicate material expression node"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::LayoutMaterialExpressionNodes(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
	if (!Material)
	{
		OutError = FString::Printf(TEXT("Material not found: %s"), *AssetPath);
		return nullptr;
	}

	const int32 ExpressionCount = Material->GetExpressions().Num();
	Material->Modify();
	UMaterialEditingLibrary::LayoutMaterialExpressions(Material);
	Material->PostEditChange();
	Material->MarkPackageDirty();

	bool bCompile = false;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
	}
	if (bSave && Material->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Material->GetOutermost() }, false);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Material->GetPathName());
	Result->SetNumberField(TEXT("expressionCount"), ExpressionCount);
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(Material->GetName(), FString::Printf(TEXT("Laid out %d material expression node(s)."), ExpressionCount)));
	RecordBlueprintChange(Material->GetPathName(), TEXT("Layout material expression nodes"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::MoveMaterialExpressionNode(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString ExpressionName;
	double X = 0.0;
	double Y = 0.0;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("expression"), ExpressionName);
		Params->TryGetNumberField(TEXT("x"), X);
		Params->TryGetNumberField(TEXT("y"), Y);
	}
	if (AssetPath.IsEmpty() || ExpressionName.IsEmpty())
	{
		OutError = TEXT("Missing params.asset or params.expression.");
		return nullptr;
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
	UMaterialExpression* Expression = WebFrameMcp::FindMaterialExpression(Material, ExpressionName);
	if (!Material || !Expression)
	{
		OutError = FString::Printf(TEXT("Material or expression not found: %s / %s"), *AssetPath, *ExpressionName);
		return nullptr;
	}

	const int32 OldX = Expression->MaterialExpressionEditorX;
	const int32 OldY = Expression->MaterialExpressionEditorY;
	Material->Modify();
	Expression->Modify();
	Expression->MaterialExpressionEditorX = FMath::RoundToInt(X);
	Expression->MaterialExpressionEditorY = FMath::RoundToInt(Y);
	Material->PostEditChange();
	Material->MarkPackageDirty();

	bool bCompile = false;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
	}
	if (bSave && Material->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Material->GetOutermost() }, false);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Material->GetPathName());
	Result->SetStringField(TEXT("expression"), Expression->GetName());
	Result->SetNumberField(TEXT("oldX"), OldX);
	Result->SetNumberField(TEXT("oldY"), OldY);
	Result->SetNumberField(TEXT("x"), Expression->MaterialExpressionEditorX);
	Result->SetNumberField(TEXT("y"), Expression->MaterialExpressionEditorY);
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(Expression->GetName(), FString::Printf(TEXT("Move material expression from (%d, %d) to (%d, %d)."), OldX, OldY, Expression->MaterialExpressionEditorX, Expression->MaterialExpressionEditorY)));
	RecordBlueprintChange(Material->GetPathName(), TEXT("Move material expression node"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::DeleteMaterialExpressionNode(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString ExpressionName;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("expression"), ExpressionName);
	}
	if (AssetPath.IsEmpty() || ExpressionName.IsEmpty())
	{
		OutError = TEXT("Missing params.asset or params.expression.");
		return nullptr;
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
	UMaterialExpression* Expression = WebFrameMcp::FindMaterialExpression(Material, ExpressionName);
	if (!Material || !Expression)
	{
		OutError = FString::Printf(TEXT("Material or expression not found: %s / %s"), *AssetPath, *ExpressionName);
		return nullptr;
	}

	const FString ExpressionClass = Expression->GetClass()->GetName();
	Material->Modify();
	Expression->Modify();
	UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
	Material->PostEditChange();
	Material->MarkPackageDirty();

	bool bCompile = true;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
	}
	if (bSave && Material->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Material->GetOutermost() }, false);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Material->GetPathName());
	Result->SetStringField(TEXT("expression"), ExpressionName);
	Result->SetStringField(TEXT("class"), ExpressionClass);
	Result->SetBoolField(TEXT("deleted"), true);
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(ExpressionName, FString::Printf(TEXT("Deleted material expression node %s."), *ExpressionClass)));
	RecordBlueprintChange(Material->GetPathName(), TEXT("Delete material expression node"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::DisconnectMaterialExpressionNode(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString ExpressionName;
	FString InputName;
	FString FromExpressionName;
	FString FromOutputName;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("expression"), ExpressionName);
		Params->TryGetStringField(TEXT("input"), InputName);
		Params->TryGetStringField(TEXT("fromExpression"), FromExpressionName);
		Params->TryGetStringField(TEXT("fromOutput"), FromOutputName);
	}
	if (AssetPath.IsEmpty() || ExpressionName.IsEmpty())
	{
		OutError = TEXT("Missing params.asset or params.expression.");
		return nullptr;
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
	UMaterialExpression* Expression = WebFrameMcp::FindMaterialExpression(Material, ExpressionName);
	UMaterialExpression* FromExpression = FromExpressionName.IsEmpty() ? nullptr : WebFrameMcp::FindMaterialExpression(Material, FromExpressionName);
	if (!Material || !Expression)
	{
		OutError = FString::Printf(TEXT("Material or expression not found: %s / %s"), *AssetPath, *ExpressionName);
		return nullptr;
	}
	if (!FromExpressionName.IsEmpty() && !FromExpression)
	{
		OutError = FString::Printf(TEXT("Source material expression not found: %s"), *FromExpressionName);
		return nullptr;
	}

	TArray<FString> DisconnectedInputs;
	Material->Modify();
	Expression->Modify();
	if (!InputName.IsEmpty())
	{
		FExpressionInput* Input = WebFrameMcp::FindMaterialExpressionInputByMemberName(Expression, InputName);
		if (!Input)
		{
			OutError = FString::Printf(TEXT("Material expression input not found: %s.%s"), *ExpressionName, *InputName);
			return nullptr;
		}
		if (WebFrameMcp::DoesExpressionInputMatch(Input, FromExpression, FromOutputName))
		{
			*Input = FExpressionInput();
			DisconnectedInputs.Add(InputName);
		}
	}
	else
	{
		WebFrameMcp::ForEachMaterialExpressionInput(Expression, [&DisconnectedInputs, FromExpression, FromOutputName](const FString& CurrentInputName, FExpressionInput* Input)
		{
			if (WebFrameMcp::DoesExpressionInputMatch(Input, FromExpression, FromOutputName))
			{
				*Input = FExpressionInput();
				DisconnectedInputs.Add(CurrentInputName);
			}
		});
	}

	Material->PostEditChange();
	Material->MarkPackageDirty();
	bool bCompile = true;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
	}
	if (bSave && Material->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Material->GetOutermost() }, false);
	}

	TArray<TSharedPtr<FJsonValue>> InputValues;
	for (const FString& DisconnectedInput : DisconnectedInputs)
	{
		InputValues.Add(MakeShared<FJsonValueString>(DisconnectedInput));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Material->GetPathName());
	Result->SetStringField(TEXT("expression"), Expression->GetName());
	Result->SetArrayField(TEXT("disconnectedInputs"), InputValues);
	Result->SetNumberField(TEXT("disconnectedCount"), DisconnectedInputs.Num());
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(Expression->GetName(), FString::Printf(TEXT("Disconnected %d material expression input(s)."), DisconnectedInputs.Num())));
	RecordBlueprintChange(Material->GetPathName(), TEXT("Disconnect material expression inputs"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::DisconnectMaterialExpressionFromProperty(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString PropertyName;
	FString FromExpressionName;
	FString FromOutputName;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("property"), PropertyName);
		Params->TryGetStringField(TEXT("fromExpression"), FromExpressionName);
		Params->TryGetStringField(TEXT("fromOutput"), FromOutputName);
	}
	if (AssetPath.IsEmpty() || PropertyName.IsEmpty())
	{
		OutError = TEXT("Missing params.asset or params.property.");
		return nullptr;
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
	if (!Material)
	{
		OutError = FString::Printf(TEXT("Material not found: %s"), *AssetPath);
		return nullptr;
	}

	EMaterialProperty Property = MP_BaseColor;
	if (!WebFrameMcp::ParseMaterialProperty(PropertyName, Property))
	{
		OutError = FString::Printf(TEXT("Unknown material property: %s"), *PropertyName);
		return nullptr;
	}

	UMaterialExpression* FromExpression = FromExpressionName.IsEmpty() ? nullptr : WebFrameMcp::FindMaterialExpression(Material, FromExpressionName);
	if (!FromExpressionName.IsEmpty() && !FromExpression)
	{
		OutError = FString::Printf(TEXT("Source material expression not found: %s"), *FromExpressionName);
		return nullptr;
	}

	FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
	if (!Input)
	{
		OutError = FString::Printf(TEXT("Material property does not expose an expression input: %s"), *PropertyName);
		return nullptr;
	}

	const FString OldExpression = Input->Expression ? Input->Expression->GetName() : FString();
	bool bDisconnected = false;
	if (WebFrameMcp::DoesExpressionInputMatch(Input, FromExpression, FromOutputName))
	{
		Material->Modify();
		*Input = FExpressionInput();
		bDisconnected = true;
		Material->PostEditChange();
		Material->MarkPackageDirty();
	}

	bool bCompile = true;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("compile"), bCompile);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (bCompile)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
	}
	if (bSave && Material->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Material->GetOutermost() }, false);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Material->GetPathName());
	Result->SetStringField(TEXT("property"), PropertyName);
	Result->SetStringField(TEXT("oldExpression"), OldExpression);
	Result->SetBoolField(TEXT("disconnected"), bDisconnected);
	Result->SetBoolField(TEXT("compiled"), bCompile);
	Result->SetBoolField(TEXT("saved"), bSave);

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(PropertyName, bDisconnected ? FString::Printf(TEXT("Disconnected material property from %s."), *OldExpression) : TEXT("No matching material property connection found.")));
	RecordBlueprintChange(Material->GetPathName(), TEXT("Disconnect material property"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::CompileMaterialAsset(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	bool bSave = false;
	double LookbackSeconds = 600.0;
	int32 MaxLines = 80;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetBoolField(TEXT("save"), bSave);
		Params->TryGetNumberField(TEXT("lookbackSeconds"), LookbackSeconds);
		int32 JsonMaxLines = MaxLines;
		if (Params->TryGetNumberField(TEXT("maxLines"), JsonMaxLines))
		{
			MaxLines = JsonMaxLines;
		}
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
	if (!Material)
	{
		OutError = FString::Printf(TEXT("Material not found: %s"), *AssetPath);
		return nullptr;
	}

	Material->PostEditChange();
	Material->MarkPackageDirty();
	UMaterialEditingLibrary::RecompileMaterial(Material);
	if (bSave && Material->GetOutermost())
	{
		UEditorLoadingAndSavingUtils::SavePackages({ Material->GetOutermost() }, false);
	}
	if (GLog)
	{
		GLog->Flush();
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Material->GetPathName());
	Result->SetBoolField(TEXT("compiled"), true);
	Result->SetBoolField(TEXT("saved"), bSave);
	Result->SetObjectField(TEXT("diagnostics"), WebFrameMcp::MaterialDiagnosticsFromLog(Material->GetPathName(), LookbackSeconds, MaxLines));

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(Material->GetName(), bSave ? TEXT("Recompile and save material.") : TEXT("Recompile material.")));
	RecordBlueprintChange(Material->GetPathName(), TEXT("Compile material"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::GetMaterialDiagnostics(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	double LookbackSeconds = 600.0;
	int32 MaxLines = 80;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetNumberField(TEXT("lookbackSeconds"), LookbackSeconds);
		int32 JsonMaxLines = MaxLines;
		if (Params->TryGetNumberField(TEXT("maxLines"), JsonMaxLines))
		{
			MaxLines = JsonMaxLines;
		}
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}

	TSharedPtr<FJsonObject> Result = WebFrameMcp::MaterialDiagnosticsFromLog(AssetPath, LookbackSeconds, MaxLines);
	Result->SetStringField(TEXT("asset"), AssetPath);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::DescribeMeshAsset(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}

	if (UStaticMesh* StaticMesh = LoadObject<UStaticMesh>(nullptr, *AssetPath))
	{
		TArray<TSharedPtr<FJsonValue>> Materials;
		const TArray<FStaticMaterial>& StaticMaterials = StaticMesh->GetStaticMaterials();
		for (int32 Index = 0; Index < StaticMaterials.Num(); ++Index)
		{
			const FStaticMaterial& MaterialSlot = StaticMaterials[Index];
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("index"), Index);
			Item->SetStringField(TEXT("slotName"), MaterialSlot.MaterialSlotName.ToString());
			Item->SetStringField(TEXT("material"), MaterialSlot.MaterialInterface ? MaterialSlot.MaterialInterface->GetPathName() : FString());
			Materials.Add(MakeShared<FJsonValueObject>(Item));
		}

		TArray<TSharedPtr<FJsonValue>> Sockets;
		for (const UStaticMeshSocket* Socket : StaticMesh->Sockets)
		{
			Sockets.Add(MakeShared<FJsonValueObject>(WebFrameMcp::StaticMeshSocketToJson(Socket)));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset"), StaticMesh->GetPathName());
		Result->SetStringField(TEXT("type"), TEXT("StaticMesh"));
		Result->SetNumberField(TEXT("lodCount"), StaticMesh->GetNumLODs());
		Result->SetObjectField(TEXT("bounds"), WebFrameMcp::BoundsToJson(StaticMesh->GetBounds()));
		Result->SetArrayField(TEXT("materials"), Materials);
		Result->SetArrayField(TEXT("sockets"), Sockets);
		return Result;
	}

	if (USkeletalMesh* SkeletalMesh = LoadObject<USkeletalMesh>(nullptr, *AssetPath))
	{
		TArray<TSharedPtr<FJsonValue>> Materials;
		const TArray<FSkeletalMaterial>& SkeletalMaterials = SkeletalMesh->GetMaterials();
		for (int32 Index = 0; Index < SkeletalMaterials.Num(); ++Index)
		{
			const FSkeletalMaterial& MaterialSlot = SkeletalMaterials[Index];
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("index"), Index);
			Item->SetStringField(TEXT("slotName"), MaterialSlot.MaterialSlotName.ToString());
			Item->SetStringField(TEXT("material"), MaterialSlot.MaterialInterface ? MaterialSlot.MaterialInterface->GetPathName() : FString());
			Materials.Add(MakeShared<FJsonValueObject>(Item));
		}

		TArray<TSharedPtr<FJsonValue>> BoneNames;
		const FReferenceSkeleton& ReferenceSkeleton = SkeletalMesh->GetRefSkeleton();
		for (int32 Index = 0; Index < ReferenceSkeleton.GetNum(); ++Index)
		{
			BoneNames.Add(MakeShared<FJsonValueString>(ReferenceSkeleton.GetBoneName(Index).ToString()));
		}

		TArray<TSharedPtr<FJsonValue>> Sockets;
		if (USkeleton* Skeleton = SkeletalMesh->GetSkeleton())
		{
			for (const USkeletalMeshSocket* Socket : Skeleton->Sockets)
			{
				Sockets.Add(MakeShared<FJsonValueObject>(WebFrameMcp::SkeletalMeshSocketToJson(Socket, TEXT("Skeleton"))));
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset"), SkeletalMesh->GetPathName());
		Result->SetStringField(TEXT("type"), TEXT("SkeletalMesh"));
		Result->SetStringField(TEXT("skeleton"), SkeletalMesh->GetSkeleton() ? SkeletalMesh->GetSkeleton()->GetPathName() : FString());
		Result->SetNumberField(TEXT("lodCount"), SkeletalMesh->GetLODNum());
		Result->SetObjectField(TEXT("bounds"), WebFrameMcp::BoundsToJson(SkeletalMesh->GetBounds()));
		Result->SetArrayField(TEXT("materials"), Materials);
		Result->SetArrayField(TEXT("bones"), BoneNames);
		Result->SetArrayField(TEXT("sockets"), Sockets);
		return Result;
	}

	OutError = FString::Printf(TEXT("Mesh asset not found or unsupported: %s"), *AssetPath);
	return nullptr;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::DescribeMeshGeometry(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	int32 LodIndex = 0;
	bool bIncludeVertices = false;
	bool bIncludeTriangles = false;
	int32 VertexLimit = 0;
	int32 TriangleLimit = 0;
	FVector RadialOrigin = FVector::ZeroVector;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetNumberField(TEXT("lodIndex"), LodIndex);
		Params->TryGetBoolField(TEXT("includeVertices"), bIncludeVertices);
		Params->TryGetBoolField(TEXT("includeTriangles"), bIncludeTriangles);
		Params->TryGetNumberField(TEXT("vertexLimit"), VertexLimit);
		Params->TryGetNumberField(TEXT("triangleLimit"), TriangleLimit);
		WebFrameMcp::TryReadVectorArray(Params, TEXT("origin"), RadialOrigin);
		WebFrameMcp::TryReadVectorArray(Params, TEXT("radialOrigin"), RadialOrigin);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}
	LodIndex = FMath::Max(0, LodIndex);
	VertexLimit = FMath::Clamp(VertexLimit, 0, 100000);
	TriangleLimit = FMath::Clamp(TriangleLimit, 0, 100000);

	UStaticMesh* StaticMesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
	if (!StaticMesh)
	{
		if (LoadObject<USkeletalMesh>(nullptr, *AssetPath))
		{
			OutError = TEXT("mesh.geometry.describe currently supports StaticMesh assets only. SkeletalMesh skinned geometry is not implemented.");
		}
		else
		{
			OutError = FString::Printf(TEXT("StaticMesh asset not found: %s"), *AssetPath);
		}
		return nullptr;
	}

	const FStaticMeshLODResources* LodResource = nullptr;
	if (!WebFrameMcp::ReadStaticMeshLod(StaticMesh, LodIndex, LodResource, OutError))
	{
		return nullptr;
	}

	const int32 VertexCount = LodResource->GetNumVertices();
	const FIndexArrayView Indices = LodResource->IndexBuffer.GetArrayView();
	const int32 TriangleCount = Indices.Num() / 3;
	TArray<TSharedPtr<FJsonValue>> VertexSamples;
	TArray<TSharedPtr<FJsonValue>> TriangleSamples;
	WebFrameMcp::FGeometryRadialBounds RadialBounds;
	WebFrameMcp::AddGeometrySamples(
		*LodResource,
		FTransform::Identity,
		RadialOrigin,
		bIncludeVertices,
		VertexLimit,
		bIncludeTriangles,
		TriangleLimit,
		FString(),
		RadialBounds,
		bIncludeVertices ? &VertexSamples : nullptr,
		bIncludeTriangles ? &TriangleSamples : nullptr);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), StaticMesh->GetPathName());
	Result->SetStringField(TEXT("type"), TEXT("StaticMesh"));
	Result->SetNumberField(TEXT("lodIndex"), LodIndex);
	Result->SetNumberField(TEXT("lodCount"), StaticMesh->GetNumLODs());
	Result->SetNumberField(TEXT("vertexCount"), VertexCount);
	Result->SetNumberField(TEXT("triangleCount"), TriangleCount);
	Result->SetObjectField(TEXT("bounds"), WebFrameMcp::BoundsToJson(StaticMesh->GetBounds()));
	Result->SetObjectField(TEXT("radialBounds"), WebFrameMcp::RadialBoundsToJson(RadialBounds, RadialOrigin));
	Result->SetStringField(TEXT("space"), TEXT("asset-local"));
	if (bIncludeVertices)
	{
		Result->SetArrayField(TEXT("vertices"), VertexSamples);
		Result->SetNumberField(TEXT("vertexSampleCount"), VertexSamples.Num());
		Result->SetNumberField(TEXT("vertexLimit"), VertexLimit);
	}
	if (bIncludeTriangles)
	{
		Result->SetArrayField(TEXT("triangles"), TriangleSamples);
		Result->SetNumberField(TEXT("triangleSampleCount"), TriangleSamples.Num());
		Result->SetNumberField(TEXT("triangleLimit"), TriangleLimit);
	}
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::SetMeshMaterialSlot(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString MaterialPath;
	FString SlotName;
	int32 SlotIndex = INDEX_NONE;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("material"), MaterialPath);
		Params->TryGetStringField(TEXT("slotName"), SlotName);
		Params->TryGetNumberField(TEXT("slotIndex"), SlotIndex);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}
	if (MaterialPath.IsEmpty())
	{
		OutError = TEXT("Missing params.material.");
		return nullptr;
	}

	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!Material)
	{
		OutError = FString::Printf(TEXT("Material not found: %s"), *MaterialPath);
		return nullptr;
	}

	FString OldMaterial;
	FString MeshType;
	if (UStaticMesh* StaticMesh = LoadObject<UStaticMesh>(nullptr, *AssetPath))
	{
		if (SlotIndex == INDEX_NONE && !SlotName.IsEmpty())
		{
			SlotIndex = StaticMesh->GetMaterialIndex(*SlotName);
		}
		if (!StaticMesh->GetStaticMaterials().IsValidIndex(SlotIndex))
		{
			OutError = FString::Printf(TEXT("StaticMesh material slot not found: %s"), *SlotName);
			return nullptr;
		}
		OldMaterial = StaticMesh->GetMaterial(SlotIndex) ? StaticMesh->GetMaterial(SlotIndex)->GetPathName() : FString();
		StaticMesh->Modify();
		StaticMesh->SetMaterial(SlotIndex, Material);
		StaticMesh->PostEditChange();
		WebFrameMcp::SaveAssetObject(StaticMesh, bSave);
		MeshType = TEXT("StaticMesh");
	}
	else if (USkeletalMesh* SkeletalMesh = LoadObject<USkeletalMesh>(nullptr, *AssetPath))
	{
		TArray<FSkeletalMaterial>& Materials = SkeletalMesh->GetMaterials();
		if (SlotIndex == INDEX_NONE && !SlotName.IsEmpty())
		{
			for (int32 Index = 0; Index < Materials.Num(); ++Index)
			{
				if (Materials[Index].MaterialSlotName == *SlotName)
				{
					SlotIndex = Index;
					break;
				}
			}
		}
		if (!Materials.IsValidIndex(SlotIndex))
		{
			OutError = FString::Printf(TEXT("SkeletalMesh material slot not found: %s"), *SlotName);
			return nullptr;
		}
		OldMaterial = Materials[SlotIndex].MaterialInterface ? Materials[SlotIndex].MaterialInterface->GetPathName() : FString();
		SkeletalMesh->Modify();
		Materials[SlotIndex].MaterialInterface = Material;
		SkeletalMesh->PostEditChange();
		WebFrameMcp::SaveAssetObject(SkeletalMesh, bSave);
		MeshType = TEXT("SkeletalMesh");
	}
	else
	{
		OutError = FString::Printf(TEXT("Mesh asset not found or unsupported: %s"), *AssetPath);
		return nullptr;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetStringField(TEXT("type"), MeshType);
	Result->SetNumberField(TEXT("slotIndex"), SlotIndex);
	Result->SetStringField(TEXT("oldMaterial"), OldMaterial);
	Result->SetStringField(TEXT("material"), Material->GetPathName());
	Result->SetBoolField(TEXT("saved"), bSave);

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(FString::Printf(TEXT("slot %d"), SlotIndex), FString::Printf(TEXT("Set %s material from %s to %s."), *MeshType, *OldMaterial, *Material->GetPathName())));
	RecordBlueprintChange(AssetPath, TEXT("Set mesh material slot"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::AddMeshSocket(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString SocketName;
	FString BoneName;
	bool bSave = false;
	bool bReplaceExisting = false;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("socketName"), SocketName);
		Params->TryGetStringField(TEXT("boneName"), BoneName);
		Params->TryGetBoolField(TEXT("save"), bSave);
		Params->TryGetBoolField(TEXT("replaceExisting"), bReplaceExisting);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}
	if (SocketName.IsEmpty())
	{
		OutError = TEXT("Missing params.socketName.");
		return nullptr;
	}

	const FVector RelativeLocation = WebFrameMcp::ReadVectorArray(Params, TEXT("relativeLocation"), FVector::ZeroVector);
	const FRotator RelativeRotation = WebFrameMcp::ReadRotatorArray(Params, TEXT("relativeRotation"), FRotator::ZeroRotator);
	const FVector RelativeScale = WebFrameMcp::ReadVectorArray(Params, TEXT("relativeScale"), FVector::OneVector);

	UObject* ModifiedAsset = nullptr;
	FString TargetAsset;
	FString TargetType;
	if (UStaticMesh* StaticMesh = LoadObject<UStaticMesh>(nullptr, *AssetPath))
	{
		UStaticMeshSocket* Socket = StaticMesh->FindSocket(*SocketName);
		if (Socket && !bReplaceExisting)
		{
			OutError = FString::Printf(TEXT("StaticMesh socket already exists: %s"), *SocketName);
			return nullptr;
		}
		StaticMesh->Modify();
		if (!Socket)
		{
			Socket = NewObject<UStaticMeshSocket>(StaticMesh);
			Socket->SocketName = *SocketName;
			StaticMesh->AddSocket(Socket);
		}
		Socket->Modify();
		Socket->RelativeLocation = RelativeLocation;
		Socket->RelativeRotation = RelativeRotation;
		Socket->RelativeScale = RelativeScale;
		StaticMesh->PostEditChange();
		WebFrameMcp::SaveAssetObject(StaticMesh, bSave);
		ModifiedAsset = StaticMesh;
		TargetAsset = StaticMesh->GetPathName();
		TargetType = TEXT("StaticMesh");
	}
	else if (USkeletalMesh* SkeletalMesh = LoadObject<USkeletalMesh>(nullptr, *AssetPath))
	{
		USkeleton* Skeleton = SkeletalMesh->GetSkeleton();
		if (!Skeleton)
		{
			OutError = FString::Printf(TEXT("SkeletalMesh has no Skeleton: %s"), *AssetPath);
			return nullptr;
		}
		if (BoneName.IsEmpty())
		{
			OutError = TEXT("Missing params.boneName for SkeletalMesh socket.");
			return nullptr;
		}
		if (Skeleton->GetReferenceSkeleton().FindBoneIndex(*BoneName) == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("Skeleton bone not found: %s"), *BoneName);
			return nullptr;
		}
		USkeletalMeshSocket* Socket = Skeleton->FindSocket(*SocketName);
		if (Socket && !bReplaceExisting)
		{
			OutError = FString::Printf(TEXT("Skeleton socket already exists: %s"), *SocketName);
			return nullptr;
		}
		Skeleton->Modify();
		if (!Socket)
		{
			Socket = NewObject<USkeletalMeshSocket>(Skeleton);
			Socket->SocketName = *SocketName;
			Skeleton->Sockets.Add(Socket);
		}
		Socket->Modify();
		Socket->BoneName = *BoneName;
		Socket->RelativeLocation = RelativeLocation;
		Socket->RelativeRotation = RelativeRotation;
		Socket->RelativeScale = RelativeScale;
		Skeleton->PostEditChange();
		WebFrameMcp::SaveAssetObject(Skeleton, bSave);
		ModifiedAsset = Skeleton;
		TargetAsset = Skeleton->GetPathName();
		TargetType = TEXT("Skeleton");
	}
	else
	{
		OutError = FString::Printf(TEXT("Mesh asset not found or unsupported: %s"), *AssetPath);
		return nullptr;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetStringField(TEXT("targetAsset"), TargetAsset);
	Result->SetStringField(TEXT("targetType"), TargetType);
	Result->SetStringField(TEXT("socketName"), SocketName);
	Result->SetStringField(TEXT("boneName"), BoneName);
	Result->SetBoolField(TEXT("saved"), bSave);

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(SocketName, FString::Printf(TEXT("Add %s socket on %s."), *TargetType, *TargetAsset)));
	RecordBlueprintChange(TargetAsset.IsEmpty() ? AssetPath : TargetAsset, TEXT("Add mesh socket"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::UpdateMeshSocket(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString SocketName;
	FString BoneName;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("socketName"), SocketName);
		Params->TryGetStringField(TEXT("boneName"), BoneName);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}
	if (SocketName.IsEmpty())
	{
		OutError = TEXT("Missing params.socketName.");
		return nullptr;
	}

	FString TargetAsset;
	FString TargetType;
	if (UStaticMesh* StaticMesh = LoadObject<UStaticMesh>(nullptr, *AssetPath))
	{
		UStaticMeshSocket* Socket = StaticMesh->FindSocket(*SocketName);
		if (!Socket)
		{
			OutError = FString::Printf(TEXT("StaticMesh socket not found: %s"), *SocketName);
			return nullptr;
		}
		StaticMesh->Modify();
		Socket->Modify();
		Socket->RelativeLocation = WebFrameMcp::ReadVectorArray(Params, TEXT("relativeLocation"), Socket->RelativeLocation);
		Socket->RelativeRotation = WebFrameMcp::ReadRotatorArray(Params, TEXT("relativeRotation"), Socket->RelativeRotation);
		Socket->RelativeScale = WebFrameMcp::ReadVectorArray(Params, TEXT("relativeScale"), Socket->RelativeScale);
		StaticMesh->PostEditChange();
		WebFrameMcp::SaveAssetObject(StaticMesh, bSave);
		TargetAsset = StaticMesh->GetPathName();
		TargetType = TEXT("StaticMesh");
	}
	else if (USkeletalMesh* SkeletalMesh = LoadObject<USkeletalMesh>(nullptr, *AssetPath))
	{
		USkeleton* Skeleton = SkeletalMesh->GetSkeleton();
		if (!Skeleton)
		{
			OutError = FString::Printf(TEXT("SkeletalMesh has no Skeleton: %s"), *AssetPath);
			return nullptr;
		}
		USkeletalMeshSocket* Socket = Skeleton->FindSocket(*SocketName);
		if (!Socket)
		{
			OutError = FString::Printf(TEXT("Skeleton socket not found: %s"), *SocketName);
			return nullptr;
		}
		if (!BoneName.IsEmpty())
		{
			if (Skeleton->GetReferenceSkeleton().FindBoneIndex(*BoneName) == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("Skeleton bone not found: %s"), *BoneName);
				return nullptr;
			}
			Socket->BoneName = *BoneName;
		}
		Skeleton->Modify();
		Socket->Modify();
		Socket->RelativeLocation = WebFrameMcp::ReadVectorArray(Params, TEXT("relativeLocation"), Socket->RelativeLocation);
		Socket->RelativeRotation = WebFrameMcp::ReadRotatorArray(Params, TEXT("relativeRotation"), Socket->RelativeRotation);
		Socket->RelativeScale = WebFrameMcp::ReadVectorArray(Params, TEXT("relativeScale"), Socket->RelativeScale);
		Skeleton->PostEditChange();
		WebFrameMcp::SaveAssetObject(Skeleton, bSave);
		TargetAsset = Skeleton->GetPathName();
		TargetType = TEXT("Skeleton");
	}
	else
	{
		OutError = FString::Printf(TEXT("Mesh asset not found or unsupported: %s"), *AssetPath);
		return nullptr;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetStringField(TEXT("targetAsset"), TargetAsset);
	Result->SetStringField(TEXT("targetType"), TargetType);
	Result->SetStringField(TEXT("socketName"), SocketName);
	Result->SetBoolField(TEXT("saved"), bSave);

	const FString BoneSuffix = BoneName.IsEmpty() ? FString() : FString::Printf(TEXT(" and bone %s"), *BoneName);
	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(SocketName, FString::Printf(TEXT("Update %s socket transform%s."), *TargetType, *BoneSuffix)));
	RecordBlueprintChange(TargetAsset.IsEmpty() ? AssetPath : TargetAsset, TEXT("Update mesh socket"), Details);
	return Result;
}

TSharedPtr<FJsonObject> FWebFrameMcpModule::RemoveMeshSocket(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FString AssetPath;
	FString SocketName;
	bool bSave = false;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		Params->TryGetStringField(TEXT("socketName"), SocketName);
		Params->TryGetBoolField(TEXT("save"), bSave);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing params.asset.");
		return nullptr;
	}
	if (SocketName.IsEmpty())
	{
		OutError = TEXT("Missing params.socketName.");
		return nullptr;
	}

	FString TargetAsset;
	FString TargetType;
	if (UStaticMesh* StaticMesh = LoadObject<UStaticMesh>(nullptr, *AssetPath))
	{
		UStaticMeshSocket* Socket = StaticMesh->FindSocket(*SocketName);
		if (!Socket)
		{
			OutError = FString::Printf(TEXT("StaticMesh socket not found: %s"), *SocketName);
			return nullptr;
		}
		StaticMesh->Modify();
		StaticMesh->RemoveSocket(Socket);
		StaticMesh->PostEditChange();
		WebFrameMcp::SaveAssetObject(StaticMesh, bSave);
		TargetAsset = StaticMesh->GetPathName();
		TargetType = TEXT("StaticMesh");
	}
	else if (USkeletalMesh* SkeletalMesh = LoadObject<USkeletalMesh>(nullptr, *AssetPath))
	{
		USkeleton* Skeleton = SkeletalMesh->GetSkeleton();
		if (!Skeleton)
		{
			OutError = FString::Printf(TEXT("SkeletalMesh has no Skeleton: %s"), *AssetPath);
			return nullptr;
		}
		int32 SocketIndex = INDEX_NONE;
		USkeletalMeshSocket* Socket = Skeleton->FindSocketAndIndex(*SocketName, SocketIndex);
		if (!Socket || SocketIndex == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("Skeleton socket not found: %s"), *SocketName);
			return nullptr;
		}
		Skeleton->Modify();
		Skeleton->Sockets.RemoveAt(SocketIndex);
		Skeleton->PostEditChange();
		WebFrameMcp::SaveAssetObject(Skeleton, bSave);
		TargetAsset = Skeleton->GetPathName();
		TargetType = TEXT("Skeleton");
	}
	else
	{
		OutError = FString::Printf(TEXT("Mesh asset not found or unsupported: %s"), *AssetPath);
		return nullptr;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetStringField(TEXT("targetAsset"), TargetAsset);
	Result->SetStringField(TEXT("targetType"), TargetType);
	Result->SetStringField(TEXT("socketName"), SocketName);
	Result->SetBoolField(TEXT("saved"), bSave);

	TArray<TPair<FString, FString>> Details;
	Details.Add(TPair<FString, FString>(SocketName, FString::Printf(TEXT("Remove %s socket."), *TargetType)));
	RecordBlueprintChange(TargetAsset.IsEmpty() ? AssetPath : TargetAsset, TEXT("Remove mesh socket"), Details);
	return Result;
}

void FWebFrameMcpModule::AddConsoleLine(const FString& Line)
{
	ConsoleLines.Add(Line);
	if (ConsoleLines.Num() > 1000)
	{
		ConsoleLines.RemoveAt(0, ConsoleLines.Num() - 1000);
	}

	if (PanelWidget.IsValid())
	{
		PanelWidget->AppendLine(Line);
	}
}

FString FWebFrameMcpModule::ResolveHourlyLogPath() const
{
	const FString LogDir = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("WebFrameMcp"), TEXT("Saved"), TEXT("Logs"));
	const FString FileName = FString::Printf(TEXT("WebFrameMcp-%s.log"), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H")));
	return FPaths::Combine(LogDir, FileName);
}

void FWebFrameMcpModule::WriteExternalCommandLog(const FString& Line)
{
	const FString HourlyLogPath = ResolveHourlyLogPath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(HourlyLogPath), true);
	FFileHelper::SaveStringToFile(Line + LINE_TERMINATOR, *HourlyLogPath, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FWebFrameMcpModule, WebFrameMcp)

