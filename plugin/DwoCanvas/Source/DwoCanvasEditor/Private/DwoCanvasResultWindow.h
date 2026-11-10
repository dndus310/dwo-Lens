// DwoCanvas - import result window

#pragma once

#include "CoreMinimal.h"

struct FDwoCanvasReport;

class FDwoCanvasResultWindow
{
public:
	/**
	 * Shows the outcome of an import: overall status, what changed, the written blueprints
	 * (C++ bindings, match with the design preview), the items to check, and collapsed details.
	 */
	static void Show(const FDwoCanvasReport& Report, const FString& SourceFile, const FString& ReportFile, FSimpleDelegate OnReimport);
};
