#pragma once

#include "CoreMinimal.h"
#include "Components/Widget.h"
#include "StateMonitorWidget.generated.h"

class FStateMonitorModel;
class SStateMonitor;
class UScriptStruct;

/** Embeddable, read-only runtime monitor. Submit reflected value copies from the owning application. */
UCLASS(BlueprintType, meta = (DisplayName = "State Monitor"))
class STATEMONITOR_API UStateMonitorWidget : public UWidget
{
	GENERATED_BODY()

public:
	UStateMonitorWidget(const FObjectInitializer& ObjectInitializer);

	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

	/** Blueprint accepts any reflected struct. Native callers use SubmitStructSnapshot. */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "State Monitor",
	          meta = (CustomStructureParam = "Snapshot", AutoCreateRefTerm = "Snapshot"))
	bool SubmitSnapshot(FName SourceId, FText Label, const int32& Snapshot);

	DECLARE_FUNCTION(execSubmitSnapshot);

	UFUNCTION(BlueprintCallable, Category = "State Monitor")
	bool RemoveSource(FName SourceId);

	UFUNCTION(BlueprintCallable, Category = "State Monitor")
	void ClearHistory(FName SourceId);

	UFUNCTION(BlueprintCallable, Category = "State Monitor")
	void ResetBaseline(FName SourceId);

	UFUNCTION(BlueprintCallable, Category = "State Monitor")
	void ResetMonitor();

	bool SubmitStructSnapshot(FName SourceId, const FText& Label, const UScriptStruct* StructType,
	                          const void* StructData);

	TSharedPtr<FStateMonitorModel> GetMonitorModel();

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

public:
	/** @brief true면 게임 인스턴스의 UStateMonitorSubsystem 모델을 표시한다. UStateMonitorLibrary로 제출한 값이 이 위젯에 보인다. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "State Monitor")
	bool bUseSharedModel;

private:
	TSharedPtr<FStateMonitorModel> MonitorModel;
	TSharedPtr<SStateMonitor> MonitorView;
};
