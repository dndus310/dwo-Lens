#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "DwoLensInspectorCommandlet.generated.h"

/**
 * Read-only extractor. Writes reflection, Blueprint graph and asset reference data as JSON.
 *
 * Usage:
 *   UnrealEditor-Cmd.exe <Project>.uproject -run=DwoLensInspector -LensOut=<dir> [-LensContentRoots=/Game,/MyPlugin]
 *
 * Never saves packages and never modifies project files.
 */
UCLASS()
class UDwoLensInspectorCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UDwoLensInspectorCommandlet();

	virtual int32 Main(const FString& Params) override;
};
