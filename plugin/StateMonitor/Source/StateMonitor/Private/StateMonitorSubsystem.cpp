#include "StateMonitorSubsystem.h"

#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "SStateMonitor.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"

UStateMonitorSubsystem::UStateMonitorSubsystem() : Model(), Overlay()
{
}

void UStateMonitorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Model = MakeShared<FStateMonitorModel>();
}

void UStateMonitorSubsystem::Deinitialize()
{
	HideMonitor();
	Model.Reset();
	Super::Deinitialize();
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
