#pragma once

#include "CoreMinimal.h"
#include "StateMonitorTestTypes.generated.h"

USTRUCT()
struct FStateMonitorTestNested
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Position = FVector::ZeroVector;

	UPROPERTY()
	FString Text;

	UPROPERTY()
	FDateTime Timestamp;
};

USTRUCT()
struct FStateMonitorTestState
{
	GENERATED_BODY()

	UPROPERTY()
	FStateMonitorTestNested Nested;

	UPROPERTY()
	TArray<int32> Items;

	UPROPERTY()
	TMap<FString, int32> Counters;

	UPROPERTY()
	TSet<FString> Tags;

	UPROPERTY()
	TObjectPtr<UObject> Object;

	UPROPERTY()
	TSoftObjectPtr<UObject> SoftObject;

	UPROPERTY()
	int64 Signed = MIN_int64;

	UPROPERTY()
	uint64 Unsigned = MAX_uint64;

	UPROPERTY()
	bool bEnabled = true;

	UPROPERTY()
	float SpecialNumber = 0.0f;

	UPROPERTY()
	int32 StaticItems[3] = {1, 2, 3};

	int32 Unreflected = 17;
};
