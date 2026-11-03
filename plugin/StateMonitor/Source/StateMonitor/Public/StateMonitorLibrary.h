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

	/**
	 * @brief Object를 자동 관찰 목록에 넣는다. dwo-Lens 뷰어가 자동 관찰을 켰거나 모니터가 보이는 동안 주기적으로
	 * 프로젝트 프로퍼티를 스냅샷하고, 객체가 사라지면 소스를 지운다. Source ID는 객체 이름(컴포넌트는 "<액터>.<컴포넌트>").
	 */
	UFUNCTION(BlueprintCallable, Category = "State Monitor", meta = (WorldContext = "WorldContextObject", DisplayName = "Watch State Object"))
	static bool WatchStateObject(const UObject* WorldContextObject, UObject* Object);

	UFUNCTION(BlueprintCallable, Category = "State Monitor", meta = (WorldContext = "WorldContextObject", DisplayName = "Unwatch State Object"))
	static void UnwatchStateObject(const UObject* WorldContextObject, UObject* Object);

	/** @brief Native 호출용. Blueprint의 SubmitStateSnapshot과 같은 동작을 한다. */
	static bool SubmitStructSnapshot(const UObject* WorldContextObject, FName SourceId, const FText& Label, const UScriptStruct* StructType, const void* StructData);
};
