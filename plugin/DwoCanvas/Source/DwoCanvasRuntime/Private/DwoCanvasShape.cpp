// DwoCanvas - UMG shape widget

#include "DwoCanvasShape.h"

#include "SDwoCanvasShape.h"

#define LOCTEXT_NAMESPACE "DwoCanvas"

TSharedRef<SWidget> UDwoCanvasShape::RebuildWidget()
{
	MyShape = SNew(SDwoCanvasShape)
		.Style(Style)
		.DesiredSize(DesiredSize)
		.Material(ShapeMaterial);
	return MyShape.ToSharedRef();
}

void UDwoCanvasShape::SynchronizeProperties()
{
	Super::SynchronizeProperties();

	if (MyShape.IsValid())
	{
		MyShape->SetMaterial(ShapeMaterial);
		MyShape->SetStyle(Style);
		MyShape->SetDesiredShapeSize(DesiredSize);
	}
}

void UDwoCanvasShape::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	MyShape.Reset();
}

void UDwoCanvasShape::SetStyle(const FDwoCanvasShapeStyle& InStyle)
{
	Style = InStyle;
	if (MyShape.IsValid())
	{
		MyShape->SetStyle(Style);
	}
}

void UDwoCanvasShape::SetDesiredSize(FVector2D InSize)
{
	DesiredSize = InSize;
	if (MyShape.IsValid())
	{
		MyShape->SetDesiredShapeSize(DesiredSize);
	}
}

#if WITH_EDITOR
const FText UDwoCanvasShape::GetPaletteCategory()
{
	return LOCTEXT("PaletteCategory", "DwoCanvas");
}
#endif

#undef LOCTEXT_NAMESPACE
