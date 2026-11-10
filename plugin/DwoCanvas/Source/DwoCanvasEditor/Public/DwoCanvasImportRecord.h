// DwoCanvas - re-import bookkeeping (node id <-> widget, edit detection) and the import report

#pragma once

#include "CoreMinimal.h"

class UWidget;

/** One generated widget. A node can produce several (key "<nodeId>" plus "<nodeId>#size" style parts). */
struct FDwoCanvasRecordEntry
{
	FName WidgetName;
	FString ClassPath;
	/** Hash of the widget (+ slot) right after the last import; a different hash means someone edited it. */
	FString Hash;
	/** Edited in UE at some point: never overwritten again (until -ResetOverrides). */
	bool bLocked = false;
};

/**
 * Stored as JSON next to the project (RecordDirectory/<package path>.json) and meant to be committed with the WBP.
 * Kept outside the asset so it is never cooked.
 */
struct DWOCANVASEDITOR_API FDwoCanvasImportRecord
{
	FString DocumentId;
	FString AssetPath;
	TMap<FString, FDwoCanvasRecordEntry> Entries;

	static FString GetRecordFile(const FString& AssetPackagePath);
	bool Load(const FString& AssetPackagePath);
	bool Save(const FString& AssetPackagePath) const;

	const FString* FindKeyByWidget(FName WidgetName) const;

	/** Property export of the widget and its slot. Stable across load / compile / save. */
	static FString ComputeWidgetHash(const UWidget* Widget);

	/** Same hash computed with the names of the older DesignBridge plugin (records written before the rename). */
	static FString ComputeLegacyWidgetHash(const UWidget* Widget);

	/** True when the widget differs from its record (current or legacy naming). */
	static bool IsEdited(const UWidget* Widget, const FDwoCanvasRecordEntry& Entry);
};

enum class EDwoCanvasSeverity : uint8
{
	Info,
	Warning,
	Error
};

struct DWOCANVASEDITOR_API FDwoCanvasReport
{
	struct FEntry
	{
		EDwoCanvasSeverity Severity;
		FString Document;
		FString NodeId;
		FString NodeName;
		FString Message;
	};

	/** Per written blueprint, for the result window. */
	struct FDocumentResult
	{
		FString Document;
		FString AssetPath;
		bool bComponent = false;
		/** Inferred draft (new blueprint in the draft folder, not re-importable). */
		bool bDraft = false;
		/** -1 = parent class has no BindWidget properties. */
		int32 BindingsMatched = -1;
		int32 BindingsTotal = 0;
		/** Share of pixels that differ from the design preview; -1 = not compared. */
		double DiffRatio = -1.0;
		FString DiffImage;
		FString ActualImage;
		FString ExpectedImage;
	};

	TArray<FEntry> Entries;
	TArray<FString> WrittenAssets;
	TArray<FDocumentResult> Documents;

	/** source.tool / source.adapter of the document, and whether it is an inferred draft. */
	FString SourceTool;
	FString Adapter;
	bool bDraft = false;

	FDocumentResult& FindOrAddDocument(const FString& Document);

	int32 Created = 0;
	int32 Regenerated = 0;
	int32 KeptEdited = 0;
	int32 KeptUserAdded = 0;
	int32 Removed = 0;
	int32 Orphaned = 0;

	void Add(EDwoCanvasSeverity Severity, const FString& Document, const FString& NodeId, const FString& NodeName, const FString& Message);
	int32 Count(EDwoCanvasSeverity Severity) const;
	bool HasErrors() const { return Count(EDwoCanvasSeverity::Error) > 0; }

	FString ToText() const;
	/** Saved/DwoCanvas/Reports/<name>_<time>.txt; returns the path. */
	FString SaveToFile(const FString& SourceName) const;
};
