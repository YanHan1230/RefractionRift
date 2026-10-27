#include "MaterialChineseCatalog.h"
#include "MaterialChinesePresentation.h"
#include "MaterialChineseSearch.h"
#include "Containers/Ticker.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Styling/CoreStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"

#define LOCTEXT_NAMESPACE "MaterialChineseHelper"
DEFINE_LOG_CATEGORY_STATIC(LogMaterialChineseHelper, Log, All);

class FMaterialChineseHelperModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        Catalog = MakeShared<FMCHCatalog>();
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MaterialChineseHelper"));
        FString Error;
        if (!Plugin || !Catalog->Load(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/NodeDictionary.json")), Error))
        {
            UE_LOG(LogMaterialChineseHelper, Error, TEXT("%s"), *Error);
            return;
        }
        UE_LOG(LogMaterialChineseHelper, Display, TEXT("Loaded %d material node translations."), Catalog->Entries.Num());
        Presentation = MakeUnique<FMCHPresentation>(Catalog.ToSharedRef());
        NativeSearch = MakeUnique<FMCHNativeSearch>(Catalog.ToSharedRef());
        GConfig->GetBool(Section, TEXT("Enabled"), bEnabled, GEditorPerProjectIni);
        GConfig->GetBool(Section, TEXT("ShowEnglish"), Presentation->bEnglish, GEditorPerProjectIni);
        GConfig->GetBool(Section, TEXT("TranslatePins"), Presentation->bPins, GEditorPerProjectIni);
        GConfig->GetBool(Section, TEXT("NativeSearch"), bNativeSearch, GEditorPerProjectIni);
        NativeSearch->bEnglish = Presentation->bEnglish;
        FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName,
            FOnSpawnTab::CreateRaw(this, &FMaterialChineseHelperModule::SpawnTab))
            .SetDisplayName(LOCTEXT("Tab", "材质中文助手"))
            .SetMenuType(ETabSpawnerMenuType::Hidden);
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FMaterialChineseHelperModule::RegisterMenus));
        TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FMaterialChineseHelperModule::Tick), 0.75f);
        SearchTickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FMaterialChineseHelperModule::SearchTick), 0.1f);
    }

    virtual void ShutdownModule() override
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
        FTSTicker::GetCoreTicker().RemoveTicker(SearchTickHandle);
        UToolMenus::UnRegisterStartupCallback(this);
        UToolMenus::UnregisterOwner(this);
        FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
        if (Presentation) Presentation->Restore();
        Presentation.Reset();
        if (NativeSearch) NativeSearch->Restore();
        NativeSearch.Reset();
    }

    // Widgets can hold module-backed attribute delegates until editor shutdown.
    virtual bool SupportsDynamicReloading() override { return false; }

private:
    bool SearchTick(float)
    {
        if (bEnabled && bNativeSearch && NativeSearch) NativeSearch->ScanWindows();
        return true;
    }
    bool Tick(float)
    {
        if (bEnabled && Presentation) Presentation->ScanWindows();
        return true;
    }

    void SaveAndRefresh()
    {
        GConfig->SetBool(Section, TEXT("Enabled"), bEnabled, GEditorPerProjectIni);
        GConfig->SetBool(Section, TEXT("ShowEnglish"), Presentation->bEnglish, GEditorPerProjectIni);
        GConfig->SetBool(Section, TEXT("TranslatePins"), Presentation->bPins, GEditorPerProjectIni);
        GConfig->SetBool(Section, TEXT("NativeSearch"), bNativeSearch, GEditorPerProjectIni);
        GConfig->Flush(false, GEditorPerProjectIni);
        Presentation->Restore();
        NativeSearch->Restore();
        NativeSearch->bEnglish = Presentation->bEnglish;
        if (bEnabled && bNativeSearch) NativeSearch->ScanWindows();
        if (bEnabled) Presentation->ScanWindows();
    }

    void RegisterMenus()
    {
        FToolMenuOwnerScoped Owner(this);
        UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
        Menu->AddSection(TEXT("MaterialChineseLearning"), LOCTEXT("Section", "材质学习"))
            .AddMenuEntry(TEXT("OpenMaterialChineseHelper"), LOCTEXT("Open", "材质中文助手"),
                LOCTEXT("Tip", "中英节点名称、接口解释，以及可按中文搜索的节点词典。"), FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([] { FGlobalTabmanager::Get()->TryInvokeTab(TabName); })));
    }

    void Filter(const FText& Text)
    {
        const FString Query = Text.ToString().TrimStartAndEnd();
        Filtered.Reset();
        for (const auto& Entry : Catalog->Entries)
        {
            const FString Searchable = Entry->Chinese + TEXT(" ") + Entry->English + TEXT(" ") +
                Entry->ClassName + TEXT(" ") + Entry->Category + TEXT(" ") + Entry->Description;
            if (Query.IsEmpty() || Searchable.Contains(Query)) Filtered.Add(Entry);
        }
        if (List) List->RequestListRefresh();
    }

    TSharedRef<ITableRow> Row(TSharedPtr<FMCHNodeEntry> Entry, const TSharedRef<STableViewBase>& Owner)
    {
        FString Explanation = Entry->Description;
        TArray<FString> PinNames;
        Entry->Pins.GetKeys(PinNames);
        PinNames.Sort();
        for (const FString& Pin : PinNames)
        {
            const FMCHPinEntry& Info = Entry->Pins[Pin];
            Explanation += TEXT("\n") + Pin + TEXT(" → ") + Info.Chinese + TEXT("：") + Info.Description;
        }
        return SNew(STableRow<TSharedPtr<FMCHNodeEntry>>, Owner).Padding(8)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                [SNew(STextBlock).Text(FText::FromString(Entry->Chinese + TEXT(" / ") + Entry->English))
                    .Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))]
                + SHorizontalBox::Slot().AutoWidth().Padding(8, 0)
                [SNew(SButton).Text(LOCTEXT("Copy", "复制英文名称"))
                    .OnClicked_Lambda([Entry]
                    {
                        FPlatformApplicationMisc::ClipboardCopy(*Entry->English);
                        return FReply::Handled();
                    })]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 5)
            [SNew(STextBlock).Text(FText::FromString(Explanation)).AutoWrapText(true)]
        ];
    }

    TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs&)
    {
        Filter(FText::GetEmpty());
        return SNew(SDockTab).TabRole(ETabRole::NomadTab)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(12, 10)
            [SNew(STextBlock).Text(LOCTEXT("Intro", "材质中文助手 · UE 5.5\n节点中英对照与中文说明。参数名称、连线和材质计算保持原样。"))
                .AutoWrapText(true)]
            + SVerticalBox::Slot().AutoHeight().Padding(12, 2)
            [SNew(SCheckBox).IsChecked_Lambda([this] { return bEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bEnabled = State == ECheckBoxState::Checked; SaveAndRefresh(); })
                [SNew(STextBlock).Text(LOCTEXT("Enabled", "启用节点中文显示"))]]
            + SVerticalBox::Slot().AutoHeight().Padding(12, 2)
            [SNew(SCheckBox).IsChecked_Lambda([this] { return Presentation->bEnglish ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Presentation->bEnglish = State == ECheckBoxState::Checked; SaveAndRefresh(); })
                [SNew(STextBlock).Text(LOCTEXT("English", "保留英文对照（推荐）"))]]
            + SVerticalBox::Slot().AutoHeight().Padding(12, 2)
            [SNew(SCheckBox).IsChecked_Lambda([this] { return Presentation->bPins ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Presentation->bPins = State == ECheckBoxState::Checked; SaveAndRefresh(); })
                [SNew(STextBlock).Text(LOCTEXT("Pins", "显示常用接口中文与说明"))]]
            + SVerticalBox::Slot().AutoHeight().Padding(12, 8)
            [SNew(SCheckBox).IsChecked_Lambda([this] { return bNativeSearch ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bNativeSearch = State == ECheckBoxState::Checked; SaveAndRefresh(); })
                [SNew(STextBlock).Text(LOCTEXT("NativeSearch", "原生右键菜单中文名称与中文搜索"))]]
            + SVerticalBox::Slot().AutoHeight().Padding(12, 8)
            [SNew(SSearchBox).HintText(LOCTEXT("Search", "输入中文或英文，例如：菲涅耳、混合、Fresnel"))
                .OnTextChanged_Raw(this, &FMaterialChineseHelperModule::Filter)]
            + SVerticalBox::Slot().AutoHeight().Padding(12, 0, 12, 8)
            [SNew(STextBlock).Text(LOCTEXT("SearchHelpV2", "可直接在材质编辑器右键菜单用中文搜索。此面板还提供节点与接口的学习说明。"))
                .AutoWrapText(true)]
            + SVerticalBox::Slot().FillHeight(1).Padding(4)
            [SAssignNew(List, SListView<TSharedPtr<FMCHNodeEntry>>).ListItemsSource(&Filtered)
                .SelectionMode(ESelectionMode::None)
                .OnGenerateRow_Raw(this, &FMaterialChineseHelperModule::Row)]
        ];
    }

    inline static const FName TabName = TEXT("MaterialChineseHelper");
    inline static const TCHAR* Section = TEXT("MaterialChineseHelper");
    TSharedPtr<FMCHCatalog> Catalog;
    TUniquePtr<FMCHPresentation> Presentation;
    TUniquePtr<FMCHNativeSearch> NativeSearch;
    TArray<TSharedPtr<FMCHNodeEntry>> Filtered;
    TSharedPtr<SListView<TSharedPtr<FMCHNodeEntry>>> List;
    FTSTicker::FDelegateHandle TickHandle;
    FTSTicker::FDelegateHandle SearchTickHandle;
    bool bEnabled = true;
    bool bNativeSearch = true;
};

IMPLEMENT_MODULE(FMaterialChineseHelperModule, MaterialChineseHelper)
#undef LOCTEXT_NAMESPACE
