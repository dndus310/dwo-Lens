#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FStateMonitorModel;
class SMultiLineEditableTextBox;
class SSearchBox;
struct FStateMonitorHistoryEntry;
struct FStateMonitorNode;
struct FStateMonitorSource;
struct FStateMonitorSourceOption;
struct FStateMonitorViewNode;
template <typename ItemType>
class SComboBox;
template <typename ItemType>
class STreeView;

/** An embeddable, read-only state inspector. History pause never pauses the current state. */
class STATEMONITOR_API SStateMonitor : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SStateMonitor)
	{
	}

	SLATE_ARGUMENT(TSharedPtr<FStateMonitorModel>, Model)

	SLATE_END_ARGS()

	SStateMonitor();

	virtual void Tick(const FGeometry& AllottedGeometry, double CurrentTime, float DeltaTime) override;

	void Construct(const FArguments& InArgs);

	bool SelectSource(FName SourceId);

	void SetHistoryPaused(bool bPaused);

	void ClearHistory();

	FName GetSelectedSourceId() const;

	bool IsHistoryPaused() const;

private:
	TSharedRef<SWidget> BuildSourceControls();

	TSharedRef<SWidget> BuildViewControls();

	TSharedRef<SWidget> BuildCurrentView();

	TSharedRef<SWidget> BuildHistoryView();

	void RefreshModel(bool bForce = false);

	void RefreshCurrent();

	void RefreshHistory(bool bForce = false);

	void RefreshTree();

	void RefreshHistoryDetails();

	TSharedPtr<FStateMonitorViewNode> UpdateTreeNode(
	    const TSharedPtr<FStateMonitorNode>& Node, TMap<FString, TSharedPtr<FStateMonitorViewNode>>& UpdatedNodes,
	    bool bParentMatches);

	bool MatchesFilter(const FStateMonitorNode& Node) const;

	void SaveTreeState();

	void RestoreTreeState();

	void OnSourceChanged(TSharedPtr<FStateMonitorSourceOption> Item, ESelectInfo::Type SelectInfo);

	void OnFilterChanged(const FText& Text);

	void OnHistorySelected(TSharedPtr<FStateMonitorHistoryEntry> Item, ESelectInfo::Type SelectInfo);

	FReply OnPauseHistoryClicked();

	FReply OnClearHistoryClicked();

	FReply OnAcknowledgeClicked();

	FReply OnToggleJsonClicked();

	FReply OnCopyJsonClicked();

	FReply OnReturnToCurrentClicked();

	TSharedRef<SWidget> GenerateSourceOption(TSharedPtr<FStateMonitorSourceOption> Item);

	TSharedRef<ITableRow> GenerateFieldRow(TSharedPtr<FStateMonitorViewNode> Item,
	                                       const TSharedRef<STableViewBase>& OwnerTable);

	TSharedRef<ITableRow> GenerateHistoryRow(TSharedPtr<FStateMonitorHistoryEntry> Item,
	                                         const TSharedRef<STableViewBase>& OwnerTable);

	void GetFieldChildren(TSharedPtr<FStateMonitorViewNode> Item,
	                      TArray<TSharedPtr<FStateMonitorViewNode>>& OutChildren) const;

	FText GetSourceText() const;

	FText GetCurrentStatusText() const;

	FText GetCurrentJsonText() const;

	FText GetHistoryStatusText() const;

	FText GetHistoryDetailsText() const;

	FText GetPauseHistoryText() const;

	FText GetViewToggleText() const;

	EVisibility GetTreeVisibility() const;

	EVisibility GetJsonVisibility() const;

	EVisibility GetEmptyVisibility() const;

	EVisibility GetHistoryDetailsVisibility() const;

	bool HasSource() const;

	TSharedPtr<FStateMonitorModel> Model;
	TSharedPtr<FStateMonitorSource> CurrentSource;
	TSharedPtr<FStateMonitorHistoryEntry> SelectedHistory;
	TSharedPtr<SComboBox<TSharedPtr<FStateMonitorSourceOption>>> SourceCombo;
	TSharedPtr<STreeView<TSharedPtr<FStateMonitorViewNode>>> FieldTree;
	TSharedPtr<SListView<TSharedPtr<FStateMonitorHistoryEntry>>> HistoryList;
	TSharedPtr<SMultiLineEditableTextBox> JsonView;
	TArray<TSharedPtr<FStateMonitorSourceOption>> SourceOptions;
	TArray<TSharedPtr<FStateMonitorViewNode>> RootItems;
	TArray<TSharedPtr<FStateMonitorHistoryEntry>> HistoryItems;
	TMap<FString, TSharedPtr<FStateMonitorViewNode>> FieldItems;
	TSet<FString> SavedExpandedPaths;
	FString SavedSelectedPath;
	FString FilterText;
	FText CurrentJsonText;
	FText HistoryDetailsText;
	FName SelectedSourceId;
	double HistoryTimeOriginSeconds;
	uint64 LastRevision;
	float SavedScrollOffset;
	bool bHistoryPaused;
	bool bShowJson;
	bool bChangingSource;
	bool bInitialExpansion;
};
