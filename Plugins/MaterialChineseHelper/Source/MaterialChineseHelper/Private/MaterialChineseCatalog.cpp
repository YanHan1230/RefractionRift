#include "MaterialChineseCatalog.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
void ReadPins(const TSharedPtr<FJsonObject>& Parent, TMap<FString, FMCHPinEntry>& Result)
{
    const TSharedPtr<FJsonObject>* Pins = nullptr;
    if (!Parent->TryGetObjectField(TEXT("Pins"), Pins)) return;
    for (const auto& Pair : (*Pins)->Values)
    {
        const TSharedPtr<FJsonObject>* Object = nullptr;
        if (!Pair.Value->TryGetObject(Object)) continue;
        FMCHPinEntry Pin;
        (*Object)->TryGetStringField(TEXT("Chinese"), Pin.Chinese);
        (*Object)->TryGetStringField(TEXT("Description"), Pin.Description);
        if (!Pin.Chinese.IsEmpty()) Result.Add(Pair.Key, MoveTemp(Pin));
    }
}
}

bool FMCHCatalog::Load(const FString& Path, FString& OutError)
{
    Entries.Reset();
    ByClass.Reset();
    CommonPins.Reset();
    Terms.Reset();
    FString Json;
    TSharedPtr<FJsonObject> Root;
    if (!FFileHelper::LoadFileToString(Json, *Path) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root)
    {
        OutError = TEXT("Cannot read dictionary: ") + Path;
        return false;
    }
    ReadPins(Root, CommonPins);
    const TSharedPtr<FJsonObject>* TermObject = nullptr;
    if (Root->TryGetObjectField(TEXT("Terms"), TermObject))
        for (const auto& Pair : (*TermObject)->Values)
            Terms.Add(Pair.Key, Pair.Value->AsString());
    const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
    if (!Root->TryGetArrayField(TEXT("Nodes"), Nodes))
    {
        OutError = TEXT("Dictionary has no Nodes array.");
        return false;
    }
    for (const TSharedPtr<FJsonValue>& Value : *Nodes)
    {
        const TSharedPtr<FJsonObject>* Object = nullptr;
        if (!Value->TryGetObject(Object)) continue;
        auto Entry = MakeShared<FMCHNodeEntry>();
        (*Object)->TryGetStringField(TEXT("Class"), Entry->ClassName);
        (*Object)->TryGetStringField(TEXT("English"), Entry->English);
        (*Object)->TryGetStringField(TEXT("Chinese"), Entry->Chinese);
        (*Object)->TryGetStringField(TEXT("Category"), Entry->Category);
        (*Object)->TryGetStringField(TEXT("Description"), Entry->Description);
        if (Entry->ClassName.IsEmpty() || Entry->Chinese.IsEmpty() ||
            Entry->English.IsEmpty() || ByClass.Contains(Entry->ClassName))
        {
            OutError = TEXT("Invalid or duplicate class: ") + Entry->ClassName;
            Entries.Reset();
            ByClass.Reset();
            return false;
        }
        ReadPins(*Object, Entry->Pins);
        ByClass.Add(Entry->ClassName, Entries.Num());
        Entries.Add(Entry);
    }
    if (Entries.IsEmpty())
    {
        OutError = TEXT("Dictionary is empty.");
        return false;
    }
    return true;
}

const FMCHNodeEntry* FMCHCatalog::Find(const FString& ClassName) const
{
    const int32* Index = ByClass.Find(ClassName);
    return Index ? Entries[*Index].Get() : nullptr;
}

const FMCHPinEntry* FMCHCatalog::FindPin(const FString& ClassName, const FString& PinName) const
{
    if (const FMCHNodeEntry* Node = Find(ClassName))
        if (const FMCHPinEntry* Pin = Node->Pins.Find(PinName)) return Pin;
    return CommonPins.Find(PinName);
}

FString FMCHCatalog::TranslateTerms(const FString& Original, bool bEnglish) const
{
    if (const FString* Chinese = Terms.Find(Original))
        return bEnglish ? *Chinese + TEXT(" (") + Original + TEXT(")") : *Chinese;
    TArray<FString> Categories;
    if (Original.ParseIntoArray(Categories, TEXT("|"), false) > 1)
    {
        for (FString& Category : Categories) Category = TranslateTerms(Category, bEnglish);
        return FString::Join(Categories, TEXT("|"));
    }
    return Original;
}

FString FMCHCatalog::TranslateTitle(const FString& ClassName, const FString& Original, bool bEnglish) const
{
    const FMCHNodeEntry* Entry = Find(ClassName);
    if (!Entry) return Original;
    // Keep numeric values, texture names, function names and other dynamic captions intact.
    FString Simple = Original.Replace(TEXT(" "), TEXT(""));
    FString EnglishSimple = Entry->English.Replace(TEXT(" "), TEXT(""));
    FString ClassSimple = ClassName;
    ClassSimple.RemoveFromStart(TEXT("MaterialExpression"));
    const bool bOnlyType = Simple.Equals(EnglishSimple, ESearchCase::IgnoreCase) ||
        Simple.Equals(ClassSimple, ESearchCase::IgnoreCase) ||
        (ClassSimple == TEXT("LinearInterpolate") && Simple == TEXT("Lerp"));
    if (bOnlyType)
        return bEnglish ? Entry->Chinese + TEXT(" (") + Original + TEXT(")") : Entry->Chinese;
    return Entry->Chinese + TEXT(" · ") + Original;
}

FString FMCHCatalog::TranslatePin(const FString& ClassName, const FString& PinName,
                                const FString& Original, bool bEnglish) const
{
    const FMCHPinEntry* Pin = FindPin(ClassName, PinName);
    if (!Pin) Pin = FindPin(ClassName, Original);
    if (!Pin || Original.TrimStartAndEnd().IsEmpty()) return TranslateTerms(Original, bEnglish);
    return bEnglish ? Pin->Chinese + TEXT(" (") + PinName + TEXT(")") : Pin->Chinese;
}
