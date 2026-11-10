#include "SStateMonitor.h"

#include "StateMonitorModel.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SExpanderArrow.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/STreeView.h"

#define LOCTEXT_NAMESPACE "StateMonitor"

struct FStateMonitorSourceOption
{
	FName SourceId;
	FText Label;
};

struct FStateMonitorViewNode
{
	TSharedPtr<FStateMonitorNode> Snapshot;
	TArray<TSharedPtr<FStateMonitorViewNode>> Children;
};

class SStateMonitorFieldRow : public SMultiColumnTableRow<TSharedPtr<FStateMonitorViewNode>>
{
public:
	SLATE_BEGIN_ARGS(SStateMonitorFieldRow)
	{
	}

	SLATE_ARGUMENT(TSharedPtr<FStateMonitorViewNode>, Item)

	SLATE_END_ARGS()

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override;

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable);

private:
	FText GetNameText() const;

	FText GetTypeText() const;

	FText GetValueText() const;

	FText GetFieldTooltip() const;

	FSlateColor GetValueColor() const;

	TSharedPtr<FStateMonitorViewNode> Item;
};

void SStateMonitorFieldRow::Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable)
{
	Item = InArgs._Item;
	SMultiColumnTableRow::Construct(FSuperRowType::FArguments().Padding(FMargin(2, 3)), OwnerTable);
	SetToolTipText(TAttribute<FText>::CreateSP(this, &SStateMonitorFieldRow::GetFieldTooltip));
}

TSharedRef<SWidget> SStateMonitorFieldRow::GenerateWidgetForColumn(const FName& ColumnName)
{
	if (ColumnName == TEXT("Field"))
	{
		// clang-format off
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SExpanderArrow, SharedThis(this))
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(this, &SStateMonitorFieldRow::GetNameText)
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			];
		// clang-format on
	}
	if (ColumnName == TEXT("Type"))
	{
		return SNew(STextBlock)
		    .Text(this, &SStateMonitorFieldRow::GetTypeText)
		    .ColorAndOpacity(FLinearColor(0.57f, 0.64f, 0.72f))
		    .OverflowPolicy(ETextOverflowPolicy::Ellipsis);
	}
	return SNew(STextBlock)
	    .Text(this, &SStateMonitorFieldRow::GetValueText)
	    .ColorAndOpacity(this, &SStateMonitorFieldRow::GetValueColor)
	    .OverflowPolicy(ETextOverflowPolicy::Ellipsis);
}

FText SStateMonitorFieldRow::GetNameText() const
{
	return FText::FromString(Item->Snapshot->Name);
}

FText SStateMonitorFieldRow::GetTypeText() const
{
	return FText::FromString(Item->Snapshot->TypeName);
}

FText SStateMonitorFieldRow::GetValueText() const
{
	const FStateMonitorNode& Node = *Item->Snapshot;
	FString Prefix;
	if (Node.Change == EStateMonitorChange::Added)
	{
		Prefix = TEXT("+ ");
	}
	if (Node.Change == EStateMonitorChange::Modified)
	{
		Prefix = TEXT("~ ");
	}
	return FText::FromString(Prefix + Node.Value);
}

FText SStateMonitorFieldRow::GetFieldTooltip() const
{
	const FStateMonitorNode& Node = *Item->Snapshot;
	FString Details = Node.Path + TEXT("\n") + Node.TypeName + TEXT("\nCurrent: ") + Node.Value;
	if (Node.Change == EStateMonitorChange::Modified)
	{
		Details += TEXT("\nPrevious: ") + Node.PreviousValue;
	}
	if (Node.Change == EStateMonitorChange::Added)
	{
		Details += TEXT("\nAdded in the latest change.");
	}
	if (Node.bTruncated)
	{
		Details += TEXT("\nCapture limit reached; this value is incomplete.");
	}
	return FText::FromString(Details);
}

FSlateColor SStateMonitorFieldRow::GetValueColor() const
{
	if (Item->Snapshot->Change == EStateMonitorChange::Modified)
	{
		return FLinearColor(1.0f, 0.73f, 0.24f);
	}
	if (Item->Snapshot->Change == EStateMonitorChange::Added)
	{
		return FLinearColor(0.36f, 0.89f, 0.61f);
	}
	return FSlateColor::UseForeground();
}

SStateMonitor::SStateMonitor()
    : Model(), CurrentSource(), SelectedHistory(), SourceCombo(), FieldTree(), HistoryList(), JsonView(),
      SourceOptions(), RootItems(), HistoryItems(), FieldItems(), SavedExpandedPaths(), SavedSelectedPath(),
      FilterText(), CurrentJsonText(), HistoryDetailsText(), SelectedSourceId(NAME_None),
      HistoryTimeOriginSeconds(FPlatformTime::Seconds()), LastRevision(MAX_uint64),
      SavedScrollOffset(0.0f), bHistoryPaused(false), bShowJson(false), bChangingSource(false), bInitialExpansion(true)
{
}

// clang-format off
void SStateMonitor::Construct(const FArguments& InArgs)
{
	Model = InArgs._Model;
	if (Model.IsValid())
	{
		for (const TSharedPtr<FStateMonitorSource>& Source : Model->GetSources())
		{
			HistoryTimeOriginSeconds = FMath::Min(HistoryTimeOriginSeconds, Source->TimeSeconds);
			for (const TSharedPtr<FStateMonitorHistoryEntry>& Entry : Source->History)
			{
				HistoryTimeOriginSeconds = FMath::Min(HistoryTimeOriginSeconds, Entry->TimeSeconds);
			}
		}
	}
	RefreshHistoryDetails();
	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
		[
			BuildSourceControls()
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
		[
			SNew(SSearchBox).Tag(TEXT("StateMonitorSearch"))
			.HintText(LOCTEXT("SearchHint", "Filter field path, type or value"))
			.OnTextChanged(this, &SStateMonitor::OnFilterChanged)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
		[
			BuildViewControls()
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
		[
			SNew(STextBlock).Text(this, &SStateMonitor::GetCurrentStatusText)
			.AutoWrapText(true).ColorAndOpacity(FLinearColor(0.65f, 0.72f, 0.8f))
		]
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			BuildCurrentView()
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
		[
			BuildHistoryView()
		]
	];
	RefreshModel(true);
}

void SStateMonitor::Tick(const FGeometry& AllottedGeometry, double CurrentTime, float DeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, CurrentTime, DeltaTime);
	RefreshModel();
}

TSharedRef<SWidget> SStateMonitor::BuildSourceControls()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
		[
			SNew(STextBlock).Text(LOCTEXT("Source", "Source"))
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[
			SAssignNew(SourceCombo, SComboBox<TSharedPtr<FStateMonitorSourceOption>>)
			.Tag(TEXT("StateMonitorSource"))
			.OptionsSource(&SourceOptions)
			.OnGenerateWidget(this, &SStateMonitor::GenerateSourceOption)
			.OnSelectionChanged(this, &SStateMonitor::OnSourceChanged)
			[
				SNew(STextBlock).Text(this, &SStateMonitor::GetSourceText)
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
		];
}

TSharedRef<SWidget> SStateMonitor::BuildViewControls()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
		[
			SNew(SButton).Text(this, &SStateMonitor::GetViewToggleText)
			.OnClicked(this, &SStateMonitor::OnToggleJsonClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
		[
			SNew(SButton).Text(LOCTEXT("CopyJson", "Copy JSON"))
			.IsEnabled(this, &SStateMonitor::HasSource)
			.ToolTipText(LOCTEXT("CopyJsonTip", "Copy the complete captured current state. The tree filter does not alter JSON."))
			.OnClicked(this, &SStateMonitor::OnCopyJsonClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SButton).Text(LOCTEXT("Acknowledge", "Acknowledge"))
			.IsEnabled(this, &SStateMonitor::HasSource)
			.ToolTipText(LOCTEXT("AcknowledgeTip", "Clear current change highlights. Retained history and observed state stay unchanged."))
			.OnClicked(this, &SStateMonitor::OnAcknowledgeClicked)
		];
}

TSharedRef<SWidget> SStateMonitor::BuildCurrentView()
{
	return SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SAssignNew(FieldTree, STreeView<TSharedPtr<FStateMonitorViewNode>>)
			.Tag(TEXT("StateMonitorTree"))
			.TreeItemsSource(&RootItems)
			.OnGenerateRow(this, &SStateMonitor::GenerateFieldRow)
			.OnGetChildren(this, &SStateMonitor::GetFieldChildren)
			.SelectionMode(ESelectionMode::Single)
			.Visibility(this, &SStateMonitor::GetTreeVisibility)
			.HeaderRow
			(
				SNew(SHeaderRow)
				+ SHeaderRow::Column(TEXT("Field")).DefaultLabel(LOCTEXT("Field", "Field")).FillWidth(0.4f)
				+ SHeaderRow::Column(TEXT("Type")).DefaultLabel(LOCTEXT("Type", "Type")).FillWidth(0.2f)
				+ SHeaderRow::Column(TEXT("Value")).DefaultLabel(LOCTEXT("Value", "Current value")).FillWidth(0.4f)
			)
		]
		+ SOverlay::Slot()
		[
			SAssignNew(JsonView, SMultiLineEditableTextBox).Tag(TEXT("StateMonitorJson"))
			.IsReadOnly(true).AutoWrapText(false)
			.Text(this, &SStateMonitor::GetCurrentJsonText)
			.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
			.Visibility(this, &SStateMonitor::GetJsonVisibility)
		]
		+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(12)
		[
			SNew(STextBlock).Text(LOCTEXT("Empty", "No fields to display.\nSubmit a reflected struct or adjust the filter."))
			.Justification(ETextJustify::Center).AutoWrapText(true)
			.Visibility(this, &SStateMonitor::GetEmptyVisibility)
		];
}

TSharedRef<SWidget> SStateMonitor::BuildHistoryView()
{
	return SNew(SExpandableArea).InitiallyCollapsed(false)
		.HeaderContent()
		[
			SNew(STextBlock).Text(this, &SStateMonitor::GetHistoryStatusText)
		]
		.BodyContent()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton).Text(this, &SStateMonitor::GetPauseHistoryText)
					.ToolTipText(LOCTEXT("PauseTip", "Freeze the displayed history. Current state and producer capture continue."))
					.OnClicked(this, &SStateMonitor::OnPauseHistoryClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton).Text(LOCTEXT("ClearHistory", "Clear history"))
					.IsEnabled(this, &SStateMonitor::HasSource)
					.OnClicked(this, &SStateMonitor::OnClearHistoryClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton).Text(LOCTEXT("ReturnCurrent", "Return to Current"))
					.Visibility(this, &SStateMonitor::GetHistoryDetailsVisibility)
					.OnClicked(this, &SStateMonitor::OnReturnToCurrentClicked)
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).HeightOverride(170)
				[
					SNew(SSplitter)
					+ SSplitter::Slot().Value(0.3f)
					[
						SAssignNew(HistoryList, SListView<TSharedPtr<FStateMonitorHistoryEntry>>)
						.Tag(TEXT("StateMonitorHistory"))
						.ListItemsSource(&HistoryItems).SelectionMode(ESelectionMode::Single)
						.OnGenerateRow(this, &SStateMonitor::GenerateHistoryRow)
						.OnSelectionChanged(this, &SStateMonitor::OnHistorySelected)
					]
					+ SSplitter::Slot().Value(0.7f)
					[
						SNew(SMultiLineEditableTextBox).Tag(TEXT("StateMonitorHistoryDetails"))
						.IsReadOnly(true).AutoWrapText(true)
						.Text(this, &SStateMonitor::GetHistoryDetailsText)
					]
				]
			]
		];
}
// clang-format on
void SStateMonitor::RefreshModel(bool bForce)
{
	if (!Model.IsValid())
	{
		return;
	}
	if (!bForce)
	{
		if (LastRevision == Model->GetRevision())
		{
			return;
		}
	}
	LastRevision = Model->GetRevision();
	TArray<TSharedPtr<FStateMonitorSourceOption>> UpdatedOptions;
	for (const TSharedPtr<FStateMonitorSource>& Source : Model->GetSources())
	{
		TSharedPtr<FStateMonitorSourceOption> Option;
		for (const TSharedPtr<FStateMonitorSourceOption>& Existing : SourceOptions)
		{
			if (Existing->SourceId == Source->SourceId)
			{
				Option = Existing;
				break;
			}
		}
		if (!Option.IsValid())
		{
			Option = MakeShared<FStateMonitorSourceOption>();
		}
		Option->SourceId = Source->SourceId;
		Option->Label = Source->Label;
		UpdatedOptions.Add(Option);
	}
	SourceOptions = MoveTemp(UpdatedOptions);
	SourceCombo->RefreshOptions();
	const bool bSelectionExists = Model->FindSource(SelectedSourceId).IsValid();
	if (!bSelectionExists)
	{
		if (!SourceOptions.IsEmpty())
		{
			SelectSource(SourceOptions[0]->SourceId);
			return;
		}
		SelectedSourceId = NAME_None;
		CurrentSource.Reset();
		FieldItems.Reset();
		RootItems.Reset();
		HistoryItems.Reset();
		SelectedHistory.Reset();
		RefreshHistoryDetails();
		FieldTree->RequestTreeRefresh();
		HistoryList->RequestListRefresh();
		CurrentJsonText = FText::GetEmpty();
		return;
	}
	RefreshCurrent();
	RefreshHistory();
}

bool SStateMonitor::SelectSource(FName SourceId)
{
	if (!Model.IsValid())
	{
		return false;
	}
	if (!Model->FindSource(SourceId).IsValid())
	{
		return false;
	}
	if (SelectedSourceId == SourceId)
	{
		return true;
	}
	SelectedSourceId = SourceId;
	FieldItems.Reset();
	SavedExpandedPaths.Reset();
	SavedSelectedPath.Reset();
	SelectedHistory.Reset();
	RefreshHistoryDetails();
	bInitialExpansion = true;
	FieldTree->ClearSelection();
	FieldTree->ClearExpandedItems();
	FieldTree->SetScrollOffset(0.0f);
	HistoryList->ClearSelection();
	HistoryList->SetScrollOffset(0.0f);
	bChangingSource = true;
	for (const TSharedPtr<FStateMonitorSourceOption>& Option : SourceOptions)
	{
		if (Option->SourceId == SourceId)
		{
			SourceCombo->SetSelectedItem(Option);
			break;
		}
	}
	bChangingSource = false;
	RefreshCurrent();
	RefreshHistory(true);
	return true;
}

void SStateMonitor::RefreshCurrent()
{
	const TSharedPtr<FStateMonitorSource> LatestSource = Model->FindSource(SelectedSourceId);
	if (CurrentSource == LatestSource)
	{
		return;
	}
	CurrentSource = LatestSource;
	RefreshTree();
	CurrentJsonText = FText::FromString(CurrentSource->CurrentJson);
}

void SStateMonitor::RefreshHistory(bool bForce)
{
	if (!bForce)
	{
		if (bHistoryPaused)
		{
			return;
		}
	}
	const float OldOffset = HistoryList->GetScrollOffset();
	const int32 FirstVisibleIndex = FMath::FloorToInt(OldOffset);
	uint64 AnchorSequence = 0;
	if (HistoryItems.IsValidIndex(FirstVisibleIndex))
	{
		AnchorSequence = HistoryItems[FirstVisibleIndex]->Sequence;
	}
	HistoryItems = CurrentSource->History;
	HistoryItems.Sort(
	    [](const TSharedPtr<FStateMonitorHistoryEntry>& Left, const TSharedPtr<FStateMonitorHistoryEntry>& Right)
	    {
		    return Left->Sequence > Right->Sequence;
	    });
	HistoryList->RequestListRefresh();
	for (int32 Index = 0; Index < HistoryItems.Num(); ++Index)
	{
		if (HistoryItems[Index]->Sequence == AnchorSequence)
		{
			HistoryList->SetScrollOffset(Index + FMath::Frac(OldOffset));
			break;
		}
	}
}

void SStateMonitor::RefreshTree()
{
	TMap<FString, TSharedPtr<FStateMonitorViewNode>> UpdatedNodes;
	RootItems.Reset();
	if (CurrentSource.IsValid())
	{
		if (CurrentSource->Root.IsValid())
		{
			TSharedPtr<FStateMonitorViewNode> Root = UpdateTreeNode(CurrentSource->Root, UpdatedNodes, false);
			if (Root.IsValid())
			{
				RootItems.Add(Root);
			}
		}
	}
	FieldItems = MoveTemp(UpdatedNodes);
	FieldTree->RequestTreeRefresh();
	if (bInitialExpansion)
	{
		for (const TSharedPtr<FStateMonitorViewNode>& Root : RootItems)
		{
			FieldTree->SetItemExpansion(Root, true);
		}
		bInitialExpansion = false;
	}
	if (!FilterText.IsEmpty())
	{
		for (const TPair<FString, TSharedPtr<FStateMonitorViewNode>>& Pair : FieldItems)
		{
			if (!Pair.Value->Children.IsEmpty())
			{
				FieldTree->SetItemExpansion(Pair.Value, true);
			}
		}
	}
}

TSharedPtr<FStateMonitorViewNode> SStateMonitor::UpdateTreeNode(
    const TSharedPtr<FStateMonitorNode>& Node, TMap<FString, TSharedPtr<FStateMonitorViewNode>>& UpdatedNodes,
    bool bParentMatches)
{
	TSharedPtr<FStateMonitorViewNode> Item = FieldItems.FindRef(Node->Path);
	if (!Item.IsValid())
	{
		Item = MakeShared<FStateMonitorViewNode>();
	}
	Item->Snapshot = Node;
	Item->Children.Reset();
	bool bMatches = bParentMatches;
	if (!bMatches)
	{
		bMatches = MatchesFilter(*Node);
	}
	for (const TSharedPtr<FStateMonitorNode>& Child : Node->Children)
	{
		TSharedPtr<FStateMonitorViewNode> ChildItem = UpdateTreeNode(Child, UpdatedNodes, bMatches);
		if (ChildItem.IsValid())
		{
			Item->Children.Add(ChildItem);
		}
	}
	// Keep identities for filtered-out fields as well, so clearing a filter restores the same rows.
	UpdatedNodes.Add(Node->Path, Item);
	if (bMatches)
	{
		return Item;
	}
	if (!Item->Children.IsEmpty())
	{
		return Item;
	}
	return nullptr;
}

bool SStateMonitor::MatchesFilter(const FStateMonitorNode& Node) const
{
	if (FilterText.IsEmpty())
	{
		return true;
	}
	if (Node.Path.Contains(FilterText))
	{
		return true;
	}
	if (Node.TypeName.Contains(FilterText))
	{
		return true;
	}
	if (Node.Value.Contains(FilterText))
	{
		return true;
	}
	return Node.PreviousValue.Contains(FilterText);
}

void SStateMonitor::SaveTreeState()
{
	SavedExpandedPaths.Reset();
	STreeView<TSharedPtr<FStateMonitorViewNode>>::TItemSet Expanded;
	FieldTree->GetExpandedItems(Expanded);
	for (const TSharedPtr<FStateMonitorViewNode>& Item : Expanded)
	{
		SavedExpandedPaths.Add(Item->Snapshot->Path);
	}
	SavedSelectedPath.Reset();
	const TArray<TSharedPtr<FStateMonitorViewNode>> Selected = FieldTree->GetSelectedItems();
	if (!Selected.IsEmpty())
	{
		SavedSelectedPath = Selected[0]->Snapshot->Path;
	}
	SavedScrollOffset = FieldTree->GetScrollOffset();
}

void SStateMonitor::RestoreTreeState()
{
	FieldTree->ClearExpandedItems();
	for (const FString& Path : SavedExpandedPaths)
	{
		TSharedPtr<FStateMonitorViewNode> Item = FieldItems.FindRef(Path);
		if (Item.IsValid())
		{
			FieldTree->SetItemExpansion(Item, true);
		}
	}
	TSharedPtr<FStateMonitorViewNode> Selected = FieldItems.FindRef(SavedSelectedPath);
	if (Selected.IsValid())
	{
		FieldTree->SetSelection(Selected, ESelectInfo::Direct);
	}
	FieldTree->SetScrollOffset(SavedScrollOffset);
}

void SStateMonitor::OnSourceChanged(TSharedPtr<FStateMonitorSourceOption> Item, ESelectInfo::Type SelectInfo)
{
	if (bChangingSource)
	{
		return;
	}
	if (Item.IsValid())
	{
		SelectSource(Item->SourceId);
	}
}

void SStateMonitor::OnFilterChanged(const FText& Text)
{
	const FString NewFilter = Text.ToString().TrimStartAndEnd();
	if (FilterText.IsEmpty())
	{
		SaveTreeState();
	}
	FilterText = NewFilter;
	RefreshTree();
	if (FilterText.IsEmpty())
	{
		RestoreTreeState();
	}
}

void SStateMonitor::OnHistorySelected(TSharedPtr<FStateMonitorHistoryEntry> Item, ESelectInfo::Type SelectInfo)
{
	// List refresh can evict an item. Keep an inspected record until the user explicitly leaves it.
	if (Item.IsValid())
	{
		SelectedHistory = Item;
		RefreshHistoryDetails();
	}
}

void SStateMonitor::SetHistoryPaused(bool bPaused)
{
	if (bHistoryPaused == bPaused)
	{
		return;
	}
	bHistoryPaused = bPaused;
	if (!bHistoryPaused)
	{
		if (CurrentSource.IsValid())
		{
			RefreshHistory(true);
		}
	}
}

void SStateMonitor::ClearHistory()
{
	if (!HasSource())
	{
		return;
	}
	Model->ClearHistory(SelectedSourceId);
	SelectedHistory.Reset();
	RefreshHistoryDetails();
	HistoryList->ClearSelection();
	RefreshModel(true);
	RefreshHistory(true);
}

FName SStateMonitor::GetSelectedSourceId() const
{
	return SelectedSourceId;
}

bool SStateMonitor::IsHistoryPaused() const
{
	return bHistoryPaused;
}

FReply SStateMonitor::OnPauseHistoryClicked()
{
	SetHistoryPaused(!bHistoryPaused);
	return FReply::Handled();
}

FReply SStateMonitor::OnClearHistoryClicked()
{
	ClearHistory();
	return FReply::Handled();
}

FReply SStateMonitor::OnAcknowledgeClicked()
{
	if (HasSource())
	{
		Model->ResetBaseline(SelectedSourceId);
		RefreshModel(true);
	}
	return FReply::Handled();
}

FReply SStateMonitor::OnToggleJsonClicked()
{
	bShowJson = !bShowJson;
	return FReply::Handled();
}

FReply SStateMonitor::OnCopyJsonClicked()
{
	if (CurrentSource.IsValid())
	{
		FPlatformApplicationMisc::ClipboardCopy(*CurrentSource->CurrentJson);
	}
	return FReply::Handled();
}

FReply SStateMonitor::OnReturnToCurrentClicked()
{
	SelectedHistory.Reset();
	RefreshHistoryDetails();
	HistoryList->ClearSelection();
	return FReply::Handled();
}

TSharedRef<SWidget> SStateMonitor::GenerateSourceOption(TSharedPtr<FStateMonitorSourceOption> Item)
{
	return SNew(STextBlock).Text(Item->Label).ToolTipText(FText::FromName(Item->SourceId));
}

TSharedRef<ITableRow> SStateMonitor::GenerateFieldRow(TSharedPtr<FStateMonitorViewNode> Item,
                                                         const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SStateMonitorFieldRow, OwnerTable).Item(Item);
}

TSharedRef<ITableRow> SStateMonitor::GenerateHistoryRow(TSharedPtr<FStateMonitorHistoryEntry> Item,
                                                           const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Summary =
	    FString::Printf(TEXT("#%llu  %d fields\n+%.2f s"), Item->Sequence, Item->Changes.Num(),
	        Item->TimeSeconds - HistoryTimeOriginSeconds);
	return SNew(STableRow<TSharedPtr<FStateMonitorHistoryEntry>>, OwnerTable)
	    .Padding(FMargin(
	        4, 5))[SNew(STextBlock).Text(FText::FromString(Summary)).OverflowPolicy(ETextOverflowPolicy::Ellipsis)];
}

void SStateMonitor::GetFieldChildren(TSharedPtr<FStateMonitorViewNode> Item,
                                        TArray<TSharedPtr<FStateMonitorViewNode>>& OutChildren) const
{
	OutChildren = Item->Children;
}

FText SStateMonitor::GetSourceText() const
{
	if (CurrentSource.IsValid())
	{
		return CurrentSource->Label;
	}
	return LOCTEXT("NoSource", "No sources");
}

FText SStateMonitor::GetCurrentStatusText() const
{
	if (!CurrentSource.IsValid())
	{
		return LOCTEXT("AwaitingState", "Current | Waiting for a struct snapshot");
	}
	FString Status = FString::Printf(TEXT("Current #%llu  |  %s  |  %d changed fields"), CurrentSource->Sequence,
	                                 *CurrentSource->StructType, CurrentSource->Changes.Num());
	if (CurrentSource->bTruncated)
	{
		Status += TEXT("\nCapture limit reached; some values are omitted.");
	}
	if (bShowJson)
	{
		if (!FilterText.IsEmpty())
		{
			Status += TEXT("\nJSON shows the full capture; filtering applies to the tree.");
		}
	}
	return FText::FromString(Status);
}

FText SStateMonitor::GetCurrentJsonText() const
{
	return CurrentJsonText;
}

FText SStateMonitor::GetHistoryStatusText() const
{
	const TCHAR* Status = bHistoryPaused ? TEXT("Paused; Current stays live") : TEXT("Live");
	return FText::FromString(FString::Printf(TEXT("History  |  %d retained  |  %s"), HistoryItems.Num(), Status));
}

FText SStateMonitor::GetHistoryDetailsText() const
{
	return HistoryDetailsText;
}

void SStateMonitor::RefreshHistoryDetails()
{
	if (!SelectedHistory.IsValid())
	{
		HistoryDetailsText = LOCTEXT(
		    "SelectHistory", "Select a history entry to inspect its changes.\n\nThe current state above remains "
		                     "live.\nAmber ~ changed, green + added. Removed fields appear in history.");
		return;
	}
	FString Details = FString::Printf(TEXT("History #%llu\n"), SelectedHistory->Sequence);
	for (const FStateMonitorChange& Change : SelectedHistory->Changes)
	{
		const TCHAR* Marker = TEXT("~");
		if (Change.Change == EStateMonitorChange::Added)
		{
			Marker = TEXT("+");
		}
		if (Change.Change == EStateMonitorChange::Removed)
		{
			Marker = TEXT("-");
		}
		Details += FString::Printf(TEXT("\n%s %s [%s]\nPrevious: %s\nCurrent: %s\n"), Marker, *Change.Path,
		                           *Change.TypeName, *Change.PreviousValue, *Change.Value);
	}
	if (SelectedHistory->OmittedChanges > 0)
	{
		Details += FString::Printf(TEXT("\n%d additional changes omitted by the capture limit."),
		                           SelectedHistory->OmittedChanges);
	}
	HistoryDetailsText = FText::FromString(Details);
}

FText SStateMonitor::GetPauseHistoryText() const
{
	return bHistoryPaused ? LOCTEXT("ResumeHistory", "Resume history") : LOCTEXT("PauseHistory", "Pause history");
}

FText SStateMonitor::GetViewToggleText() const
{
	return bShowJson ? LOCTEXT("ShowTree", "Show tree") : LOCTEXT("ShowJson", "Show JSON");
}

EVisibility SStateMonitor::GetTreeVisibility() const
{
	return bShowJson ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SStateMonitor::GetJsonVisibility() const
{
	return bShowJson ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SStateMonitor::GetEmptyVisibility() const
{
	if (bShowJson)
	{
		return EVisibility::Collapsed;
	}
	return RootItems.IsEmpty() ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
}

EVisibility SStateMonitor::GetHistoryDetailsVisibility() const
{
	return SelectedHistory.IsValid() ? EVisibility::Visible : EVisibility::Collapsed;
}

bool SStateMonitor::HasSource() const
{
	return CurrentSource.IsValid();
}

#undef LOCTEXT_NAMESPACE
