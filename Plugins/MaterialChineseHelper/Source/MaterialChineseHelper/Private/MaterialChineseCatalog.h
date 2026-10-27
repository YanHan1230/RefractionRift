#pragma once

#include "CoreMinimal.h"

struct FMCHPinEntry
{
    FString Chinese;
    FString Description;
};

struct FMCHNodeEntry
{
    FString ClassName;
    FString English;
    FString Chinese;
    FString Category;
    FString Description;
    TMap<FString, FMCHPinEntry> Pins;
};

/** All data is editor-only. This object never writes material assets or class metadata. */
class FMCHCatalog
{
public:
    bool Load(const FString& Path, FString& OutError);
    const FMCHNodeEntry* Find(const FString& ClassName) const;
    const FMCHPinEntry* FindPin(const FString& ClassName, const FString& PinName) const;
    FString TranslateTitle(const FString& ClassName, const FString& Original, bool bEnglish) const;
    FString TranslatePin(const FString& ClassName, const FString& PinName,
                         const FString& Original, bool bEnglish) const;
    FString TranslateTerms(const FString& Original, bool bEnglish) const;
    TArray<TSharedPtr<FMCHNodeEntry>> Entries;

private:
    TMap<FString, int32> ByClass;
    TMap<FString, FMCHPinEntry> CommonPins;
    TMap<FString, FString> Terms;
};
