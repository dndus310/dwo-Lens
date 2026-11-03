#pragma once

#include "CoreMinimal.h"

class FProperty;
class UObject;
class UScriptStruct;
class UStruct;

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
	/** @brief Object snapshot only: path of the captured object. Empty for struct snapshots. */
	FString ObjectPath;
	/** @brief Object snapshot only: owner actor path, or the outer actor for components. */
	FString OwnerPath;
	FString CurrentJson;
	uint64 Sequence = 0;
	double TimeSeconds = 0.0;
	int32 OmittedChanges = 0;
	bool bTruncated = false;
	/** @brief true when the value was captured from a UObject instead of a USTRUCT value. */
	bool bObject = false;
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

	/**
	 * @brief Captures the reflected properties of a UObject (any UClass).
	 * By default only properties declared by project code are included: owner class is a Blueprint class,
	 * or a native class whose module is not under the engine directory. bIncludeEngineProperties includes all.
	 */
	bool SubmitObjectSnapshot(FName SourceId, const FText& Label, const UObject* Object, bool bRecordHistory = true,
	                          bool bIncludeEngineProperties = false);

	bool RemoveSource(FName SourceId);

	void ClearHistory(FName SourceId);

	void ResetBaseline(FName SourceId);

	void Reset();

	const TArray<TSharedPtr<FStateMonitorSource>>& GetSources() const;

	TSharedPtr<FStateMonitorSource> FindSource(FName SourceId) const;

	uint64 GetRevision() const;

	const FStateMonitorSettings& GetSettings() const;

	/** @brief Object capture filter for one top-level property. Deprecated, editor-only and delegate properties are always skipped. */
	static bool IsObjectPropertyIncluded(const FProperty* Property, bool bIncludeEngineProperties);

	/** @brief Number of top-level properties of Type that IsObjectPropertyIncluded accepts. */
	static int32 CountIncludedProperties(const UStruct* Type, bool bIncludeEngineProperties);

	/** @brief true when Type was declared by project code (Blueprint class, or a native module outside the engine directory). */
	static bool IsProjectType(const UStruct* Type);

private:
	bool SubmitCaptured(FName SourceId, const FText& Label, const UStruct* Type, const void* Data, bool bRecordHistory,
	                    bool bObject, bool bIncludeEngineProperties, const FString& ObjectPath, const FString& OwnerPath);

	FStateMonitorSettings Settings;
	TArray<TSharedPtr<FStateMonitorSource>> Sources;
	uint64 Revision;
	uint64 NextSequence;
};
