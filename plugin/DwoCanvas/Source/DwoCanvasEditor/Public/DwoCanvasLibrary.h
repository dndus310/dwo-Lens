// DwoCanvas - Blueprint / Python entry points and the import commandlet

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "DwoCanvasLibrary.generated.h"

UCLASS()
class DWOCANVASEDITOR_API UDwoCanvasLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Imports a designbridge/1 JSON file. Returns the report text (also saved under Saved/DwoCanvas/Reports).
	 * Python: unreal.DwoCanvasLibrary.import_design_file("C:/.../export.json", False)
	 */
	UFUNCTION(BlueprintCallable, Category = "DwoCanvas")
	static FString ImportDesignFile(const FString& JsonFile, bool bResetOverrides, bool& bSucceeded);
};

/**
 * UnrealEditor-Cmd.exe Project.uproject -run=DwoCanvasImport -File="C:/.../export.json" [-ResetOverrides] [-Doc=Name]
 * Exit code 0 = no errors.
 */
UCLASS()
class UDwoCanvasImportCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UDwoCanvasImportCommandlet();
	virtual int32 Main(const FString& Params) override;
};
