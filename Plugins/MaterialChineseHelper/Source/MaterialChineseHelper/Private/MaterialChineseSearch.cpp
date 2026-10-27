#include "MaterialChineseSearch.h"
#include "EdGraph/EdGraphSchema.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/Children.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "SGraphActionMenu.h"
#include "Widgets/SWindow.h"

namespace
{
// UE 5.5 exposes no public getter for the complete action list. A protected
// pointer-to-member accessor accesses it without a layout cast or private API.
// The actual object remains the original SGraphActionMenu.
struct FMaterialMenuAccess : SGraphActionMenu
{
    static auto ActionsMember() { return &FMaterialMenuAccess::AllActions; }
};

void WalkMenu(const TSharedRef<SWidget>& Widget, FMCHNativeSearch& Search)
{
    const FString Type = Widget->GetTypeAsString();
    if (Type == TEXT("SGraphActionMenu"))
    {
        Search.PatchMenu(StaticCastSharedRef<SGraphActionMenu>(Widget));
        return;
    }
    if (Type.StartsWith(TEXT("SGraphNode")) || !Widget->GetVisibility().IsVisible()) return;
    FChildren* Children = Widget->GetChildren();
    if (Children)
        for (int32 I = 0; I < Children->Num(); ++I) WalkMenu(Children->GetChildAt(I), Search);
}
}

FMCHNativeSearch::~FMCHNativeSearch() { Restore(); }

bool FMCHNativeSearch::PatchMenu(const TSharedRef<SGraphActionMenu>& Menu)
{
    const TSharedPtr<FGraphActionListBuilderBase>& Actions = Menu.Get().*FMaterialMenuAccess::ActionsMember();
    if (!Actions) return false;
    bool bChanged = false;
    for (int32 I = 0; I < Actions->GetNumActions(); ++I)
    {
        const TSharedPtr<FEdGraphSchemaAction>& Action = Actions->GetSchemaAction(I);
        if (!Action || Records.Contains(Action.Get())) continue;
        const FMCHNodeEntry* Entry = nullptr;
        if (Action->GetTypeId() == FMaterialGraphSchemaAction_NewNode::StaticGetTypeId())
        {
            const auto Native = StaticCastSharedPtr<FMaterialGraphSchemaAction_NewNode>(Action);
            if (Native->MaterialExpressionClass) Entry = Catalog->Find(Native->MaterialExpressionClass->GetName());
        }
        else if (Action->GetTypeId() == FMaterialGraphSchemaAction_NewFunctionCall::StaticGetTypeId())
        {
            const auto Native = StaticCastSharedPtr<FMaterialGraphSchemaAction_NewFunctionCall>(Action);
            // Do not reinterpret user or third-party asset names as engine functions.
            if (Native->FunctionPath.StartsWith(TEXT("/Engine/")))
            {
                FString Path, Name;
                if (Native->FunctionPath.Split(TEXT("."), &Path, &Name, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
                    Entry = Catalog->Find(TEXT("Function:") + Name);
            }
        }
        if (!Entry) continue;
        FRecord Record{Action, Action->GetMenuDescription(), Action->GetTooltipDescription(), Action->GetCategory(), Action->GetKeywords()};
        Record.OriginalBase = MakeShared<FEdGraphSchemaAction>(*Action);
        Records.Add(Action.Get(), Record);
        FString Name = Entry->Chinese;
        if (bEnglish) Name += TEXT(" (") + Record.Name.ToString() + TEXT(")");
        const FString Tooltip = Entry->Chinese + TEXT(" / ") + Entry->English + TEXT("\n\n") +
            Entry->Description + TEXT("\n\n") + Record.Tooltip.ToString();
        const FString Keywords = Record.Keywords.ToString() + TEXT(" ") + Entry->English + TEXT(" ") +
            Entry->Chinese + TEXT(" ") + Entry->ClassName;
        // UE 5.5 UpdateSearchData assumes an empty private category cache.
        // Assign freshly constructed base presentation data to rebuild every cache,
        // preserving the derived native action, its class/path and its virtual dispatch.
        *Action = FEdGraphSchemaAction(FText::FromString(Catalog->TranslateTerms(Record.Category.ToString(), bEnglish)),
            FText::FromString(Name), FText::FromString(Tooltip), Action->Grouping,
            FText::FromString(Keywords), Action->SectionID);
        bChanged = true;
    }
    if (bChanged)
    {
        Menus.Add(&Menu.Get(), Menu);
        // Refresh filtering with the current user text intact, including an already typed Chinese query.
        Menu->GenerateFilteredItems(true);
    }
    return bChanged;
}

void FMCHNativeSearch::ScanWidget(const TSharedRef<SWidget>& Widget) { WalkMenu(Widget, *this); }

void FMCHNativeSearch::ScanWindows()
{
    for (auto It = Records.CreateIterator(); It; ++It) if (!It.Value().Action.IsValid()) It.RemoveCurrent();
    for (auto It = Menus.CreateIterator(); It; ++It) if (!It.Value().IsValid()) It.RemoveCurrent();
    if (!FSlateApplication::IsInitialized()) return;
    TArray<TSharedRef<SWindow>> Windows;
    // Includes popup / child windows used by native right-click menus.
    FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
    for (const auto& Window : Windows) ScanWidget(Window);
}

void FMCHNativeSearch::Restore()
{
    for (const auto& Pair : Records)
        if (auto Action = Pair.Value.Action.Pin())
            *Action = *Pair.Value.OriginalBase;
    for (const auto& Pair : Menus)
        if (auto Menu = Pair.Value.Pin()) Menu->GenerateFilteredItems(true);
    Records.Reset();
    Menus.Reset();
}
