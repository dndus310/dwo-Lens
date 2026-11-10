// DwoCanvas - Slate leaf that draws an FDwoCanvasShapeStyle

#include "SDwoCanvasShape.h"

#include "Materials/MaterialInstanceDynamic.h"
#include "Rendering/DrawElements.h"
#include "UObject/Package.h"

void SDwoCanvasShape::Construct(const FArguments& InArgs)
{
	Style = InArgs._Style;
	DesiredShapeSize = InArgs._DesiredSize;
	BaseMaterial = InArgs._Material;
	RefreshResources();
}

void SDwoCanvasShape::SetStyle(const FDwoCanvasShapeStyle& InStyle)
{
	Style = InStyle;
	RefreshResources();
	Invalidate(EInvalidateWidgetReason::Paint);
}

void SDwoCanvasShape::SetDesiredShapeSize(const FVector2D& InSize)
{
	if (DesiredShapeSize != InSize)
	{
		DesiredShapeSize = InSize;
		Invalidate(EInvalidateWidgetReason::Layout);
	}
}

void SDwoCanvasShape::SetMaterial(UMaterialInterface* InMaterial)
{
	if (BaseMaterial.Get() != InMaterial)
	{
		BaseMaterial = InMaterial;
		MaterialInstance.Reset();
		RefreshResources();
		Invalidate(EInvalidateWidgetReason::Paint);
	}
}

void SDwoCanvasShape::RefreshResources()
{
	SimpleBrush = Style.MakeSimpleBrush();

	if (!Style.NeedsMaterial())
	{
		return;
	}

	UMaterialInterface* Base = BaseMaterial.IsValid() ? BaseMaterial.Get() : DwoCanvas::GetDefaultShapeMaterial();
	if (!Base)
	{
		return; // falls back to the simple brush
	}

	if (!MaterialInstance.IsValid() || MaterialInstance->Parent != Base)
	{
		MaterialInstance.Reset(UMaterialInstanceDynamic::Create(Base, GetTransientPackage()));
		LastQuadSize = FVector2f::ZeroVector;
		LastQuadFlags = -1.f;
	}

	Style.ApplyToMaterial(MaterialInstance.Get());
	MaterialBrush = FSlateBrush();
	MaterialBrush.DrawAs = ESlateBrushDrawType::Image;
	MaterialBrush.SetResourceObject(MaterialInstance.Get());
}

FVector2D SDwoCanvasShape::ComputeDesiredSize(float) const
{
	return DesiredShapeSize;
}

int32 SDwoCanvasShape::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const ESlateDrawEffect DrawEffects = ShouldBeEnabled(bParentEnabled) ? ESlateDrawEffect::None : ESlateDrawEffect::DisabledEffect;
	const FVector2f LocalSize = AllottedGeometry.GetLocalSize();

	if (Style.NeedsMaterial() && MaterialInstance.IsValid())
	{
		// One extra unit so the anti-aliased edge of the overflow is not cut
		const float Pad = Style.GetOverflow() + 1.f;
		const FVector2f QuadSize = LocalSize + FVector2f(2.f * Pad);
		// Quad.w: flags (1 = drop shadow shows behind the fill)
		const float Flags = Style.DropShadow.bShowBehindFill ? 1.f : 0.f;
		if (!QuadSize.Equals(LastQuadSize, 0.01f) || Flags != LastQuadFlags)
		{
			MaterialInstance->SetVectorParameterValue(TEXT("Quad"), FLinearColor(QuadSize.X, QuadSize.Y, Pad, Flags));
			LastQuadSize = QuadSize;
			LastQuadFlags = Flags;
		}

		FSlateDrawElement::MakeBox(
			OutDrawElements,
			LayerId,
			AllottedGeometry.ToPaintGeometry(QuadSize, FSlateLayoutTransform(FVector2f(-Pad, -Pad))),
			&MaterialBrush,
			DrawEffects,
			InWidgetStyle.GetColorAndOpacityTint());
	}
	else
	{
		FSlateDrawElement::MakeBox(
			OutDrawElements,
			LayerId,
			AllottedGeometry.ToPaintGeometry(),
			&SimpleBrush,
			DrawEffects,
			InWidgetStyle.GetColorAndOpacityTint() * SimpleBrush.GetTint(InWidgetStyle));
	}

	return LayerId;
}
