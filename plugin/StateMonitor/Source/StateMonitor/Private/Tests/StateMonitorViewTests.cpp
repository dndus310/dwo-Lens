#if WITH_DEV_AUTOMATION_TESTS

#include "SStateMonitor.h"

#include "StateMonitorModel.h"
#include "Tests/StateMonitorTestTypes.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/Children.h"
#include "Misc/AutomationTest.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Views/STreeView.h"

namespace StateMonitorViewTests
{
TSharedPtr<SWidget> FindTaggedWidget(const TSharedRef<SWidget>& Widget, FName Tag)
{
	if (Widget->GetTag() == Tag)
	{
		return Widget;
	}
	FChildren* Children = Widget->GetChildren();
	for (int32 Index = 0; Index < Children->Num(); ++Index)
	{
		TSharedPtr<SWidget> Found = FindTaggedWidget(Children->GetChildAt(Index), Tag);
		if (Found.IsValid())
		{
			return Found;
		}
	}
	return nullptr;
}

void RefreshView(const TSharedRef<SStateMonitor>& View)
{
	const FGeometry Geometry = FGeometry::MakeRoot(FVector2D(600.0, 700.0), FSlateLayoutTransform());
	View->Tick(Geometry, FPlatformTime::Seconds(), 0.1f);
}
} // namespace StateMonitorViewTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStateMonitorViewTest, "StateMonitor.View.NavigationAndLiveHistory",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStateMonitorViewTest::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("Slate is available for the real monitor view"), FSlateApplication::IsInitialized()))
	{
		return false;
	}
	FStateMonitorSettings Settings;
	Settings.MaxHistoryEntries = 2;
	const TSharedPtr<FStateMonitorModel> Model = MakeShared<FStateMonitorModel>(Settings);
	FStateMonitorTestState State;
	State.Nested.Text = TEXT("Needle A");
	Model->SubmitSnapshot(TEXT("First"), FText::FromString(TEXT("First source")),
	                      FStateMonitorTestState::StaticStruct(), &State);
	Model->SubmitSnapshot(TEXT("Second"), FText::FromString(TEXT("Second source")),
	                      FStateMonitorTestState::StaticStruct(), &State);
	const TSharedRef<SStateMonitor> View = SNew(SStateMonitor).Model(Model);
	const TSharedPtr<STreeView<TSharedPtr<FStateMonitorViewNode>>> Tree =
	    StaticCastSharedPtr<STreeView<TSharedPtr<FStateMonitorViewNode>>>(
	        StateMonitorViewTests::FindTaggedWidget(View, TEXT("StateMonitorTree")));
	const TSharedPtr<SSearchBox> Search =
	    StaticCastSharedPtr<SSearchBox>(StateMonitorViewTests::FindTaggedWidget(View, TEXT("StateMonitorSearch")));
	const TSharedPtr<SListView<TSharedPtr<FStateMonitorHistoryEntry>>> History =
	    StaticCastSharedPtr<SListView<TSharedPtr<FStateMonitorHistoryEntry>>>(
	        StateMonitorViewTests::FindTaggedWidget(View, TEXT("StateMonitorHistory")));
	const TSharedPtr<SMultiLineEditableTextBox> Json = StaticCastSharedPtr<SMultiLineEditableTextBox>(
	    StateMonitorViewTests::FindTaggedWidget(View, TEXT("StateMonitorJson")));
	const TSharedPtr<SMultiLineEditableTextBox> HistoryDetails = StaticCastSharedPtr<SMultiLineEditableTextBox>(
	    StateMonitorViewTests::FindTaggedWidget(View, TEXT("StateMonitorHistoryDetails")));
	if (!TestTrue(TEXT("Current tree is built"), Tree.IsValid()))
	{
		return false;
	}
	if (!TestTrue(TEXT("Search is built"), Search.IsValid()))
	{
		return false;
	}
	if (!TestTrue(TEXT("History list is built"), History.IsValid()))
	{
		return false;
	}
	if (!TestTrue(TEXT("JSON view is built"), Json.IsValid()))
	{
		return false;
	}
	if (!TestTrue(TEXT("History details are built"), HistoryDetails.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("First source selected by default"), View->GetSelectedSourceId(), FName(TEXT("First")));
	if (!TestEqual(TEXT("Current has one struct root"), Tree->GetRootItems().Num(), 1))
	{
		return false;
	}
	const TSharedPtr<FStateMonitorViewNode> InitialRoot = Tree->GetRootItems()[0];
	Tree->SetItemExpansion(InitialRoot, false);
	Tree->SetSelection(InitialRoot, ESelectInfo::Direct);
	State.Nested.Text = TEXT("Needle B");
	Model->SubmitSnapshot(TEXT("First"), FText::FromString(TEXT("First source")),
	                      FStateMonitorTestState::StaticStruct(), &State);
	StateMonitorViewTests::RefreshView(View);
	TestTrue(TEXT("Capture refresh preserves field identity"), Tree->GetRootItems()[0] == InitialRoot);
	TestFalse(TEXT("Capture refresh preserves a collapsed branch"), Tree->IsItemExpanded(InitialRoot));
	TestTrue(TEXT("Capture refresh preserves selection"), Tree->GetSelectedItems().Contains(InitialRoot));
	TestTrue(TEXT("JSON reflects the new current state"), Json->GetText().ToString().Contains(TEXT("Needle B")));

	Search->SetText(FText::FromString(TEXT("no-field-matches-this-filter")));
	TestEqual(TEXT("Unmatched filter removes visible roots"), Tree->GetRootItems().Num(), 0);
	Search->SetText(FText::FromString(TEXT("Needle B")));
	TestEqual(TEXT("Value search includes its containing struct"), Tree->GetRootItems().Num(), 1);
	Search->SetText(FText::GetEmpty());
	TestFalse(TEXT("Clearing search restores the previous expansion"), Tree->IsItemExpanded(InitialRoot));
	TestTrue(TEXT("Clearing search restores the previous selection"), Tree->GetSelectedItems().Contains(InitialRoot));

	if (!TestTrue(TEXT("Changes produce inspectable history"), History->GetItems().Num() > 0))
	{
		return false;
	}
	const TSharedPtr<FStateMonitorHistoryEntry> Inspected = History->GetItems()[0];
	History->SetSelection(Inspected, ESelectInfo::Direct);
	const FString FrozenDetails = HistoryDetails->GetText().ToString();
	View->SetHistoryPaused(true);
	const uint64 FrozenNewest = History->GetItems()[0]->Sequence;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		State.Nested.Text = FString::Printf(TEXT("Live %d"), Index);
		Model->SubmitSnapshot(TEXT("First"), FText::FromString(TEXT("First source")),
		                      FStateMonitorTestState::StaticStruct(), &State);
		StateMonitorViewTests::RefreshView(View);
	}
	TestTrue(TEXT("History pause is active"), View->IsHistoryPaused());
	TestEqual(TEXT("Paused history retains its displayed entries"), History->GetItems()[0]->Sequence, FrozenNewest);
	TestTrue(TEXT("Current stays live while history pauses"), Json->GetText().ToString().Contains(TEXT("Live 3")));
	TestEqual(TEXT("An inspected change remains stable while current changes"), HistoryDetails->GetText().ToString(),
	          FrozenDetails);
	TestFalse(TEXT("Inspected record was evicted from model history"),
	          Model->FindSource(TEXT("First"))->History.Contains(Inspected));
	View->SetHistoryPaused(false);
	TestTrue(TEXT("Resuming shows latest retained entries"), History->GetItems()[0]->Sequence > FrozenNewest);
	TestEqual(TEXT("Resuming does not replace inspected change details"), HistoryDetails->GetText().ToString(),
	          FrozenDetails);
	TestTrue(TEXT("Another source can be selected"), View->SelectSource(TEXT("Second")));
	TestEqual(TEXT("Selection changes source ID"), View->GetSelectedSourceId(), FName(TEXT("Second")));
	TestTrue(TEXT("Selected source supplies current JSON"), Json->GetText().ToString().Contains(TEXT("Needle A")));
	TestFalse(TEXT("Unknown source is rejected"), View->SelectSource(TEXT("Missing")));
	View->ClearHistory();
	TestEqual(TEXT("Clear history updates selected view"), History->GetItems().Num(), 0);
	TestTrue(TEXT("Clear history retains selected current state"),
	         Json->GetText().ToString().Contains(TEXT("Needle A")));
	return true;
}

#endif
