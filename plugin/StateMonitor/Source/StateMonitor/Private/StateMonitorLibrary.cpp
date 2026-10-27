#include "StateMonitorLibrary.h"

#include "StateMonitorSubsystem.h"
#include "UObject/Stack.h"
#include "UObject/UnrealType.h"

bool UStateMonitorLibrary::SubmitStateSnapshot(const UObject* WorldContextObject, FName SourceId, const FText& Label,
                                               const int32& Snapshot)
{
	// Blueprint는 custom thunk를 실행하므로 이 본문은 호출되지 않는다.
	return false;
}

DEFINE_FUNCTION(UStateMonitorLibrary::execSubmitStateSnapshot)
{
	P_GET_OBJECT(UObject, WorldContextObject);
	P_GET_PROPERTY(FNameProperty, SourceId);
	P_GET_PROPERTY_REF(FTextProperty, Label);
	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FStructProperty>(nullptr);
	const FStructProperty* StructProperty = CastField<FStructProperty>(Stack.MostRecentProperty);
	const void* StructData = Stack.MostRecentPropertyAddress;
	P_FINISH;

	P_NATIVE_BEGIN;
	const bool bSubmitted = StructProperty && SubmitStructSnapshot(WorldContextObject, SourceId, Label, StructProperty->Struct, StructData);
	if (!StructProperty)
	{
		FFrame::KismetExecutionMessage(TEXT("State Monitor requires a reflected struct snapshot."), ELogVerbosity::Warning);
	}
	*static_cast<bool*>(RESULT_PARAM) = bSubmitted;
	P_NATIVE_END;
}

bool UStateMonitorLibrary::RemoveStateSource(const UObject* WorldContextObject, FName SourceId)
{
	UStateMonitorSubsystem* Monitor = UStateMonitorSubsystem::Get(WorldContextObject);
	return Monitor && Monitor->RemoveSource(SourceId);
}

void UStateMonitorLibrary::ClearStateHistory(const UObject* WorldContextObject, FName SourceId)
{
	if (UStateMonitorSubsystem* Monitor = UStateMonitorSubsystem::Get(WorldContextObject))
	{
		Monitor->ClearHistory(SourceId);
	}
}

bool UStateMonitorLibrary::ShowStateMonitor(const UObject* WorldContextObject)
{
	UStateMonitorSubsystem* Monitor = UStateMonitorSubsystem::Get(WorldContextObject);
	return Monitor && Monitor->ShowMonitor();
}

void UStateMonitorLibrary::HideStateMonitor(const UObject* WorldContextObject)
{
	if (UStateMonitorSubsystem* Monitor = UStateMonitorSubsystem::Get(WorldContextObject))
	{
		Monitor->HideMonitor();
	}
}

bool UStateMonitorLibrary::ToggleStateMonitor(const UObject* WorldContextObject)
{
	UStateMonitorSubsystem* Monitor = UStateMonitorSubsystem::Get(WorldContextObject);
	return Monitor && Monitor->ToggleMonitor();
}

bool UStateMonitorLibrary::SubmitStructSnapshot(const UObject* WorldContextObject, FName SourceId, const FText& Label,
                                                const UScriptStruct* StructType, const void* StructData)
{
	UStateMonitorSubsystem* Monitor = UStateMonitorSubsystem::Get(WorldContextObject);
	return Monitor && Monitor->SubmitStructSnapshot(SourceId, Label, StructType, StructData);
}
