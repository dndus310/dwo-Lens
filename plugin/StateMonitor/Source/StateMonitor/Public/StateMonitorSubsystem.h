#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "StateMonitorModel.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "StateMonitorSubsystem.generated.h"

class SWidget;

/**
 * @brief 게임 인스턴스 하나가 공유하는 상태 모니터 모델과 화면 표시를 관리한다.
 * 어느 코드에서든 Get(WorldContext)로 찾아 스냅샷을 제출하고, ShowMonitor로 뷰포트에 띄운다.
 * 레벨이 바뀌어도 모델과 이력은 유지된다. 모든 함수는 게임 스레드에서만 호출한다.
 * 비 Shipping 빌드에서는 dwo-Lens 뷰어(127.0.0.1:47731)에 Live로 붙어 소스를 스트리밍하고, 뷰어가 켜면
 * GameMode, GameState, 로컬 PlayerController/PlayerState/Pawn을 코드 없이 자동 관찰한다.
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

	/**
	 * @brief UObject 하나의 reflected 프로퍼티를 제출한다. 기본으로 프로젝트 코드(Blueprint, 엔진 밖 모듈)가 선언한
	 * 프로퍼티만 담는다. Label이 비면 Source ID를 표시한다.
	 */
	bool SubmitObjectSnapshot(FName SourceId, const FText& Label, const UObject* Object);

	/**
	 * @brief Object를 자동 관찰 목록에 넣는다. 뷰어가 자동 관찰을 켰거나 모니터가 보이는 동안 주기적으로 스냅샷을 찍고,
	 * 객체가 사라지면 소스도 지운다. Source ID는 액터 이름, 컴포넌트는 "<액터>.<컴포넌트>"다.
	 */
	UFUNCTION(BlueprintCallable, Category = "State Monitor")
	bool WatchObject(UObject* Object);

	UFUNCTION(BlueprintCallable, Category = "State Monitor")
	void UnwatchObject(UObject* Object);

	UFUNCTION(BlueprintPure, Category = "State Monitor")
	bool IsWatchingObject(const UObject* Object) const;

	/** @brief dwo-Lens 뷰어와 Live 연결 중인지. */
	UFUNCTION(BlueprintPure, Category = "State Monitor")
	bool IsLiveConnected() const;

	/** @brief 자동 관찰 소스의 ID. 수동 Submit 소스와 구분할 때 쓴다. */
	bool IsAutoSource(FName SourceId) const;

	/**
	 * @brief 자동 관찰을 한 번 실행한다. Live 뷰어가 켜면 Interval마다 자동으로 불린다.
	 * bObserveWorld가 false이면 Watch로 넣은 객체만 찍는다. 테스트와 도구에서 직접 부를 수 있다.
	 */
	void UpdateAutoSources(bool bObserveWorld);

	/** @brief 자동 관찰 옵션. 기본은 프로젝트 프로퍼티만, 액터 클래스 감시 없음. */
	void SetAutoObserveOptions(bool bIncludeEngineProperties);

	/** @brief 클래스 이름(예: BP_Enemy_C, 부모 클래스 이름도 일치)으로 액터를 최대 Max개까지 자동 관찰한다. */
	void WatchActorClass(const FString& ClassName, int32 Max);

	void UnwatchActorClass(const FString& ClassName);

private:
	TSharedRef<SWidget> CreateOverlay();

	bool TickLive(float DeltaTime);

	void HandleLiveCommand(const FString& Line);

	void SendLiveState(bool bAll);

	void SendHello();

	FString GetCurrentWorldName() const;

	bool CaptureAutoSource(FName SourceId, const UObject* Object, TSet<FName>& Desired);

	void CaptureAutoComponents(const FString& Prefix, const class AActor* Actor, TSet<FName>& Desired);

private:
	TSharedPtr<FStateMonitorModel> Model;
	TSharedPtr<SWidget> Overlay;
	TSharedPtr<class FStateMonitorLiveLink> LiveLink;
	FTSTicker::FDelegateHandle LiveTickerHandle;
	TArray<TWeakObjectPtr<UObject>> WatchedObjects;
	TMap<FString, int32> WatchedClasses;
	TSet<FName> AutoSourceIds;
	TMap<FName, uint64> SentSequences;
	FString LiveWorldName;
	double AutoInterval;
	double NextAutoTime;
	double NextSendTime;
	bool bAutoObserve;
	bool bAutoEngineProperties;
	bool bLivePaused;
	bool bResendAll;
};

template <typename TStruct>
bool UStateMonitorSubsystem::Submit(FName SourceId, const TStruct& Snapshot, const FText& Label)
{
	return SubmitStructSnapshot(SourceId, Label, TStruct::StaticStruct(), &Snapshot);
}
