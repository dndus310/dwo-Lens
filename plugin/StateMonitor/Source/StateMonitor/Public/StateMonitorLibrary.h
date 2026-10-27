#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "StateMonitorLibrary.generated.h"

/**
 * @brief 위젯이나 모델을 들고 있지 않아도 어디서든 상태를 제출하고 모니터를 띄우는 Blueprint 진입점.
 * 제출한 값은 게임 인스턴스의 UStateMonitorSubsystem에 모인다.
 */
UCLASS()
class STATEMONITOR_API UStateMonitorLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** @brief Snapshot 핀에 연결한 구조체의 reflected 필드를 제출한다. Label이 비면 Source ID를 표시한다. */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "State Monitor",
	          meta = (WorldContext = "WorldContextObject", CustomStructureParam = "Snapshot", AutoCreateRefTerm = "Label,Snapshot"))
	static bool SubmitStateSnapshot(const UObject* WorldContextObject, FName SourceId, const FText& Label, const int32& Snapshot);

	DECLARE_FUNCTION(execSubmitStateSnapshot);

	UFUNCTION(BlueprintCallable, Category = "State Monitor", meta = (WorldContext = "WorldContextObject"))
	static bool RemoveStateSource(const UObject* WorldContextObject, FName SourceId);

	UFUNCTION(BlueprintCallable, Category = "State Monitor", meta = (WorldContext = "WorldContextObject"))
	static void ClearStateHistory(const UObject* WorldContextObject, FName SourceId);

	UFUNCTION(BlueprintCallable, Category = "State Monitor", meta = (WorldContext = "WorldContextObject"))
	static bool ShowStateMonitor(const UObject* WorldContextObject);

	UFUNCTION(BlueprintCallable, Category = "State Monitor", meta = (WorldContext = "WorldContextObject"))
	static void HideStateMonitor(const UObject* WorldContextObject);

	UFUNCTION(BlueprintCallable, Category = "State Monitor", meta = (WorldContext = "WorldContextObject"))
	static bool ToggleStateMonitor(const UObject* WorldContextObject);

	/** @brief Native 호출용. Blueprint의 SubmitStateSnapshot과 같은 동작을 한다. */
	static bool SubmitStructSnapshot(const UObject* WorldContextObject, FName SourceId, const FText& Label, const UScriptStruct* StructType, const void* StructData);
};
