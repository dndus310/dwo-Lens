// DwoCanvas - renders a Widget Blueprint and compares it with the design preview

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "DwoCanvasVisualDiff.generated.h"

class UWidgetBlueprint;
struct FDwoCanvasReport;

/** Placeholder list item used when rendering list views for the visual diff. */
UCLASS(Transient)
class UDwoCanvasPreviewItem : public UObject
{
	GENERATED_BODY()

public:
	/** Entry text widget name -> the text this entry had in the design. */
	UPROPERTY()
	TMap<FName, FString> Texts;
};

/** List / tile view widget name -> one text map per design entry (what the design showed in each row). */
using FDwoCanvasListPreview = TMap<FName, TArray<TMap<FName, FString>>>;

class DWOCANVASEDITOR_API FDwoCanvasVisualDiff
{
public:
	/** True when widgets can be rendered here (editor with Slate + RHI; not commandlets). */
	static bool CanRender();

	/** Renders the blueprint at Size into RGBA8 sRGB pixels. */
	static bool Render(UWidgetBlueprint* Blueprint, FIntPoint Size, TArray<FColor>& OutPixels, const FDwoCanvasListPreview* ListPreview = nullptr);

	/**
	 * Renders the blueprint, compares it with the preview PNG (exported at PreviewScale), writes
	 * <doc>_actual.png / _expected.png / _diff.png to Saved/DwoCanvas/Reports and reports the result.
	 * ListPreview fills list entries with the design's texts so rows compare like for like.
	 */
	static void Compare(UWidgetBlueprint* Blueprint, const TArray<uint8>& PreviewPng, FIntPoint Size, double PreviewScale,
		const FString& DocumentName, FDwoCanvasReport& Report, const FDwoCanvasListPreview* ListPreview = nullptr);
};
