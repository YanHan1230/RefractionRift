#if WITH_DEV_AUTOMATION_TESTS

#include "MaterialChineseCatalog.h"
#include "MaterialChinesePresentation.h"
#include "MaterialChineseSearch.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraphPin.h"
#include "Framework/Application/SlateApplication.h"
#include "Interfaces/IPluginManager.h"
#include "Layout/Children.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionFresnel.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "SGraphActionMenu.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "SGraphNode.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Text/SInlineEditableTextBlock.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
TSharedRef<FMCHCatalog> LoadCatalog(FAutomationTestBase& Test)
{
    auto Catalog = MakeShared<FMCHCatalog>();
    FString Error;
    Test.TestTrue(TEXT("Dictionary loads"), Catalog->Load(FPaths::Combine(
        IPluginManager::Get().FindPlugin(TEXT("MaterialChineseHelper"))->GetBaseDir(),
        TEXT("Resources/NodeDictionary.json")), Error));
    if (!Error.IsEmpty()) Test.AddError(Error);
    return Catalog;
}

void CollectText(const TSharedRef<SWidget>& Widget, TArray<FString>& Out)
{
    if (Widget->GetType() == FName(TEXT("SInlineEditableTextBlock")))
    {
        Out.Add(StaticCastSharedRef<SInlineEditableTextBlock>(Widget)->GetText().ToString());
        return;
    }
    if (Widget->GetType() == FName(TEXT("STextBlock")))
    {
        Out.Add(StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString());
        return;
    }
    FChildren* Children = Widget->GetChildren();
    if (Children)
        for (int32 I = 0; I < Children->Num(); ++I) CollectText(Children->GetChildAt(I), Out);
}

bool Contains(const TSharedRef<SWidget>& Widget, const FString& Needle)
{
    TArray<FString> Texts;
    CollectText(Widget, Texts);
    return Texts.ContainsByPredicate([&](const FString& Value) { return Value.Contains(Needle); });
}

struct FGraphFixture
{
    UMaterial* Material = NewObject<UMaterial>();
    UMaterialGraph* Graph = NewObject<UMaterialGraph>(Material);
    FGraphFixture()
    {
        Graph->Material = Material;
        Graph->Schema = UMaterialGraphSchema::StaticClass();
    }
    template<typename T>
    UMaterialGraphNode* MakeNode()
    {
        T* Expression = NewObject<T>(Material);
        Expression->Material = Material;
        Expression->bHidePreviewWindow = true;
        auto* Node = NewObject<UMaterialGraphNode>(Graph);
        Node->MaterialExpression = Expression;
        Graph->AddNode(Node, false, false);
        Node->PostPlacedNewNode();
        Node->AllocateDefaultPins();
        return Node;
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCHCatalogTest, "MaterialChineseHelper.DictionaryAndClasses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMCHCatalogTest::RunTest(const FString&)
{
    auto Catalog = LoadCatalog(*this);
    TestTrue(TEXT("At least 100 native material nodes"), Catalog->Entries.Num() >= 100);
    for (const auto& Entry : Catalog->Entries)
    {
        if (Entry->ClassName.StartsWith(TEXT("Function:"))) continue;
        const FString ClassPath = TEXT("/Script/Engine.") + Entry->ClassName;
        UClass* Class = FindObject<UClass>(nullptr, *ClassPath);
        if (!Class) Class = FindObject<UClass>(nullptr, *(TEXT("/Script/Landscape.") + Entry->ClassName));
        TestNotNull(*ClassPath, Class);
        TestFalse(TEXT("Learning description exists"), Entry->Description.IsEmpty());
    }
    TestEqual(TEXT("Fresnel title"), Catalog->TranslateTitle(TEXT("MaterialExpressionFresnel"), TEXT("Fresnel"), true),
        FString(TEXT("菲涅耳·视角遮罩 (Fresnel)")));
    TestEqual(TEXT("Numeric values preserved"), Catalog->TranslateTitle(TEXT("MaterialExpressionConstant"), TEXT("0.18"), false),
        FString(TEXT("标量常量 · 0.18")));
    TestEqual(TEXT("Unknown node unchanged"), Catalog->TranslateTitle(TEXT("MyCustomExpression"), TEXT("User Title"), true),
        FString(TEXT("User Title")));
    TestEqual(TEXT("Unknown custom pin unchanged"), Catalog->TranslatePin(TEXT("MaterialExpressionCustom"), TEXT("UserUV"), TEXT("UserUV"), true),
        FString(TEXT("UserUV")));
    TestNotNull(TEXT("Fresnel exponent explanation"), Catalog->FindPin(TEXT("MaterialExpressionFresnel"), TEXT("ExponentIn")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCHNativeMenuTest, "MaterialChineseHelper.NativeChineseSearch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMCHNativeMenuTest::RunTest(const FString&)
{
    auto Catalog = LoadCatalog(*this);
    auto Action = MakeShared<FMaterialGraphSchemaAction_NewNode>(FText::FromString(TEXT("Math")),
        FText::FromString(TEXT("Fresnel")), FText::FromString(TEXT("Original tooltip")), 0, FText::FromString(TEXT("OriginalKeyword")));
    Action->MaterialExpressionClass = UMaterialExpressionFresnel::StaticClass();
    auto Menu = SNew(SGraphActionMenu).OnCollectAllActions_Lambda([Action](FGraphActionListBuilderBase& List) { List.AddAction(Action); });
    // Simulate typing Chinese before the plugin first sees the popup.
    Menu->GetFilterTextBox()->SetText(FText::FromString(TEXT("菲涅耳")));
    FMCHNativeSearch Search(Catalog);
    TestTrue(TEXT("Native menu actions patched"), Search.PatchMenu(Menu));
    const FString Translated = Action->GetMenuDescription().ToString();
    TestTrue(TEXT("Chinese action name"), Translated.Contains(TEXT("菲涅耳")));
    TestTrue(TEXT("Chinese query produces a selectable native result"), Menu->SelectItemByName(FName(*Translated)));
    TArray<TSharedPtr<FEdGraphSchemaAction>> Selected;
    Menu->GetSelectedActions(Selected);
    TestTrue(TEXT("Original node creation action is retained"), Selected.Num() == 1 && Selected[0] == Action);
    TestEqual(TEXT("User query unchanged"), Menu->GetFilterTextBox()->GetText().ToString(), FString(TEXT("菲涅耳")));
    Menu->GetFilterTextBox()->SetText(FText::FromString(TEXT("Fresnel")));
    Menu->GenerateFilteredItems(false);
    TestTrue(TEXT("English search retained"), Menu->SelectItemByName(FName(*Translated)));
    Menu->GetFilterTextBox()->SetText(FText::FromString(TEXT("不存在的查询_xyz")));
    Menu->GenerateFilteredItems(false);
    TestFalse(TEXT("Unmatched query has no result"), Menu->SelectItemByName(FName(*Translated)));
    TestFalse(TEXT("Repeated patch is idempotent"), Search.PatchMenu(Menu));
    Search.Restore();
    TestEqual(TEXT("Original native action restored"), Action->GetMenuDescription().ToString(), FString(TEXT("Fresnel")));
    TestEqual(TEXT("Original keywords restored"), Action->GetKeywords().ToString(), FString(TEXT("OriginalKeyword")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCHInventoryTest, "MaterialChineseHelper.NativeCoverageInventory",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMCHInventoryTest::RunTest(const FString&)
{
    auto Catalog = LoadCatalog(*this);
    FGraphFixture Fixture;
    TArray<TSharedPtr<FJsonValue>> Classes, ActionNames, Functions;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        if (!Class->IsChildOf(UMaterialExpression::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated)) continue;
        TestNotNull(*FString::Printf(TEXT("Concrete native class is translated: %s"), *Class->GetName()), Catalog->Find(Class->GetName()));
        auto Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("Class"), Class->GetName());
        Object->SetStringField(TEXT("Path"), Class->GetPathName());
        Object->SetStringField(TEXT("DisplayName"), Class->GetDisplayNameText().ToString());
        TArray<TSharedPtr<FJsonValue>> Pins;
        for (TFieldIterator<FProperty> P(Class); P; ++P)
        {
            if (const auto* Struct = CastField<FStructProperty>(*P))
                if (Struct->Struct->GetName() == TEXT("ExpressionInput"))
                    Pins.Add(MakeShared<FJsonValueString>(P->GetName()));
        }
        Object->SetArrayField(TEXT("Pins"), Pins);
        Classes.Add(MakeShared<FJsonValueObject>(Object));
    }
    FGraphContextMenuBuilder Builder(Fixture.Graph);
    const UEdGraphSchema* Schema = GetDefault<UMaterialGraphSchema>();
    Schema->GetGraphContextActions(Builder);
    for (int32 I = 0; I < Builder.GetNumActions(); ++I)
    {
        const auto& Action = Builder.GetSchemaAction(I);
        if (Action->GetTypeId() == FMaterialGraphSchemaAction_NewNode::StaticGetTypeId())
        {
            auto Native = StaticCastSharedPtr<FMaterialGraphSchemaAction_NewNode>(Action);
            TestNotNull(TEXT("Native menu class has a translation"), Catalog->Find(Native->MaterialExpressionClass->GetName()));
            auto Object = MakeShared<FJsonObject>();
            Object->SetStringField(TEXT("Class"), Native->MaterialExpressionClass->GetName());
            Object->SetStringField(TEXT("Name"), Action->GetMenuDescription().ToString());
            Object->SetStringField(TEXT("Category"), Action->GetCategory().ToString());
            ActionNames.Add(MakeShared<FJsonValueObject>(Object));
        }
        if (Action->GetTypeId() == FMaterialGraphSchemaAction_NewFunctionCall::StaticGetTypeId())
        {
            auto Native = StaticCastSharedPtr<FMaterialGraphSchemaAction_NewFunctionCall>(Action);
            if (!Native->FunctionPath.StartsWith(TEXT("/Engine/"))) continue;
            const FString FunctionName = Native->FunctionPath.Mid(Native->FunctionPath.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd) + 1);
            TestNotNull(*FString::Printf(TEXT("Exposed engine function is translated: %s"), *FunctionName), Catalog->Find(TEXT("Function:") + FunctionName));
            auto Object = MakeShared<FJsonObject>();
            Object->SetStringField(TEXT("Name"), Action->GetMenuDescription().ToString());
            Object->SetStringField(TEXT("Path"), Native->FunctionPath);
            Object->SetStringField(TEXT("Category"), Action->GetCategory().ToString());
            Functions.Add(MakeShared<FJsonValueObject>(Object));
        }
    }
    auto Root = MakeShared<FJsonObject>();
    Root->SetArrayField(TEXT("Classes"), Classes);
    Root->SetArrayField(TEXT("Actions"), ActionNames);
    Root->SetArrayField(TEXT("Functions"), Functions);
    // Exercise the actual native material menu and engine action list, not a parallel search implementation.
    auto NativeMenu = SNew(SGraphActionMenu).OnCollectAllActions_Lambda([&Builder](FGraphActionListBuilderBase& List)
    {
        // Append moves its source entries; retain Builder for assertions and repeat collection.
        for (int32 I = 0; I < Builder.GetNumActions(); ++I) List.AddAction(Builder.GetSchemaAction(I));
    });
    FMCHNativeSearch Search(Catalog);
    TestTrue(TEXT("Full native material menu translated"), Search.PatchMenu(NativeMenu));
    NativeMenu->GetFilterTextBox()->SetText(FText::FromString(TEXT("菲涅耳")));
    NativeMenu->GenerateFilteredItems(false);
    bool FoundFresnel = false;
    for (int32 I = 0; I < Builder.GetNumActions(); ++I)
    {
        const auto& Action = Builder.GetSchemaAction(I);
        if (Action->GetTypeId() != FMaterialGraphSchemaAction_NewNode::StaticGetTypeId()) continue;
        if (StaticCastSharedPtr<FMaterialGraphSchemaAction_NewNode>(Action)->MaterialExpressionClass != UMaterialExpressionFresnel::StaticClass()) continue;
        FoundFresnel |= NativeMenu->SelectItemByName(FName(*Action->GetMenuDescription().ToString()));
    }
    TestTrue(TEXT("Actual engine menu finds Fresnel using Chinese"), FoundFresnel);
    Search.Restore();
    FString Json;
    FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Json));
    TestTrue(TEXT("Engine inventory exported"), FFileHelper::SaveStringToFile(Json,
        *FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MCHInventory.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    AddInfo(FString::Printf(TEXT("Loaded classes: %d; native menu entries: %d; engine function menu entries: %d"), Classes.Num(), ActionNames.Num(), Functions.Num()));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCHWidgetTest, "MaterialChineseHelper.StockWidgetTranslationAndRestore",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMCHWidgetTest::RunTest(const FString&)
{
    if (!FSlateApplication::IsInitialized()) { AddError(TEXT("Slate is required.")); return false; }
    auto Catalog = LoadCatalog(*this);
    FGraphFixture Fixture;
    auto* Node = Fixture.MakeNode<UMaterialExpressionFresnel>();
    auto* Expression = CastChecked<UMaterialExpressionFresnel>(Node->MaterialExpression);
    const FString OriginalTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
    TArray<FString> OriginalNames;
    for (const auto* Pin : Node->Pins) OriginalNames.Add(Pin->PinName.ToString());
    const float OriginalExponent = Expression->Exponent;
    const bool OriginalDirty = Fixture.Material->GetOutermost()->IsDirty();
    auto Widget = Node->CreateVisualWidget().ToSharedRef();
    FMCHPresentation Presentation(Catalog);
    Presentation.ScanWidget(Widget);
    TestTrue(TEXT("Stock title becomes bilingual"), Contains(Widget, TEXT("菲涅耳·视角遮罩 (Fresnel)")));
    TestTrue(TEXT("Exponent pin translated"), Contains(Widget, TEXT("指数 (ExponentIn)")));
    TestTrue(TEXT("Base reflect pin translated"), Contains(Widget, TEXT("正面基础值 (BaseReflectFractionIn)")));
    TestTrue(TEXT("Normal pin translated"), Contains(Widget, TEXT("世界空间法线 (Normal)")));
    Presentation.ScanWidget(Widget);
    TestFalse(TEXT("Repeated scans do not double-translate"), Contains(Widget, TEXT("指数 (指数")));
    TestEqual(TEXT("Graph title data untouched"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString(), OriginalTitle);
    TestEqual(TEXT("Exponent untouched"), Expression->Exponent, OriginalExponent);
    TestEqual(TEXT("Asset dirty flag unchanged"), Fixture.Material->GetOutermost()->IsDirty(), OriginalDirty);
    for (int32 I = 0; I < Node->Pins.Num(); ++I)
        TestEqual(TEXT("Pin identifiers untouched"), Node->Pins[I]->PinName.ToString(), OriginalNames[I]);
    Presentation.Restore();
    TestFalse(TEXT("Restore removes Chinese pin labels"), Contains(Widget, TEXT("正面基础值")));
    TestTrue(TEXT("Restore returns original title"), Contains(Widget, TEXT("Fresnel")));
    Presentation.bEnglish = false;
    Presentation.ScanWidget(Widget);
    TestTrue(TEXT("Chinese-only mode works"), Contains(Widget, TEXT("正面基础值")));
    TestFalse(TEXT("Chinese-only mode removes English pin suffix"), Contains(Widget, TEXT("正面基础值 (BaseReflectFractionIn)")));
    Presentation.Restore();
    Widget->UpdateGraphNode();
    Presentation.bEnglish = true;
    Presentation.ScanWidget(Widget);
    TestTrue(TEXT("Reconstructed stock widgets translate"), Contains(Widget, TEXT("世界空间法线 (Normal)")));
    Presentation.Restore();
    Presentation.bPins = false;
    Presentation.ScanWidget(Widget);
    TestFalse(TEXT("Pin translation can be disabled"), Contains(Widget, TEXT("正面基础值")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCHParameterTest, "MaterialChineseHelper.ParameterNamesPreserved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMCHParameterTest::RunTest(const FString&)
{
    auto Catalog = LoadCatalog(*this);
    FGraphFixture Fixture;
    auto* Node = Fixture.MakeNode<UMaterialExpressionScalarParameter>();
    auto* Expression = CastChecked<UMaterialExpressionScalarParameter>(Node->MaterialExpression);
    Expression->ParameterName = TEXT("My_Roughness_参数");
    TestTrue(TEXT("Fixture uses the real editable parameter heading"), Node->GetCanRenameNode());
    auto Widget = Node->CreateVisualWidget().ToSharedRef();
    FMCHPresentation Presentation(Catalog);
    Presentation.ScanWidget(Widget);
    TestEqual(TEXT("Parameter identifier remains unchanged"), Expression->ParameterName, FName(TEXT("My_Roughness_参数")));
    TestTrue(TEXT("Editable parameter heading retains original name"), Contains(Widget, TEXT("My_Roughness_参数")));
    TestFalse(TEXT("Editable name is not prefixed with translation"), Contains(Widget, TEXT("标量参数 · My_Roughness_参数")));
    Presentation.Restore();
    return true;
}

#endif
