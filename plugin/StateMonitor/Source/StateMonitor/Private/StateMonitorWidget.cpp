#include "StateMonitorWidget.h"

#include "StateMonitorModel.h"
#include "StateMonitorSubsystem.h"
#include "SStateMonitor.h"
#include "UObject/Stack.h"
#include "UObject/UnrealType.h"

UStateMonitorWidget::UStateMonitorWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer), bUseSharedModel(false), MonitorModel(), MonitorView()
{
}

void UStateMonitorWidget::ReleaseSlateResources(bool bReleaseChildren)
{
	MonitorView.Reset();
	Super::ReleaseSlateResources(bReleaseChildren);
}

bool UStateMonitorWidget::SubmitSnapshot(FName SourceId, FText Label, const int32& Snapshot)
{
	// Blueprint executes the custom thunk so the actual script struct is available.
	return false;
}

DEFINE_FUNCTION(UStateMonitorWidget::execSubmitSnapshot)
{
	P_GET_PROPERTY(FNameProperty, SourceId);
	P_GET_PROPERTY(FTextProperty, Label);
	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FStructProperty>(nullptr);
	const FStructProperty* StructProperty = CastField<FStructProperty>(Stack.MostRecentProperty);
	const void* StructData = Stack.MostRecentPropertyAddress;
	P_FINISH;

	P_NATIVE_BEGIN;
	bool bSubmitted = false;
	if (StructProperty)
	{
		bSubmitted = P_THIS->SubmitStructSnapshot(SourceId, Label, StructProperty->Struct, StructData);
	}
	else
	{
		FFrame::KismetExecutionMessage(TEXT("State Monitor requires a reflected struct snapshot."),
		                               ELogVerbosity::Warning);
	}
	*static_cast<bool*>(RESULT_PARAM) = bSubmitted;
	P_NATIVE_END;
}

bool UStateMonitorWidget::RemoveSource(FName SourceId)
{
	return GetMonitorModel()->RemoveSource(SourceId);
}

void UStateMonitorWidget::ClearHistory(FName SourceId)
{
	GetMonitorModel()->ClearHistory(SourceId);
}

void UStateMonitorWidget::ResetBaseline(FName SourceId)
{
	GetMonitorModel()->ResetBaseline(SourceId);
}

void UStateMonitorWidget::ResetMonitor()
{
	GetMonitorModel()->Reset();
}

bool UStateMonitorWidget::SubmitStructSnapshot(FName SourceId, const FText& Label, const UScriptStruct* StructType,
                                                  const void* StructData)
{
	return GetMonitorModel()->SubmitSnapshot(SourceId, Label, StructType, StructData);
}

TSharedPtr<FStateMonitorModel> UStateMonitorWidget::GetMonitorModel()
{
	check(IsInGameThread());
	const UStateMonitorSubsystem* SharedMonitor = bUseSharedModel ? UStateMonitorSubsystem::Get(this) : nullptr;
	TSharedPtr<FStateMonitorModel> SharedModel = SharedMonitor ? SharedMonitor->GetModel() : nullptr;
	// 공유 모델을 찾지 못하면(디자이너 미리보기 등) 위젯 자체 모델로 동작한다.
	if (!SharedModel.IsValid() && !MonitorModel.IsValid())
	{
		MonitorModel = MakeShared<FStateMonitorModel>();
	}
	return SharedModel.IsValid() ? SharedModel : MonitorModel;
}

TSharedRef<SWidget> UStateMonitorWidget::RebuildWidget()
{
	return SAssignNew(MonitorView, SStateMonitor).Model(GetMonitorModel());
}
