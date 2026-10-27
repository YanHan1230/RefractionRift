#pragma once

#include "CoreMinimal.h"
#include "MaterialChineseCatalog.h"

class SWidget;
class STextBlock;
class SInlineEditableTextBlock;
class IToolTip;
class UEdGraphNode;

/** Presentation-only adaptation of the stock UE material graph widgets. */
class FMCHPresentation
{
public:
    explicit FMCHPresentation(TSharedRef<FMCHCatalog> InCatalog) : Catalog(InCatalog) {}
    ~FMCHPresentation();
    void ScanWindows();
    void ScanWidget(const TSharedRef<SWidget>& Widget);
    void Restore();
    bool bEnglish = true;
    bool bPins = true;

private:
    struct FTitleRecord
    {
        TWeakPtr<SInlineEditableTextBlock> Widget;
        TWeakObjectPtr<UEdGraphNode> Node;
    };
    struct FTextRecord
    {
        TWeakPtr<STextBlock> Widget;
        FText Original;
    };
    struct FTooltipRecord
    {
        TWeakPtr<SWidget> Widget;
        TSharedPtr<IToolTip> Original;
    };
    void PatchNode(const TSharedRef<SWidget>& Widget);
    void PatchText(const TSharedRef<STextBlock>& Widget, const FString& Replacement);
    void PatchTooltip(const TSharedRef<SWidget>& Widget, const FString& Text);
    void Prune();
    TSharedRef<FMCHCatalog> Catalog;
    TMap<const SWidget*, FTitleRecord> Titles;
    TMap<const SWidget*, FTextRecord> Texts;
    TMap<const SWidget*, FTooltipRecord> Tooltips;
};
