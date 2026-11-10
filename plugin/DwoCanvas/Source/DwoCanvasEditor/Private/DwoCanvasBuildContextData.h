// DwoCanvas - what the importer hands to FDwoCanvasBuildContext (internal)

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UPanelWidget;
class UWidget;

struct FDwoCanvasBuildContextData
{
	TSharedPtr<FJsonObject> Node;
	FString NodeId;
	FString Role;
	FString WidgetName;
	FString DocumentName;
	FVector2D Size = FVector2D::ZeroVector;

	TFunction<UWidget*(UClass*, const FString&, bool&)> Make;
	TFunction<void(UPanelWidget*)> BuildChildren;
	TFunction<FLinearColor(const TSharedPtr<FJsonObject>&)> Color;
	TFunction<void(bool /*bWarning*/, const FString&)> Log;
};
