// DwoCanvas - auto-layout frame: background + padding + gap + direction in one panel

#pragma once

#include "CoreMinimal.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/SlateWrapperTypes.h"
#include "Layout/Margin.h"
#include "DwoCanvasShapeStyle.h"
#include "DwoCanvasFrame.generated.h"

class SBox;
class SDwoCanvasShape;
class SOverlay;
class SPanel;
class UMaterialInterface;

UENUM(BlueprintType)
enum class EDwoCanvasFrameDirection : uint8
{
	Horizontal,
	Vertical,
	Wrap
};

UENUM(BlueprintType)
enum class EDwoCanvasMainAlign : uint8
{
	Start,
	Center,
	End,
	SpaceBetween
};

UCLASS()
class DWOCANVASRUNTIME_API UDwoCanvasFrameSlot : public UPanelSlot
{
	GENERATED_BODY()

public:
	/** Added to the gap / frame padding around this child. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout|Canvas Frame Slot")
	FMargin Padding;

	/** Auto = hug the child, Fill = take the remaining space (Figma "Fill container"). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout|Canvas Frame Slot")
	FSlateChildSize Size = FSlateChildSize(ESlateSizeRule::Automatic);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout|Canvas Frame Slot")
	TEnumAsByte<EHorizontalAlignment> HorizontalAlignment = HAlign_Fill;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout|Canvas Frame Slot")
	TEnumAsByte<EVerticalAlignment> VerticalAlignment = VAlign_Fill;

	UFUNCTION(BlueprintCallable, Category = "Layout|Canvas Frame Slot")
	void SetPadding(FMargin InPadding);

	UFUNCTION(BlueprintCallable, Category = "Layout|Canvas Frame Slot")
	void SetSize(FSlateChildSize InSize);

	UFUNCTION(BlueprintCallable, Category = "Layout|Canvas Frame Slot")
	void SetHorizontalAlignment(EHorizontalAlignment InAlignment);

	UFUNCTION(BlueprintCallable, Category = "Layout|Canvas Frame Slot")
	void SetVerticalAlignment(EVerticalAlignment InAlignment);

	virtual void SynchronizeProperties() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

private:
	void RequestParentRebuild();
};

/**
 * Figma auto-layout frame as a single widget: optional shape background, padding, gap,
 * main-axis alignment (incl. space-between), wrap, fixed / min / max size.
 * Replaces the Border + SizeBox + Box + Spacer stack the native mapping would need.
 */
UCLASS(meta = (DisplayName = "Canvas Frame"))
class DWOCANVASRUNTIME_API UDwoCanvasFrame : public UPanelWidget
{
	GENERATED_BODY()

public:
	UDwoCanvasFrame();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout")
	EDwoCanvasFrameDirection Direction = EDwoCanvasFrameDirection::Vertical;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout")
	float Gap = 0.f;

	/** Gap between wrapped lines. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout", meta = (EditCondition = "Direction == EDwoCanvasFrameDirection::Wrap"))
	float CrossGap = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout")
	FMargin Padding;

	/** Ignored when a child fills the main axis. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout")
	EDwoCanvasMainAlign MainAlign = EDwoCanvasMainAlign::Start;

	/** 0 = not set. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout|Size", meta = (ClampMin = 0))
	float WidthOverride = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout|Size", meta = (ClampMin = 0))
	float HeightOverride = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout|Size", meta = (ClampMin = 0))
	float MinWidth = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout|Size", meta = (ClampMin = 0))
	float MaxWidth = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout|Size", meta = (ClampMin = 0))
	float MinHeight = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout|Size", meta = (ClampMin = 0))
	float MaxHeight = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Appearance")
	bool bDrawBackground = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Appearance", meta = (EditCondition = "bDrawBackground"))
	FDwoCanvasShapeStyle Background;

	/** Empty = /DwoCanvas/Materials/M_DwoCanvasShape. Only loaded when the background needs it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Appearance", meta = (EditCondition = "bDrawBackground"))
	TObjectPtr<UMaterialInterface> ShapeMaterial;

	UFUNCTION(BlueprintCallable, Category = "Appearance")
	void SetBackground(const FDwoCanvasShapeStyle& InBackground);

	UFUNCTION(BlueprintCallable, Category = "Layout")
	void SetGap(float InGap);

	UFUNCTION(BlueprintCallable, Category = "Layout")
	void SetFramePadding(FMargin InPadding);

	/** Re-creates the Slate children (gaps / spacers depend on all slots). */
	void RebuildChildren();

	virtual void SynchronizeProperties() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

#if WITH_EDITOR
	virtual const FText GetPaletteCategory() override;
#endif

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual UClass* GetSlotClass() const override;
	virtual void OnSlotAdded(UPanelSlot* InSlot) override;
	virtual void OnSlotRemoved(UPanelSlot* InSlot) override;

private:
	TSharedPtr<SBox> MySizeBox;
	TSharedPtr<SOverlay> MyOverlay;
	TSharedPtr<SDwoCanvasShape> MyBackground;
	TSharedPtr<SPanel> MyLayoutPanel;
	EDwoCanvasFrameDirection BuiltDirection = EDwoCanvasFrameDirection::Vertical;
};
