#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "ArchiInspectorCommandlet.generated.h"

/**
 * Read-only extractor. Writes reflection, Blueprint graph and asset reference data as JSON.
 *
 * Usage:
 *   UnrealEditor-Cmd.exe <Project>.uproject -run=ArchiInspector -ArchiOut=<dir> [-ArchiContentRoots=/Game,/MyPlugin]
 *
 * Never saves packages and never modifies project files.
 */
UCLASS()
class UArchiInspectorCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UArchiInspectorCommandlet();

	virtual int32 Main(const FString& Params) override;
};
