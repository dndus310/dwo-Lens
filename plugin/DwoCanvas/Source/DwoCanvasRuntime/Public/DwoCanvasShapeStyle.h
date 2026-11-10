// DwoCanvas - shape style shared by UDwoCanvasShape and UDwoCanvasFrame backgrounds

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateBrush.h"
#include "DwoCanvasShapeStyle.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;

UENUM(BlueprintType)
enum class EDwoCanvasFillType : uint8
{
	None,
	Solid,
	Linear,
	Radial,
	Angular,
	Diamond
};

UENUM(BlueprintType)
enum class EDwoCanvasStrokeAlign : uint8
{
	Inside,
	Center,
	Outside
};

USTRUCT(BlueprintType)
struct DWOCANVASRUNTIME_API FDwoCanvasGradientStop
{
	GENERATED_BODY()

	/** 0..1 along the gradient. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gradient", meta = (ClampMin = 0, ClampMax = 1))
	float Position = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gradient")
	FLinearColor Color = FLinearColor::White;
};

USTRUCT(BlueprintType)
struct DWOCANVASRUNTIME_API FDwoCanvasShadow
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow")
	bool bEnabled = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow")
	FLinearColor Color = FLinearColor(0.f, 0.f, 0.f, 0.25f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow")
	FVector2D Offset = FVector2D(0.f, 4.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow", meta = (ClampMin = 0))
	float Blur = 8.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow")
	float Spread = 0.f;

	/** Drop shadow only: also draw the shadow under the shape, so it shows through a translucent fill (Figma "Show behind transparent areas"). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow")
	bool bShowBehindFill = false;
};

/**
 * Rounded rectangle with fill, stroke and shadows (the Figma rectangle / frame background model).
 * Simple styles (solid fill, inside stroke, no shadow) draw with a Slate rounded-box brush;
 * everything else uses the M_DwoCanvasShape SDF material. The cheap path is picked automatically.
 */
USTRUCT(BlueprintType)
struct DWOCANVASRUNTIME_API FDwoCanvasShapeStyle
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fill")
	EDwoCanvasFillType FillType = EDwoCanvasFillType::Solid;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fill", meta = (EditCondition = "FillType == EDwoCanvasFillType::Solid"))
	FLinearColor FillColor = FLinearColor::White;

	/** Up to 4 stops are used. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fill", meta = (EditCondition = "FillType != EDwoCanvasFillType::Solid && FillType != EDwoCanvasFillType::None"))
	TArray<FDwoCanvasGradientStop> GradientStops;

	/** Gradient handles in 0..1 of the shape size (Figma gradient handle positions). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fill")
	FVector2D GradientStart = FVector2D(0.f, 0.5f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fill")
	FVector2D GradientEnd = FVector2D(1.f, 0.5f);

	/** Second axis for radial / angular / diamond gradients. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fill")
	FVector2D GradientWidth = FVector2D(0.5f, 1.f);

	/** Top-left, top-right, bottom-right, bottom-left. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape")
	FVector4 CornerRadii = FVector4(0.f, 0.f, 0.f, 0.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stroke")
	FLinearColor StrokeColor = FLinearColor::Black;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stroke", meta = (ClampMin = 0))
	float StrokeWidth = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stroke")
	EDwoCanvasStrokeAlign StrokeAlign = EDwoCanvasStrokeAlign::Inside;

	/** 0 = solid stroke. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stroke", meta = (ClampMin = 0))
	float DashLength = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stroke", meta = (ClampMin = 0))
	float DashGap = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effects")
	FDwoCanvasShadow DropShadow;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effects")
	FDwoCanvasShadow InnerShadow;

	/** True when the Slate rounded-box brush cannot draw this style. */
	bool NeedsMaterial() const;

	/** How far the drawing extends outside the shape bounds (shadow, outside stroke). */
	float GetOverflow() const;

	/** Rounded-box brush for the simple path. */
	FSlateBrush MakeSimpleBrush() const;

	/** Writes every parameter except the quad size (set at paint time). */
	void ApplyToMaterial(UMaterialInstanceDynamic* Material) const;

	bool operator==(const FDwoCanvasShapeStyle& Other) const;
	bool operator!=(const FDwoCanvasShapeStyle& Other) const { return !(*this == Other); }
};

namespace DwoCanvas
{
	/** /DwoCanvas/Materials/M_DwoCanvasShape - the SDF shape material shipped with the plugin. */
	DWOCANVASRUNTIME_API UMaterialInterface* GetDefaultShapeMaterial();

	DWOCANVASRUNTIME_API extern const TCHAR* DefaultShapeMaterialPath;
}
