// DwoCanvas - re-import bookkeeping and report

#include "DwoCanvasImportRecord.h"

#include "DwoCanvasSettings.h"

#include "Components/PanelSlot.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// ---------------------------------------------------------------------------------------------------------------------
// Record

FString FDwoCanvasImportRecord::GetRecordFile(const FString& AssetPackagePath)
{
	FString Relative = AssetPackagePath;
	Relative.RemoveFromStart(TEXT("/"));
	return FPaths::Combine(FPaths::ProjectDir(), UDwoCanvasSettings::Get().RecordDirectory, Relative + TEXT(".json"));
}

bool FDwoCanvasImportRecord::Load(const FString& AssetPackagePath)
{
	Entries.Reset();
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *GetRecordFile(AssetPackagePath)))
	{
		// Records written by the older DesignBridge plugin
		FString Relative = AssetPackagePath;
		Relative.RemoveFromStart(TEXT("/"));
		const FString Legacy = FPaths::Combine(FPaths::ProjectDir(), TEXT("DesignBridge/Records"), Relative + TEXT(".json"));
		if (!FFileHelper::LoadFileToString(Text, *Legacy))
		{
			return false;
		}
	}

	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		return false;
	}

	DocumentId = Root->GetStringField(TEXT("document"));
	AssetPath = Root->GetStringField(TEXT("asset"));
	const TSharedPtr<FJsonObject>* Widgets = nullptr;
	if (Root->TryGetObjectField(TEXT("widgets"), Widgets))
	{
		for (const auto& Pair : (*Widgets)->Values)
		{
			const TSharedPtr<FJsonObject> Item = Pair.Value->AsObject();
			if (!Item.IsValid())
			{
				continue;
			}
			FDwoCanvasRecordEntry Entry;
			Entry.WidgetName = FName(*Item->GetStringField(TEXT("name")));
			Entry.ClassPath = Item->GetStringField(TEXT("class"));
			Entry.Hash = Item->GetStringField(TEXT("hash"));
			Item->TryGetBoolField(TEXT("locked"), Entry.bLocked);
			Entries.Add(Pair.Key, Entry);
		}
	}
	return true;
}

bool FDwoCanvasImportRecord::Save(const FString& AssetPackagePath) const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("document"), DocumentId);
	Root->SetStringField(TEXT("asset"), AssetPackagePath);

	TSharedRef<FJsonObject> Widgets = MakeShared<FJsonObject>();
	TArray<FString> Keys;
	Entries.GetKeys(Keys);
	Keys.Sort(); // stable diffs
	for (const FString& Key : Keys)
	{
		const FDwoCanvasRecordEntry& Entry = Entries[Key];
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Entry.WidgetName.ToString());
		Item->SetStringField(TEXT("class"), Entry.ClassPath);
		Item->SetStringField(TEXT("hash"), Entry.Hash);
		if (Entry.bLocked)
		{
			Item->SetBoolField(TEXT("locked"), true);
		}
		Widgets->SetObjectField(Key, Item);
	}
	Root->SetObjectField(TEXT("widgets"), Widgets);

	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Root, Writer);

	const FString File = GetRecordFile(AssetPackagePath);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
	return FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

const FString* FDwoCanvasImportRecord::FindKeyByWidget(FName WidgetName) const
{
	for (const auto& Pair : Entries)
	{
		if (Pair.Value.WidgetName == WidgetName)
		{
			return &Pair.Key;
		}
	}
	return nullptr;
}

namespace
{
	void AppendProperties(const UObject* Object, const TSet<FName>& Skip, FString& Out)
	{
		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient)
				|| Skip.Contains(Property->GetFName()))
			{
				continue;
			}
			FString Value;
			Property->ExportText_InContainer(0, Value, Object, nullptr, const_cast<UObject*>(Object), PPF_None);
			Out += Property->GetName();
			Out += TEXT("=");
			Out += Value;
			Out += TEXT(";");
		}
	}
}

namespace
{
	FString WidgetHashText(const UWidget* Widget);
}

FString FDwoCanvasImportRecord::ComputeWidgetHash(const UWidget* Widget)
{
	return Widget ? FMD5::HashAnsiString(*WidgetHashText(Widget)) : FString();
}

FString FDwoCanvasImportRecord::ComputeLegacyWidgetHash(const UWidget* Widget)
{
	if (!Widget)
	{
		return FString();
	}
	// Reverse of the DesignBridge -> DwoCanvas rename (see Config/DefaultDwoCanvas.ini CoreRedirects)
	static const TPair<const TCHAR*, const TCHAR*> Names[] = {
		{ TEXT("/DwoCanvas/Materials/M_DwoCanvasShape"), TEXT("/DesignBridge/Materials/M_DesignShape") },
		{ TEXT("M_DwoCanvasShape"), TEXT("M_DesignShape") },
		{ TEXT("DwoCanvasRuntime"), TEXT("DesignBridgeRuntime") },
		{ TEXT("DwoCanvasFrameSlot"), TEXT("DesignFrameSlot") },
		{ TEXT("DwoCanvasShape"), TEXT("DesignShape") },
		{ TEXT("DwoCanvasFrame"), TEXT("DesignFrame") },
	};
	FString Text = WidgetHashText(Widget);
	for (const TPair<const TCHAR*, const TCHAR*>& Pair : Names)
	{
		Text.ReplaceInline(Pair.Key, Pair.Value, ESearchCase::CaseSensitive);
	}
	return FMD5::HashAnsiString(*Text);
}

bool FDwoCanvasImportRecord::IsEdited(const UWidget* Widget, const FDwoCanvasRecordEntry& Entry)
{
	return ComputeWidgetHash(Widget) != Entry.Hash && ComputeLegacyWidgetHash(Widget) != Entry.Hash;
}

namespace
{
FString WidgetHashText(const UWidget* Widget)
{

	static const TSet<FName> SkipWidget = { TEXT("Slot") };
	static const TSet<FName> SkipSlot = { TEXT("Parent"), TEXT("Content") };

	FString Text = Widget->GetClass()->GetPathName();
	AppendProperties(Widget, SkipWidget, Text);
	if (Widget->Slot)
	{
		Text += TEXT("|slot|");
		Text += Widget->Slot->GetClass()->GetName();
		AppendProperties(Widget->Slot, SkipSlot, Text);
	}
	return Text;
}
}

// ---------------------------------------------------------------------------------------------------------------------
// Report

void FDwoCanvasReport::Add(EDwoCanvasSeverity Severity, const FString& Document, const FString& NodeId, const FString& NodeName, const FString& Message)
{
	Entries.Add({ Severity, Document, NodeId, NodeName, Message });

	const FString Line = FString::Printf(TEXT("[%s] %s%s%s"), *Document, NodeName.IsEmpty() ? TEXT("") : *(NodeName + TEXT(" (") + NodeId + TEXT("): ")), TEXT(""), *Message);
	switch (Severity)
	{
	case EDwoCanvasSeverity::Error: UE_LOG(LogTemp, Error, TEXT("DwoCanvas %s"), *Line); break;
	case EDwoCanvasSeverity::Warning: UE_LOG(LogTemp, Warning, TEXT("DwoCanvas %s"), *Line); break;
	default: UE_LOG(LogTemp, Display, TEXT("DwoCanvas %s"), *Line); break;
	}
}

FDwoCanvasReport::FDocumentResult& FDwoCanvasReport::FindOrAddDocument(const FString& Document)
{
	if (FDocumentResult* Found = Documents.FindByPredicate([&Document](const FDocumentResult& Item) { return Item.Document == Document; }))
	{
		return *Found;
	}
	FDocumentResult& Added = Documents.AddDefaulted_GetRef();
	Added.Document = Document;
	return Added;
}

int32 FDwoCanvasReport::Count(EDwoCanvasSeverity Severity) const
{
	return Entries.FilterByPredicate([Severity](const FEntry& Entry) { return Entry.Severity == Severity; }).Num();
}

FString FDwoCanvasReport::ToText() const
{
	FString Out;
	Out += FString::Printf(TEXT("DwoCanvas import - %d error(s), %d warning(s)\n"), Count(EDwoCanvasSeverity::Error), Count(EDwoCanvasSeverity::Warning));
	Out += FString::Printf(TEXT("Widgets: %d created, %d regenerated, %d kept (edited in UE), %d kept (added in UE), %d removed, %d orphaned\n"),
		Created, Regenerated, KeptEdited, KeptUserAdded, Removed, Orphaned);
	for (const FString& Asset : WrittenAssets)
	{
		Out += TEXT("  wrote ") + Asset + TEXT("\n");
	}
	Out += TEXT("\n");

	for (const EDwoCanvasSeverity Severity : { EDwoCanvasSeverity::Error, EDwoCanvasSeverity::Warning, EDwoCanvasSeverity::Info })
	{
		for (const FEntry& Entry : Entries)
		{
			if (Entry.Severity != Severity)
			{
				continue;
			}
			const TCHAR* Tag = Severity == EDwoCanvasSeverity::Error ? TEXT("ERROR") : (Severity == EDwoCanvasSeverity::Warning ? TEXT("WARN ") : TEXT("info "));
			Out += FString::Printf(TEXT("%s [%s] %s%s\n"), Tag, *Entry.Document,
				Entry.NodeName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("%s (%s): "), *Entry.NodeName, *Entry.NodeId), *Entry.Message);
		}
	}
	return Out;
}

FString FDwoCanvasReport::SaveToFile(const FString& SourceName) const
{
	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DwoCanvas"), TEXT("Reports"));
	IFileManager::Get().MakeDirectory(*Dir, true);
	const FString File = FPaths::Combine(Dir, FString::Printf(TEXT("%s_%s.txt"), *FPaths::GetBaseFilename(SourceName), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"))));
	FFileHelper::SaveStringToFile(ToText(), *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	return File;
}
