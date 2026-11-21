// DwoCanvas - auto-layout frame

#include "DwoCanvasFrame.h"

#include "SDwoCanvasShape.h"

#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"

#define LOCTEXT_NAMESPACE "DwoCanvas"

// ---------------------------------------------------------------------------------------------------------------------
// Slot

void UDwoCanvasFrameSlot::RequestParentRebuild()
{
	if (UDwoCanvasFrame* Frame = Cast<UDwoCanvasFrame>(Parent))
	{
		Frame->RebuildChildren();
	}
}

void UDwoCanvasFrameSlot::SetPadding(FMargin InPadding)
{
	Padding = InPadding;
	RequestParentRebuild();
}

void UDwoCanvasFrameSlot::SetSize(FSlateChildSize InSize)
{
	Size = InSize;
	RequestParentRebuild();
}

void UDwoCanvasFrameSlot::SetHorizontalAlignment(EHorizontalAlignment InAlignment)
{
	HorizontalAlignment = InAlignment;
	RequestParentRebuild();
}

void UDwoCanvasFrameSlot::SetVerticalAlignment(EVerticalAlignment InAlignment)
{
	VerticalAlignment = InAlignment;
	RequestParentRebuild();
}

void UDwoCanvasFrameSlot::SynchronizeProperties()
{
	RequestParentRebuild();
}

void UDwoCanvasFrameSlot::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
}

// ---------------------------------------------------------------------------------------------------------------------
// Frame

UDwoCanvasFrame::UDwoCanvasFrame()
{
	bIsVariable = false;
	SetVisibilityInternal(ESlateVisibility::SelfHitTestInvisible);
}

UClass* UDwoCanvasFrame::GetSlotClass() const
{
	return UDwoCanvasFrameSlot::StaticClass();
}

void UDwoCanvasFrame::OnSlotAdded(UPanelSlot*)
{
	RebuildChildren();
}

void UDwoCanvasFrame::OnSlotRemoved(UPanelSlot*)
{
	RebuildChildren();
}

TSharedRef<SWidget> UDwoCanvasFrame::RebuildWidget()
{
	MyBackground = SNew(SDwoCanvasShape);
	MyOverlay = SNew(SOverlay);
	MySizeBox = SNew(SBox)[MyOverlay.ToSharedRef()];
	MyLayoutPanel.Reset();

	RebuildChildren();
	return MySizeBox.ToSharedRef();
}

void UDwoCanvasFrame::RebuildChildren()
{
	if (!MyOverlay.IsValid())
	{
		return;
	}

	const bool bHorizontal = Direction == EDwoCanvasFrameDirection::Horizontal;
	const bool bVertical = Direction == EDwoCanvasFrameDirection::Vertical;

	bool bAnyFill = false;
	for (const UPanelSlot* PanelSlot : Slots)
	{
		const UDwoCanvasFrameSlot* FrameSlot = Cast<UDwoCanvasFrameSlot>(PanelSlot);
		if (FrameSlot && FrameSlot->Size.SizeRule == ESlateSizeRule::Fill)
		{
			bAnyFill = true;
		}
	}
	const bool bSpaceBetween = MainAlign == EDwoCanvasMainAlign::SpaceBetween && !bAnyFill && Slots.Num() > 1;

	// Content placement on the main axis (cross axis always fills; children align themselves)
	EHorizontalAlignment ContentHAlign = HAlign_Fill;
	EVerticalAlignment ContentVAlign = VAlign_Fill;
	if (!bAnyFill && !bSpaceBetween)
	{
		if (bHorizontal)
		{
			ContentHAlign = MainAlign == EDwoCanvasMainAlign::Center ? HAlign_Center : (MainAlign == EDwoCanvasMainAlign::End ? HAlign_Right : HAlign_Left);
		}
		else if (bVertical)
		{
			ContentVAlign = MainAlign == EDwoCanvasMainAlign::Center ? VAlign_Center : (MainAlign == EDwoCanvasMainAlign::End ? VAlign_Bottom : VAlign_Top);
		}
	}

	// Layout panel
	TSharedPtr<SPanel> Panel;
	if (bHorizontal)
	{
		TSharedRef<SHorizontalBox> Box = SNew(SHorizontalBox);
		for (int32 Index = 0; Index < Slots.Num(); ++Index)
		{
			const UDwoCanvasFrameSlot* FrameSlot = Cast<UDwoCanvasFrameSlot>(Slots[Index]);
			if (!FrameSlot || !FrameSlot->Content)
			{
				continue;
			}
			if (bSpaceBetween && Index > 0)
			{
				Box->AddSlot().FillWidth(1.f)[SNew(SSpacer)];
			}
			FMargin SlotPadding = FrameSlot->Padding;
			SlotPadding.Left += (Index > 0 && !bSpaceBetween) ? Gap : 0.f;
			auto NewSlot = Box->AddSlot();
			NewSlot.Padding(SlotPadding).HAlign(FrameSlot->HorizontalAlignment).VAlign(FrameSlot->VerticalAlignment);
			if (FrameSlot->Size.SizeRule == ESlateSizeRule::Fill)
			{
				NewSlot.FillWidth(FrameSlot->Size.Value);
			}
			else
			{
				NewSlot.AutoWidth();
			}
			NewSlot[FrameSlot->Content->TakeWidget()];
		}
		Panel = Box;
	}
	else if (bVertical)
	{
		TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
		for (int32 Index = 0; Index < Slots.Num(); ++Index)
		{
			const UDwoCanvasFrameSlot* FrameSlot = Cast<UDwoCanvasFrameSlot>(Slots[Index]);
			if (!FrameSlot || !FrameSlot->Content)
			{
				continue;
			}
			if (bSpaceBetween && Index > 0)
			{
				Box->AddSlot().FillHeight(1.f)[SNew(SSpacer)];
			}
			FMargin SlotPadding = FrameSlot->Padding;
			SlotPadding.Top += (Index > 0 && !bSpaceBetween) ? Gap : 0.f;
			auto NewSlot = Box->AddSlot();
			NewSlot.Padding(SlotPadding).HAlign(FrameSlot->HorizontalAlignment).VAlign(FrameSlot->VerticalAlignment);
			if (FrameSlot->Size.SizeRule == ESlateSizeRule::Fill)
			{
				NewSlot.FillHeight(FrameSlot->Size.Value);
			}
			else
			{
				NewSlot.AutoHeight();
			}
			NewSlot[FrameSlot->Content->TakeWidget()];
		}
		Panel = Box;
	}
	else
	{
		TSharedRef<SWrapBox> Box = SNew(SWrapBox)
			.UseAllottedSize(true)
			.InnerSlotPadding(FVector2D(Gap, CrossGap))
			.Orientation(Orient_Horizontal);
		for (const UPanelSlot* PanelSlot : Slots)
		{
			const UDwoCanvasFrameSlot* FrameSlot = Cast<UDwoCanvasFrameSlot>(PanelSlot);
			if (!FrameSlot || !FrameSlot->Content)
			{
				continue;
			}
			Box->AddSlot()
				.Padding(FrameSlot->Padding)
				.HAlign(FrameSlot->HorizontalAlignment)
				.VAlign(FrameSlot->VerticalAlignment)
				.FillEmptySpace(FrameSlot->Size.SizeRule == ESlateSizeRule::Fill)
				[FrameSlot->Content->TakeWidget()];
		}
		Panel = Box;
	}

	MyLayoutPanel = Panel;
	BuiltDirection = Direction;

	MyOverlay->ClearChildren();
	if (bDrawBackground && MyBackground.IsValid())
	{
		MyBackground->SetMaterial(ShapeMaterial);
		MyBackground->SetStyle(Background);
		MyOverlay->AddSlot()[MyBackground.ToSharedRef()];
	}
	MyOverlay->AddSlot()
		.Padding(Padding)
		.HAlign(ContentHAlign)
		.VAlign(ContentVAlign)
		[Panel.ToSharedRef()];
}

void UDwoCanvasFrame::SynchronizeProperties()
{
	Super::SynchronizeProperties();

	if (MySizeBox.IsValid())
	{
		auto ToOptional = [](float Value) { return Value > 0.f ? FOptionalSize(Value) : FOptionalSize(); };
		MySizeBox->SetWidthOverride(ToOptional(WidthOverride));
		MySizeBox->SetHeightOverride(ToOptional(HeightOverride));
		MySizeBox->SetMinDesiredWidth(ToOptional(MinWidth));
		MySizeBox->SetMaxDesiredWidth(ToOptional(MaxWidth));
		MySizeBox->SetMinDesiredHeight(ToOptional(MinHeight));
		MySizeBox->SetMaxDesiredHeight(ToOptional(MaxHeight));
	}

	RebuildChildren();
}

void UDwoCanvasFrame::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	MySizeBox.Reset();
	MyOverlay.Reset();
	MyBackground.Reset();
	MyLayoutPanel.Reset();
}

void UDwoCanvasFrame::SetBackground(const FDwoCanvasShapeStyle& InBackground)
{
	Background = InBackground;
	bDrawBackground = true;
	RebuildChildren();
}

void UDwoCanvasFrame::SetGap(float InGap)
{
	Gap = InGap;
	RebuildChildren();
}

void UDwoCanvasFrame::SetFramePadding(FMargin InPadding)
{
	Padding = InPadding;
	RebuildChildren();
}

#if WITH_EDITOR
const FText UDwoCanvasFrame::GetPaletteCategory()
{
	return LOCTEXT("PaletteCategory", "DwoCanvas");
}
#endif

#undef LOCTEXT_NAMESPACE
