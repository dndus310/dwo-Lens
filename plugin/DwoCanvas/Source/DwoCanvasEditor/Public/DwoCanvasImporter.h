// DwoCanvas - imports a design export (designbridge/1 JSON) into Widget Blueprints

#pragma once

#include "CoreMinimal.h"
#include "DwoCanvasImportRecord.h"

class FJsonObject;
class UTexture2D;
class UWidgetBlueprint;

struct DWOCANVASEDITOR_API FDwoCanvasImportOptions
{
	/** Overwrite widgets edited in UE (clears their lock). */
	bool bResetOverrides = false;
	/** Only import documents with this name (components they use are still imported). Empty = all. */
	FString OnlyDocument;
	/** Render each document and compare with its exported preview (needs a running editor with Slate). */
	bool bVisualDiff = true;
};

/**
 * One import run. Documents become WBPs under WidgetFolder, components under ComponentFolder,
 * images become UI textures under TextureFolder (deduplicated by content hash).
 *
 * Re-import keeps work done in UE:
 *  - every generated widget is tied to its design node id (record file); names stay stable,
 *  - widgets edited in UE since the last import are kept as they are (and stay locked),
 *  - widgets added in UE are put back into their parent,
 *  - widgets whose node is gone are deleted only if nobody edited or references them; otherwise parked.
 */
class DWOCANVASEDITOR_API FDwoCanvasImporter
{
public:
	explicit FDwoCanvasImporter(const FDwoCanvasImportOptions& InOptions = FDwoCanvasImportOptions());
	~FDwoCanvasImporter();

	bool ImportFile(const FString& JsonFile, FDwoCanvasReport& OutReport);
	bool ImportJson(const FString& JsonText, const FString& SourceDirectory, FDwoCanvasReport& OutReport);

private:
	struct FDocContext;
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
