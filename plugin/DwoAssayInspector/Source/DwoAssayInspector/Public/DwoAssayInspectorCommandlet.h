#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "DwoAssayInspectorCommandlet.generated.h"

/**
 * Read-only extractor. Writes reflection, Blueprint graph and asset reference data as JSON.
 *
 * Usage:
 *   UnrealEditor-Cmd.exe <Project>.uproject -run=DwoAssayInspector -AssayOut=<dir> [-AssayContentRoots=/Game,/MyPlugin]
 *
 * Never saves packages and never modifies project files.
 */
UCLASS()
class UDwoAssayInspectorCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UDwoAssayInspectorCommandlet();

	virtual int32 Main(const FString& Params) override;
};
