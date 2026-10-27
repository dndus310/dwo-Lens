#pragma once

#include "CoreMinimal.h"
#include "StateMonitorModel.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "StateMonitorSubsystem.generated.h"

class SWidget;

/**
 * @brief 게임 인스턴스 하나가 공유하는 상태 모니터 모델과 화면 표시를 관리한다.
 * 어느 코드에서든 Get(WorldContext)로 찾아 스냅샷을 제출하고, ShowMonitor로 뷰포트에 띄운다.
 * 레벨이 바뀌어도 모델과 이력은 유지된다. 모든 함수는 게임 스레드에서만 호출한다.
 */
UCLASS()
class STATEMONITOR_API UStateMonitorSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	UStateMonitorSubsystem();

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** @brief 이전에 제출한 같은 Source ID의 상태를 지운다. */
	UFUNCTION(BlueprintCallable, Category = "State Monitor")
	bool RemoveSource(FName SourceId);

	UFUNCTION(BlueprintCallable, Category = "State Monitor")
	void ClearHistory(FName SourceId);

	UFUNCTION(BlueprintCallable, Category = "State Monitor")
	void ResetBaseline(FName SourceId);

	UFUNCTION(BlueprintCallable, Category = "State Monitor")
	void ResetMonitor();

	/** @brief 게임 뷰포트 오른쪽에 모니터를 띄운다. 뷰포트가 없는 환경(NullRHI, 서버)에서는 false를 반환한다. */
	UFUNCTION(BlueprintCallable, Category = "State Monitor")
	bool ShowMonitor();

	UFUNCTION(BlueprintCallable, Category = "State Monitor")
	void HideMonitor();

	UFUNCTION(BlueprintCallable, Category = "State Monitor")
	bool ToggleMonitor();

	UFUNCTION(BlueprintPure, Category = "State Monitor")
	bool IsMonitorVisible() const;

	/** @brief WorldContext가 속한 게임 인스턴스의 서브시스템. 게임 인스턴스가 없으면 nullptr. */
	static UStateMonitorSubsystem* Get(const UObject* WorldContextObject);
	/** @brief StructData는 StructType의 유효한 인스턴스여야 하며, 호출이 끝나면 다시 참조하지 않는다. Label이 비면 Source ID를 표시한다. */
	bool SubmitStructSnapshot(FName SourceId, const FText& Label, const UScriptStruct* StructType, const void* StructData);
	/** @brief USTRUCT 값 하나를 제출한다. 예: Monitor->Submit(TEXT("Player"), PlayerState); */
	template <typename TStruct>
	bool Submit(FName SourceId, const TStruct& Snapshot, const FText& Label = FText());
	TSharedPtr<FStateMonitorModel> GetModel() const;

private:
	TSharedRef<SWidget> CreateOverlay();

private:
	TSharedPtr<FStateMonitorModel> Model;
	TSharedPtr<SWidget> Overlay;
};

template <typename TStruct>
bool UStateMonitorSubsystem::Submit(FName SourceId, const TStruct& Snapshot, const FText& Label)
{
	return SubmitStructSnapshot(SourceId, Label, TStruct::StaticStruct(), &Snapshot);
}
