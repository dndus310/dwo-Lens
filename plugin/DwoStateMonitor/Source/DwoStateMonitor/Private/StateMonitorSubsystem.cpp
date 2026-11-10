#include "StateMonitorSubsystem.h"

#include "Components/ActorComponent.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Stats/Stats.h"
#include "SStateMonitor.h"
#include "StateMonitorLiveLink.h"
#include "StateMonitorLiveProtocol.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"

namespace StateMonitorSubsystem
{
/** 자동 관찰이 소스를 늘릴 수 있는 최대 개수. 수동 Submit을 위해 ReservedManualSources만큼 남긴다. */
constexpr int32 MaxSources = 128;
constexpr int32 ReservedManualSources = 16;
constexpr double SendInterval = 0.1;
/** 송신 버퍼가 이보다 크면 새 스냅샷을 만들지 않고 비워지기를 기다린다. */
constexpr int32 MaxPendingBytes = 512 * 1024;

FString NetModeName(const UWorld* World)
{
	switch (World ? World->GetNetMode() : NM_Standalone)
	{
	case NM_ListenServer:
		return TEXT("ListenServer");
	case NM_Client:
		return TEXT("Client");
	case NM_DedicatedServer:
		return TEXT("DedicatedServer");
	default:
		return TEXT("Standalone");
	}
}

bool ClassMatches(const UClass* Class, const FString& ClassName)
{
	for (; Class; Class = Class->GetSuperClass())
	{
		if (Class->GetName() == ClassName)
		{
			return true;
		}
	}
	return false;
}

FString WatchedSourceId(const UObject* Object)
{
	if (const UActorComponent* Component = Cast<UActorComponent>(Object))
	{
		if (const AActor* Owner = Component->GetOwner())
		{
			return Owner->GetName() + TEXT(".") + Component->GetName();
		}
	}
	return Object->GetName();
}
} // namespace StateMonitorSubsystem

UStateMonitorSubsystem::UStateMonitorSubsystem()
    : Model(), Overlay(), LiveLink(), LiveTickerHandle(), AutoInterval(0.25), NextAutoTime(0.0), NextSendTime(0.0),
      bAutoObserve(false), bAutoEngineProperties(false), bLivePaused(false), bResendAll(false)
{
}

void UStateMonitorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	FStateMonitorSettings Settings;
	Settings.MaxSources = StateMonitorSubsystem::MaxSources;
	Model = MakeShared<FStateMonitorModel>(Settings);

	// 게임 인스턴스마다 연결 하나. PIE 클라이언트가 여럿이면 각자 접속한다.
	if (!IsRunningCommandlet() && FStateMonitorLiveLink::IsEnabledByCommandLine())
	{
		LiveLink = MakeShared<FStateMonitorLiveLink>(FStateMonitorLiveLink::GetPortFromCommandLine());
	}
	LiveTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
	    FTickerDelegate::CreateUObject(this, &UStateMonitorSubsystem::TickLive), 0.0f);
}

void UStateMonitorSubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(LiveTickerHandle);
	LiveTickerHandle.Reset();
	LiveLink.Reset();
	WatchedObjects.Reset();
	WatchedClasses.Reset();
	AutoSourceIds.Reset();
	HideMonitor();
	Model.Reset();
	Super::Deinitialize();
}

bool UStateMonitorSubsystem::SubmitObjectSnapshot(FName SourceId, const FText& Label, const UObject* Object)
{
	const FText DisplayLabel = Label.IsEmpty() ? FText::FromName(SourceId) : Label;
	return Model.IsValid() && Model->SubmitObjectSnapshot(SourceId, DisplayLabel, Object);
}

bool UStateMonitorSubsystem::WatchObject(UObject* Object)
{
	if (!IsValid(Object))
	{
		return false;
	}
	if (!IsWatchingObject(Object))
	{
		WatchedObjects.Add(Object);
	}
	NextAutoTime = 0.0;
	return true;
}

void UStateMonitorSubsystem::UnwatchObject(UObject* Object)
{
	WatchedObjects.RemoveAll(
	    [Object](const TWeakObjectPtr<UObject>& Watched)
	    {
		    return !Watched.IsValid() || Watched.Get() == Object;
	    });
	NextAutoTime = 0.0;
}

bool UStateMonitorSubsystem::IsWatchingObject(const UObject* Object) const
{
	return Object && WatchedObjects.ContainsByPredicate(
	                     [Object](const TWeakObjectPtr<UObject>& Watched)
	                     {
		                     return Watched.Get() == Object;
	                     });
}

bool UStateMonitorSubsystem::IsLiveConnected() const
{
	return LiveLink.IsValid() && LiveLink->IsConnected();
}

bool UStateMonitorSubsystem::IsAutoSource(FName SourceId) const
{
	return AutoSourceIds.Contains(SourceId);
}

void UStateMonitorSubsystem::SetAutoObserveOptions(bool bIncludeEngineProperties)
{
	bAutoEngineProperties = bIncludeEngineProperties;
}

void UStateMonitorSubsystem::WatchActorClass(const FString& ClassName, int32 Max)
{
	if (!ClassName.IsEmpty())
	{
		WatchedClasses.Add(ClassName, FMath::Clamp(Max, 1, 256));
		NextAutoTime = 0.0;
	}
}

void UStateMonitorSubsystem::UnwatchActorClass(const FString& ClassName)
{
	WatchedClasses.Remove(ClassName);
	NextAutoTime = 0.0;
}

bool UStateMonitorSubsystem::CaptureAutoSource(FName SourceId, const UObject* Object, TSet<FName>& Desired)
{
	if (!Model.IsValid() || !IsValid(Object) || Desired.Contains(SourceId))
	{
		return false;
	}
	const bool bKnownAuto = AutoSourceIds.Contains(SourceId);
	if (!bKnownAuto)
	{
		// 같은 ID의 수동 Submit 소스는 덮어쓰지 않는다.
		if (Model->FindSource(SourceId).IsValid())
		{
			return false;
		}
		const int32 Limit = Model->GetSettings().MaxSources - StateMonitorSubsystem::ReservedManualSources;
		if (Model->GetSources().Num() >= Limit)
		{
			return false;
		}
	}
	if (!Model->SubmitObjectSnapshot(SourceId, FText::FromString(Object->GetName()), Object, true, bAutoEngineProperties))
	{
		return false;
	}
	AutoSourceIds.Add(SourceId);
	Desired.Add(SourceId);
	return true;
}

void UStateMonitorSubsystem::CaptureAutoComponents(const FString& Prefix, const AActor* Actor, TSet<FName>& Desired)
{
	if (!IsValid(Actor))
	{
		return;
	}
	TInlineComponentArray<UActorComponent*> Components(Actor);
	for (const UActorComponent* Component : Components)
	{
		if (IsValid(Component) &&
		    FStateMonitorModel::CountIncludedProperties(Component->GetClass(), bAutoEngineProperties) > 0)
		{
			CaptureAutoSource(FName(*(Prefix + TEXT(".") + Component->GetName())), Component, Desired);
		}
	}
}

void UStateMonitorSubsystem::UpdateAutoSources(bool bObserveWorld)
{
	if (!Model.IsValid())
	{
		return;
	}

	TSet<FName> Desired;
	UGameInstance* GameInstance = GetGameInstance();
	UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
	if (bObserveWorld && World)
	{
		CaptureAutoSource(TEXT("GameMode"), World->GetAuthGameMode(), Desired);
		CaptureAutoSource(TEXT("GameState"), World->GetGameState(), Desired);

		int32 LocalIndex = 0;
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			const APlayerController* Controller = It->Get();
			if (!IsValid(Controller) || !Controller->IsLocalController())
			{
				continue;
			}
			const FString Prefix = FString::Printf(TEXT("PC%d"), LocalIndex++);
			CaptureAutoSource(FName(*Prefix), Controller, Desired);
			CaptureAutoSource(FName(*(Prefix + TEXT(".PlayerState"))), Controller->PlayerState, Desired);
			if (const APawn* Pawn = Controller->GetPawn())
			{
				const FString PawnPrefix = Prefix + TEXT(".Pawn");
				CaptureAutoSource(FName(*PawnPrefix), Pawn, Desired);
				CaptureAutoComponents(PawnPrefix, Pawn, Desired);
			}
		}

		for (const TPair<FString, int32>& Watched : WatchedClasses)
		{
			int32 Count = 0;
			for (TActorIterator<AActor> It(World); It && Count < Watched.Value; ++It)
			{
				const AActor* Actor = *It;
				if (!IsValid(Actor) || !StateMonitorSubsystem::ClassMatches(Actor->GetClass(), Watched.Key))
				{
					continue;
				}
				++Count;
				CaptureAutoSource(Actor->GetFName(), Actor, Desired);
				CaptureAutoComponents(Actor->GetName(), Actor, Desired);
			}
		}
	}

	WatchedObjects.RemoveAll(
	    [](const TWeakObjectPtr<UObject>& Watched)
	    {
		    return !Watched.IsValid();
	    });
	if (bObserveWorld || IsMonitorVisible())
	{
		for (const TWeakObjectPtr<UObject>& Watched : WatchedObjects)
		{
			if (const UObject* Object = Watched.Get())
			{
				CaptureAutoSource(FName(*StateMonitorSubsystem::WatchedSourceId(Object)), Object, Desired);
			}
		}
	}

	// 객체가 사라졌거나 관찰을 멈춘 자동 소스를 지운다.
	for (auto It = AutoSourceIds.CreateIterator(); It; ++It)
	{
		if (!Desired.Contains(*It))
		{
			Model->RemoveSource(*It);
			It.RemoveCurrent();
		}
	}
}

FString UStateMonitorSubsystem::GetCurrentWorldName() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
	return World ? UWorld::RemovePIEPrefix(World->GetMapName()) : FString();
}

void UStateMonitorSubsystem::SendHello()
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
	const FWorldContext* Context = GameInstance ? GameInstance->GetWorldContext() : nullptr;

	FStateMonitorLiveHello Hello;
	Hello.Project = FApp::GetProjectName();
	Hello.World = GetCurrentWorldName();
	Hello.NetMode = StateMonitorSubsystem::NetModeName(World);
	Hello.PieInstance = Context && Context->WorldType == EWorldType::PIE ? Context->PIEInstance : -1;
	Hello.ProcessId = FPlatformProcess::GetCurrentProcessId();
	Hello.EngineVersion = FEngineVersion::Current().ToString(EVersionComponent::Patch);
	Hello.BuildConfiguration = LexToString(FApp::GetBuildConfiguration());
	Hello.TimeSeconds = FPlatformTime::Seconds();
	LiveLink->SendLine(StateMonitorLive::MakeHelloLine(Hello));
	LiveWorldName = Hello.World;
}

void UStateMonitorSubsystem::HandleLiveCommand(const FString& Line)
{
	FStateMonitorLiveCommand Command;
	if (!StateMonitorLive::ParseCommand(Line, Command))
	{
		return;
	}
	switch (Command.Type)
	{
	case EStateMonitorLiveCommand::Auto:
		bAutoObserve = Command.bOn;
		if (Command.bHasInterval)
		{
			AutoInterval = Command.Interval;
		}
		if (Command.bHasEngine)
		{
			bAutoEngineProperties = Command.bIncludeEngineProperties;
		}
		NextAutoTime = 0.0;
		break;
	case EStateMonitorLiveCommand::Watch:
		WatchActorClass(Command.ClassName, Command.Max);
		break;
	case EStateMonitorLiveCommand::Unwatch:
		UnwatchActorClass(Command.ClassName);
		break;
	case EStateMonitorLiveCommand::Pause:
		if (bLivePaused && !Command.bOn)
		{
			bResendAll = true;
		}
		bLivePaused = Command.bOn;
		break;
	case EStateMonitorLiveCommand::Resync:
		bResendAll = true;
		NextSendTime = 0.0;
		break;
	default:
		break;
	}
}

void UStateMonitorSubsystem::SendLiveState(bool bAll)
{
	if (!Model.IsValid())
	{
		return;
	}
	TSet<FName> Present;
	for (const TSharedPtr<FStateMonitorSource>& Source : Model->GetSources())
	{
		Present.Add(Source->SourceId);
		const uint64* Sent = SentSequences.Find(Source->SourceId);
		if (bAll || !Sent || *Sent != Source->Sequence)
		{
			LiveLink->SendLine(StateMonitorLive::MakeSnapLine(*Source, AutoSourceIds.Contains(Source->SourceId)));
			SentSequences.Add(Source->SourceId, Source->Sequence);
		}
	}
	for (auto It = SentSequences.CreateIterator(); It; ++It)
	{
		if (!Present.Contains(It->Key))
		{
			LiveLink->SendLine(StateMonitorLive::MakeRemoveLine(It->Key));
			It.RemoveCurrent();
		}
	}
}

bool UStateMonitorSubsystem::TickLive(float DeltaTime)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_StateMonitorTickLive);
	const double Now = FPlatformTime::Seconds();
	bool bConnected = false;
	if (LiveLink.IsValid())
	{
		LiveLink->Tick(Now);
		if (LiveLink->ConsumeDisconnected())
		{
			bAutoObserve = false;
			bLivePaused = false;
			NextAutoTime = 0.0;
		}
		if (LiveLink->ConsumeConnected())
		{
			// 새 뷰어는 자동 관찰을 직접 켠다. 연결 직후에는 지금까지의 소스를 모두 보낸다.
			bAutoObserve = false;
			bLivePaused = false;
			bResendAll = true;
			SentSequences.Reset();
			NextSendTime = 0.0;
			SendHello();
		}
		bConnected = LiveLink->IsConnected();
	}

	if (bConnected)
	{
		TArray<FString> Lines;
		LiveLink->PopLines(Lines);
		for (const FString& Line : Lines)
		{
			HandleLiveCommand(Line);
		}
		const FString WorldName = GetCurrentWorldName();
		if (WorldName != LiveWorldName)
		{
			LiveWorldName = WorldName;
			LiveLink->SendLine(StateMonitorLive::MakeWorldLine(WorldName));
		}
	}

	const bool bObserveWorld = bConnected && bAutoObserve;
	const bool bWatchActive = bObserveWorld || (IsMonitorVisible() && !WatchedObjects.IsEmpty());
	if (Now >= NextAutoTime && (bWatchActive || !AutoSourceIds.IsEmpty()))
	{
		UpdateAutoSources(bObserveWorld);
		NextAutoTime = Now + AutoInterval;
	}

	if (bConnected && !bLivePaused && Now >= NextSendTime &&
	    LiveLink->GetPendingBytes() < StateMonitorSubsystem::MaxPendingBytes)
	{
		SendLiveState(bResendAll);
		bResendAll = false;
		NextSendTime = Now + StateMonitorSubsystem::SendInterval;
	}
	return true;
}

bool UStateMonitorSubsystem::RemoveSource(FName SourceId)
{
	return Model.IsValid() && Model->RemoveSource(SourceId);
}

void UStateMonitorSubsystem::ClearHistory(FName SourceId)
{
	if (Model.IsValid())
	{
		Model->ClearHistory(SourceId);
	}
}

void UStateMonitorSubsystem::ResetBaseline(FName SourceId)
{
	if (Model.IsValid())
	{
		Model->ResetBaseline(SourceId);
	}
}

void UStateMonitorSubsystem::ResetMonitor()
{
	if (Model.IsValid())
	{
		Model->Reset();
	}
}

bool UStateMonitorSubsystem::ShowMonitor()
{
	UGameViewportClient* Viewport = GetGameInstance()->GetGameViewportClient();
	const bool bCanShow = Viewport && Model.IsValid();
	if (bCanShow && !Overlay.IsValid())
	{
		Overlay = CreateOverlay();
		Viewport->AddViewportWidgetContent(Overlay.ToSharedRef(), 1000);
	}
	return bCanShow;
}

void UStateMonitorSubsystem::HideMonitor()
{
	UGameViewportClient* Viewport = GetGameInstance()->GetGameViewportClient();
	if (Viewport && Overlay.IsValid())
	{
		Viewport->RemoveViewportWidgetContent(Overlay.ToSharedRef());
	}
	Overlay.Reset();
}

bool UStateMonitorSubsystem::ToggleMonitor()
{
	const bool bWasVisible = IsMonitorVisible();
	if (bWasVisible)
	{
		HideMonitor();
	}
	return !bWasVisible && ShowMonitor();
}

bool UStateMonitorSubsystem::IsMonitorVisible() const
{
	return Overlay.IsValid();
}

UStateMonitorSubsystem* UStateMonitorSubsystem::Get(const UObject* WorldContextObject)
{
	const UWorld* World = IsValid(WorldContextObject) ? WorldContextObject->GetWorld() : nullptr;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UStateMonitorSubsystem>() : nullptr;
}

bool UStateMonitorSubsystem::SubmitStructSnapshot(FName SourceId, const FText& Label, const UScriptStruct* StructType,
                                                  const void* StructData)
{
	const FText DisplayLabel = Label.IsEmpty() ? FText::FromName(SourceId) : Label;
	return Model.IsValid() && Model->SubmitSnapshot(SourceId, DisplayLabel, StructType, StructData);
}

TSharedPtr<FStateMonitorModel> UStateMonitorSubsystem::GetModel() const
{
	return Model;
}

TSharedRef<SWidget> UStateMonitorSubsystem::CreateOverlay()
{
	// 바깥 영역은 입력을 통과시켜 게임 조작을 막지 않는다.
	return SNew(SBox)
	    .HAlign(HAlign_Right)
	    .VAlign(VAlign_Fill)
	    .Padding(16.0f)
	    .Visibility(EVisibility::SelfHitTestInvisible)
	    [
	        SNew(SBox)
	        .WidthOverride(560.0f)
	        [
	            SNew(SBorder)
	            .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
	            .BorderBackgroundColor(FLinearColor(0.015f, 0.015f, 0.02f, 0.92f))
	            .Padding(8.0f)
	            [
	                SNew(SStateMonitor).Model(Model)
	            ]
	        ]
	    ];
}
