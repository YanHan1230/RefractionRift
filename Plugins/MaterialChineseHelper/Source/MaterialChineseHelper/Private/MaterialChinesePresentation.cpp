#include "MaterialChinesePresentation.h"
#include "EdGraph/EdGraphNode.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/Children.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunctionInterface.h"
#include "SGraphNode.h"
#include "SGraphPin.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/SInlineEditableTextBlock.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
FText ReadHead(const TWeakObjectPtr<UEdGraphNode> Node)
{
    if (!Node.IsValid()) return FText::GetEmpty();
    if (Node->GetCanRenameNode()) return Node->GetNodeTitle(ENodeTitleType::EditableTitle);
    FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
    int32 LineBreak;
    if (Title.FindChar(TEXT('\n'), LineBreak)) Title.LeftInline(LineBreak);
    return FText::FromString(Title.Replace(TEXT("\r"), TEXT("")));
}

template<typename Callback>
void Walk(const TSharedRef<SWidget>& Widget, Callback&& Visit)
{
    if (!Visit(Widget)) return;
    FChildren* Children = Widget->GetChildren();
    if (!Children) return;
    for (int32 Index = 0; Index < Children->Num(); ++Index)
        Walk(Children->GetChildAt(Index), Visit);
}
}

FMCHPresentation::~FMCHPresentation() { Restore(); }

void FMCHPresentation::PatchText(const TSharedRef<STextBlock>& Widget, const FString& Replacement)
{
    if (Texts.Contains(&Widget.Get())) return;
    const FText Original = Widget->GetText();
    if (Original.ToString() == Replacement) return;
    Texts.Add(&Widget.Get(), {Widget, Original});
    Widget->SetText(FText::FromString(Replacement));
}

void FMCHPresentation::PatchTooltip(const TSharedRef<SWidget>& Widget, const FString& Text)
{
    if (Text.IsEmpty() || Tooltips.Contains(&Widget.Get())) return;
    Tooltips.Add(&Widget.Get(), {Widget, Widget->GetToolTip()});
    Widget->SetToolTipText(FText::FromString(Text));
}

void FMCHPresentation::PatchNode(const TSharedRef<SWidget>& Widget)
{
    const auto GraphWidget = StaticCastSharedRef<SGraphNode>(Widget);
    UEdGraphNode* Node = GraphWidget->GetNodeObj();
    if (!IsValid(Node) || !Cast<UMaterialGraph>(Node->GetGraph())) return;
    const UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Node);
    const UMaterialExpression* Expression = MaterialNode ? MaterialNode->MaterialExpression.Get() : nullptr;
    FString ClassName = Expression ? Expression->GetClass()->GetName() : FString();
    if (const auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
        if (Call->MaterialFunction && Call->MaterialFunction->GetPathName().StartsWith(TEXT("/Engine/")))
            if (Catalog->Find(TEXT("Function:") + Call->MaterialFunction->GetName()))
                ClassName = TEXT("Function:") + Call->MaterialFunction->GetName();
    const FMCHNodeEntry* Entry = Catalog->Find(ClassName);
    if (Entry)
    {
        PatchTooltip(Widget, Entry->Chinese + TEXT(" (") + Entry->English + TEXT(")\n\n") +
            Entry->Description + TEXT("\n\nUE 原始说明：\n") + Node->GetTooltipText().ToString());
    }

    Walk(Widget, [&](const TSharedRef<SWidget>& Child)
    {
        const FString Type = Child->GetTypeAsString();
        if (Type.StartsWith(TEXT("SGraphPin")))
        {
            if (!bPins) return false;
            auto PinWidget = StaticCastSharedRef<SGraphPin>(Child);
            UEdGraphPin* Pin = PinWidget->GetPinObj();
            if (!Pin || Pin->IsPendingKill()) return false;
            const FString Name = Pin->PinName.ToString();
            const FText Original = Node->GetPinDisplayName(Pin);
            const FString Translated = Catalog->TranslatePin(ClassName, Name, Original.ToString(), bEnglish);
            const FMCHPinEntry* PinEntry = Catalog->FindPin(ClassName, Name);
            if (!PinEntry) PinEntry = Catalog->FindPin(ClassName, Original.ToString());
            if (PinEntry)
                PatchTooltip(Child, PinEntry->Chinese + TEXT(" (") + Name + TEXT(")\n\n") + PinEntry->Description);
            // Only the label is changed; inline numeric values and graph pin data are untouched.
            Walk(Child, [&](const TSharedRef<SWidget>& Label)
            {
                if (Label->GetType() == FName(TEXT("STextBlock")))
                {
                    auto Text = StaticCastSharedRef<STextBlock>(Label);
                    if (Text->GetText().EqualTo(Original)) PatchText(Text, Translated);
                    return false;
                }
                return true;
            });
            return false;
        }
        if (!Entry) return true;
        if (Type == TEXT("SInlineEditableTextBlock"))
        {
            // Editable headings are parameter identifiers. Never replace these with translations.
            if (!Node->GetCanRenameNode() && !Titles.Contains(&Child.Get()))
            {
                auto Title = StaticCastSharedRef<SInlineEditableTextBlock>(Child);
                TWeakObjectPtr<UEdGraphNode> WeakNode(Node);
                Titles.Add(&Child.Get(), {Title, WeakNode});
                TSharedRef<FMCHCatalog> SafeCatalog = Catalog;
                const bool English = bEnglish;
                Title->SetText(TAttribute<FText>::CreateLambda([WeakNode, SafeCatalog, ClassName, English]
                {
                    return FText::FromString(SafeCatalog->TranslateTitle(ClassName, ReadHead(WeakNode).ToString(), English));
                }));
            }
            return false;
        }
        if (Type == TEXT("SNodeTitle"))
        {
            // Stock SNodeTitle rebuilds these extra lines when expression values change.
            Walk(Child, [&](const TSharedRef<SWidget>& Line)
            {
                if (Line->GetType() == FName(TEXT("STextBlock")))
                {
                    auto Text = StaticCastSharedRef<STextBlock>(Line);
                    if (!Texts.Contains(&Line.Get()))
                        PatchText(Text, Catalog->TranslateTitle(ClassName, Text->GetText().ToString(), bEnglish));
                    return false;
                }
                return true;
            });
            return false;
        }
        return true;
    });
}

void FMCHPresentation::ScanWidget(const TSharedRef<SWidget>& Widget)
{
    Walk(Widget, [&](const TSharedRef<SWidget>& Child)
    {
        // Material nodes supply their own CreateVisualWidget, bypassing UE's node factories.
        // Adapt their stock Slate presentation rather than registering an ineffective factory.
        if (Child->GetTypeAsString().StartsWith(TEXT("SGraphNodeMaterial")))
        {
            PatchNode(Child);
            return false;
        }
        return true;
    });
}

void FMCHPresentation::Prune()
{
    for (auto It = Titles.CreateIterator(); It; ++It) if (!It.Value().Widget.IsValid()) It.RemoveCurrent();
    for (auto It = Texts.CreateIterator(); It; ++It) if (!It.Value().Widget.IsValid()) It.RemoveCurrent();
    for (auto It = Tooltips.CreateIterator(); It; ++It) if (!It.Value().Widget.IsValid()) It.RemoveCurrent();
}

void FMCHPresentation::ScanWindows()
{
    Prune();
    if (!FSlateApplication::IsInitialized()) return;
    for (const TSharedRef<SWindow>& Window : FSlateApplication::Get().GetInteractiveTopLevelWindows())
        ScanWidget(Window);
}

void FMCHPresentation::Restore()
{
    for (const auto& Pair : Titles)
    {
        if (auto Widget = Pair.Value.Widget.Pin())
        {
            const TWeakObjectPtr<UEdGraphNode> Node = Pair.Value.Node;
            Widget->SetText(TAttribute<FText>::CreateLambda([Node] { return ReadHead(Node); }));
        }
    }
    for (const auto& Pair : Texts)
        if (auto Widget = Pair.Value.Widget.Pin()) Widget->SetText(Pair.Value.Original);
    for (const auto& Pair : Tooltips)
        if (auto Widget = Pair.Value.Widget.Pin()) Widget->SetToolTip(Pair.Value.Original);
    Titles.Reset();
    Texts.Reset();
    Tooltips.Reset();
}
