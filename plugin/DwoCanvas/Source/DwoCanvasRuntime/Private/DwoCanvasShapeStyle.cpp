// DwoCanvas - shape style shared by UDwoCanvasShape and UDwoCanvasFrame backgrounds

#include "DwoCanvasShapeStyle.h"

#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace DwoCanvas
{
	const TCHAR* DefaultShapeMaterialPath = TEXT("/DwoCanvas/Materials/M_DwoCanvasShape.M_DwoCanvasShape");

	UMaterialInterface* GetDefaultShapeMaterial()
	{
		static TWeakObjectPtr<UMaterialInterface> Cached;
		if (!Cached.IsValid())
		{
			Cached = LoadObject<UMaterialInterface>(nullptr, DefaultShapeMaterialPath);
		}
		return Cached.Get();
	}
}

bool FDwoCanvasShapeStyle::NeedsMaterial() const
{
	const bool bGradient = FillType != EDwoCanvasFillType::None && FillType != EDwoCanvasFillType::Solid;
	const bool bShadow = DropShadow.bEnabled || InnerShadow.bEnabled;
	// Slate's rounded-box outline over-brightens the arc of a translucent stroke (e.g. 8% white: ~120 instead of ~65 on a
	// dark fill), so those go through the SDF material as well
	const bool bRounded = FMath::Max(FMath::Max(CornerRadii.X, CornerRadii.Y), FMath::Max(CornerRadii.Z, CornerRadii.W)) > 0.f;
	const bool bComplexStroke = StrokeWidth > 0.f
		&& (StrokeAlign != EDwoCanvasStrokeAlign::Inside || DashLength > 0.f || (bRounded && StrokeColor.A < 1.f));
	return bGradient || bShadow || bComplexStroke;
}

float FDwoCanvasShapeStyle::GetOverflow() const
{
	float Overflow = 0.f;
	if (DropShadow.bEnabled)
	{
		// Gaussian with sigma = Blur / 2 (Figma / CSS): visible out to ~3 sigma = 1.5 x Blur
		Overflow = FMath::Max(Overflow, DropShadow.Blur * 1.5f + FMath::Max(DropShadow.Spread, 0.f) + FMath::Max(FMath::Abs(DropShadow.Offset.X), FMath::Abs(DropShadow.Offset.Y)));
	}
	if (StrokeWidth > 0.f)
	{
		if (StrokeAlign == EDwoCanvasStrokeAlign::Outside)
		{
			Overflow = FMath::Max(Overflow, StrokeWidth);
		}
		else if (StrokeAlign == EDwoCanvasStrokeAlign::Center)
		{
			Overflow = FMath::Max(Overflow, StrokeWidth * 0.5f);
		}
	}
	return Overflow;
}

FSlateBrush FDwoCanvasShapeStyle::MakeSimpleBrush() const
{
	FSlateBrush Brush;
	Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
	Brush.TintColor = FSlateColor(FillType == EDwoCanvasFillType::Solid ? FillColor : FLinearColor::Transparent);
	Brush.OutlineSettings = FSlateBrushOutlineSettings(CornerRadii, FSlateColor(StrokeColor), StrokeWidth);
	Brush.OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
	return Brush;
}

void FDwoCanvasShapeStyle::ApplyToMaterial(UMaterialInstanceDynamic* Material) const
{
	if (!Material)
	{
		return;
	}

	float FillMode = 0.f;
	switch (FillType)
	{
	case EDwoCanvasFillType::None: FillMode = 0.f; break;
	case EDwoCanvasFillType::Solid: FillMode = 1.f; break;
	case EDwoCanvasFillType::Linear: FillMode = 2.f; break;
	case EDwoCanvasFillType::Radial: FillMode = 3.f; break;
	case EDwoCanvasFillType::Angular: FillMode = 4.f; break;
	case EDwoCanvasFillType::Diamond: FillMode = 5.f; break;
	}
	Material->SetScalarParameterValue(TEXT("FillType"), FillMode);

	// Solid fill uses stop 0
	TArray<FDwoCanvasGradientStop> Stops = GradientStops;
	if (FillType == EDwoCanvasFillType::Solid || Stops.Num() == 0)
	{
		Stops = { { 0.f, FillColor } };
	}
	Stops.Sort([](const FDwoCanvasGradientStop& A, const FDwoCanvasGradientStop& B) { return A.Position < B.Position; });

	FLinearColor StopPositions(1.f, 1.f, 1.f, 1.f);
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const FDwoCanvasGradientStop& Stop = Stops[FMath::Min(Index, Stops.Num() - 1)];
		Material->SetVectorParameterValue(*FString::Printf(TEXT("Stop%d"), Index), Stop.Color);
		StopPositions.Component(Index) = Stop.Position;
	}
	Material->SetVectorParameterValue(TEXT("StopPositions"), StopPositions);
	Material->SetScalarParameterValue(TEXT("StopCount"), FMath::Min(Stops.Num(), 4));

	Material->SetVectorParameterValue(TEXT("GradientHandles"), FLinearColor(GradientStart.X, GradientStart.Y, GradientEnd.X, GradientEnd.Y));
	Material->SetVectorParameterValue(TEXT("GradientWidth"), FLinearColor(GradientWidth.X, GradientWidth.Y, 0.f, 0.f));

	Material->SetVectorParameterValue(TEXT("Radii"), FLinearColor(CornerRadii.X, CornerRadii.Y, CornerRadii.Z, CornerRadii.W));

	const float Align = StrokeAlign == EDwoCanvasStrokeAlign::Inside ? 0.f : (StrokeAlign == EDwoCanvasStrokeAlign::Center ? 1.f : 2.f);
	Material->SetVectorParameterValue(TEXT("StrokeColor"), StrokeColor);
	Material->SetVectorParameterValue(TEXT("StrokeParams"), FLinearColor(StrokeWidth, Align, DashLength, DashGap));

	Material->SetVectorParameterValue(TEXT("DropColor"), DropShadow.bEnabled ? DropShadow.Color : FLinearColor::Transparent);
	Material->SetVectorParameterValue(TEXT("DropParams"), FLinearColor(DropShadow.Offset.X, DropShadow.Offset.Y, DropShadow.Blur, DropShadow.Spread));
	Material->SetVectorParameterValue(TEXT("InnerColor"), InnerShadow.bEnabled ? InnerShadow.Color : FLinearColor::Transparent);
	Material->SetVectorParameterValue(TEXT("InnerParams"), FLinearColor(InnerShadow.Offset.X, InnerShadow.Offset.Y, InnerShadow.Blur, InnerShadow.Spread));
}

bool FDwoCanvasShapeStyle::operator==(const FDwoCanvasShapeStyle& Other) const
{
	auto ShadowEquals = [](const FDwoCanvasShadow& A, const FDwoCanvasShadow& B)
	{
		return A.bEnabled == B.bEnabled && A.Color == B.Color && A.Offset == B.Offset && A.Blur == B.Blur && A.Spread == B.Spread
			&& A.bShowBehindFill == B.bShowBehindFill;
	};

	if (GradientStops.Num() != Other.GradientStops.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < GradientStops.Num(); ++Index)
	{
		if (GradientStops[Index].Position != Other.GradientStops[Index].Position || GradientStops[Index].Color != Other.GradientStops[Index].Color)
		{
			return false;
		}
	}

	return FillType == Other.FillType && FillColor == Other.FillColor
		&& GradientStart == Other.GradientStart && GradientEnd == Other.GradientEnd && GradientWidth == Other.GradientWidth
		&& CornerRadii == Other.CornerRadii
		&& StrokeColor == Other.StrokeColor && StrokeWidth == Other.StrokeWidth && StrokeAlign == Other.StrokeAlign
		&& DashLength == Other.DashLength && DashGap == Other.DashGap
		&& ShadowEquals(DropShadow, Other.DropShadow) && ShadowEquals(InnerShadow, Other.InnerShadow);
}
