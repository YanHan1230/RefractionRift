#pragma once
#include "CoreMinimal.h"
#include "MaterialChineseCatalog.h"

class SWidget;
class SGraphActionMenu;
struct FEdGraphSchemaAction;

/** Updates the native action objects, so UE's own filtering and spawning stay in use. */
class FMCHNativeSearch
{
public:
    explicit FMCHNativeSearch(TSharedRef<FMCHCatalog> InCatalog) : Catalog(InCatalog) {}
    ~FMCHNativeSearch();
    void ScanWindows();
    void ScanWidget(const TSharedRef<SWidget>& Widget);
    bool PatchMenu(const TSharedRef<SGraphActionMenu>& Menu);
    void Restore();
    bool bEnglish = true;

private:
    struct FRecord
    {
        TWeakPtr<FEdGraphSchemaAction> Action;
        FText Name, Tooltip, Category, Keywords;
        TSharedPtr<FEdGraphSchemaAction> OriginalBase;
    };
    TSharedRef<FMCHCatalog> Catalog;
    TMap<const FEdGraphSchemaAction*, FRecord> Records;
    TMap<const SGraphActionMenu*, TWeakPtr<SGraphActionMenu>> Menus;
};
