// DwoCanvas - UMG shape widget (gradient / shadow / dashed or outside stroke rectangles)

#pragma once

#include "CoreMinimal.h"
#include "Components/Widget.h"
#include "DwoCanvasShapeStyle.h"
#include "DwoCanvasShape.generated.h"

class SDwoCanvasShape;
class UMaterialInterface;

/**
 * Rounded rectangle with gradient fill, stroke (inside / center / outside, dashed) and drop / inner shadow.
 * Draws with a plain rounded-box brush when the style is simple, the SDF material otherwise.
 */
UCLASS(meta = (DisplayName = "Canvas Shape"))
class DWOCANVASRUNTIME_API UDwoCanvasShape : public UWidget
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Canvas Shape")
	void SetStyle(const FDwoCanvasShapeStyle& InStyle);

	UFUNCTION(BlueprintPure, Category = "Canvas Shape")
	const FDwoCanvasShapeStyle& GetStyle() const { return Style; }

	UFUNCTION(BlueprintCallable, Category = "Canvas Shape")
	void SetDesiredSize(FVector2D InSize);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Canvas Shape")
	FDwoCanvasShapeStyle Style;

	/** Size reported to layout (the design size). Zero = take the slot size. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Canvas Shape")
	FVector2D DesiredSize = FVector2D(100.f, 100.f);

	/** Shape material; empty = /DwoCanvas/Materials/M_DwoCanvasShape. Set explicitly so cooking picks it up. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Canvas Shape")
	TObjectPtr<UMaterialInterface> ShapeMaterial;

	virtual void SynchronizeProperties() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

#if WITH_EDITOR
	virtual const FText GetPaletteCategory() override;
#endif

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

	TSharedPtr<SDwoCanvasShape> MyShape;
};
