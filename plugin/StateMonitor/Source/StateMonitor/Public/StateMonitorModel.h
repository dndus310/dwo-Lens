#pragma once

#include "CoreMinimal.h"

class UScriptStruct;

enum class EStateMonitorChange : uint8
{
	Unchanged,
	Added,
	Modified,
	Removed
};

/** A copied reflection value. No field retains an address into the submitted struct. */
struct STATEMONITOR_API FStateMonitorNode
{
	TArray<TSharedPtr<FStateMonitorNode>> Children;
	FString Path;
	FString Name;
	FString DisplayName;
	FString TypeName;
	FString Value;
	FString PreviousValue;
	EStateMonitorChange Change = EStateMonitorChange::Unchanged;
	bool bTruncated = false;
};

struct STATEMONITOR_API FStateMonitorChange
{
	FString Path;
	FString TypeName;
	FString PreviousValue;
	FString Value;
	EStateMonitorChange Change = EStateMonitorChange::Unchanged;
};

struct STATEMONITOR_API FStateMonitorHistoryEntry
{
	TArray<FStateMonitorChange> Changes;
	uint64 Sequence = 0;
	double TimeSeconds = 0.0;
	int32 OmittedChanges = 0;
};

/** Replace-on-submit source. Previously obtained source pointers remain stable snapshots. */
struct STATEMONITOR_API FStateMonitorSource
{
	TSharedPtr<FStateMonitorNode> Root;
	TArray<FStateMonitorChange> Changes;
	TArray<TSharedPtr<FStateMonitorHistoryEntry>> History;
	FName SourceId;
	FText Label;
	FString StructType;
	FString CurrentJson;
	uint64 Sequence = 0;
	double TimeSeconds = 0.0;
	int32 OmittedChanges = 0;
	bool bTruncated = false;
};

struct STATEMONITOR_API FStateMonitorSettings
{
	int32 MaxSources = 32;
	int32 MaxNodesPerSnapshot = 2048;
	int32 MaxDepth = 12;
	int32 MaxHistoryEntries = 100;
	int32 MaxChangesPerEntry = 256;
	int32 MaxTextCharacters = 4096;
	int32 MaxHistoryCharacters = 1024 * 1024;
};

/** Game-thread-only, producer-owned model. Only reflected USTRUCT/UPROPERTY fields are inspected. */
class STATEMONITOR_API FStateMonitorModel
{
public:
	explicit FStateMonitorModel(const FStateMonitorSettings& InSettings = FStateMonitorSettings());

	bool SubmitSnapshot(FName SourceId, const FText& Label, const UScriptStruct* StructType, const void* StructData,
	                    bool bRecordHistory = true);

	bool RemoveSource(FName SourceId);

	void ClearHistory(FName SourceId);

	void ResetBaseline(FName SourceId);

	void Reset();

	const TArray<TSharedPtr<FStateMonitorSource>>& GetSources() const;

	TSharedPtr<FStateMonitorSource> FindSource(FName SourceId) const;

	uint64 GetRevision() const;

	const FStateMonitorSettings& GetSettings() const;

private:
	FStateMonitorSettings Settings;
	TArray<TSharedPtr<FStateMonitorSource>> Sources;
	uint64 Revision;
	uint64 NextSequence;
};
