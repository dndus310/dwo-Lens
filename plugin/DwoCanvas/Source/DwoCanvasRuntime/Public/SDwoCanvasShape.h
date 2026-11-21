// DwoCanvas - Slate leaf that draws an FDwoCanvasShapeStyle

#pragma once

#include "CoreMinimal.h"
#include "DwoCanvasShapeStyle.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SLeafWidget.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;

/**
 * Draws the shape in its allotted geometry. Shadows / outside strokes are drawn past the bounds
 * (the material quad is inflated by GetOverflow()), so layout size stays the design size.
 */
class DWOCANVASRUNTIME_API SDwoCanvasShape : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SDwoCanvasShape)
		: _DesiredSize(FVector2D::ZeroVector)
		, _Material(nullptr)
	{}
		SLATE_ARGUMENT(FDwoCanvasShapeStyle, Style)
		/** Size reported to layout; zero = no preference (takes the slot size). */
		SLATE_ARGUMENT(FVector2D, DesiredSize)
		/** Shape material; null = plugin default. */
		SLATE_ARGUMENT(UMaterialInterface*, Material)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetStyle(const FDwoCanvasShapeStyle& InStyle);
	void SetDesiredShapeSize(const FVector2D& InSize);
	void SetMaterial(UMaterialInterface* InMaterial);

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

protected:
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

private:
	void RefreshResources();

	FDwoCanvasShapeStyle Style;
	FVector2D DesiredShapeSize = FVector2D::ZeroVector;
	TWeakObjectPtr<UMaterialInterface> BaseMaterial;

	FSlateBrush SimpleBrush;
	FSlateBrush MaterialBrush;
	TStrongObjectPtr<UMaterialInstanceDynamic> MaterialInstance;

	/** Quad size last written to the material (only rewritten when the layout size changes). */
	mutable FVector2f LastQuadSize = FVector2f::ZeroVector;
	mutable float LastQuadFlags = -1.f;
};
