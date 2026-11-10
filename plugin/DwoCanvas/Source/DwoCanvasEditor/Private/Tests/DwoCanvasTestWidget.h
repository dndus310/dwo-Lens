// DwoCanvas - C++ parent used by the binding tests

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DwoCanvasTestWidget.generated.h"

class UButton;
class UTextBlock;

UCLASS(Abstract, HideDropdown, NotPlaceable)
class UDwoCanvasTestWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UTextBlock> Title;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UButton> Confirm;
};
