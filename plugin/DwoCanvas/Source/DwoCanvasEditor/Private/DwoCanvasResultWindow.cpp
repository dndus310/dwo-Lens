// DwoCanvas - import result window

#include "DwoCanvasResultWindow.h"

#include "DwoCanvasImportRecord.h"
#include "DwoCanvasSettings.h"

#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "DwoCanvasResult"

namespace DwoCanvasResultUI
{
	FSlateFontInfo Font(int32 Size, bool bBold = false)
	{
		return FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size);
	}

	const FSlateBrush* SeverityIcon(EDwoCanvasSeverity Severity, bool bLarge = false)
	{
		switch (Severity)
		{
		case EDwoCanvasSeverity::Error: return FAppStyle::GetBrush(bLarge ? "Icons.ErrorWithColor.Large" : "Icons.ErrorWithColor");
		case EDwoCanvasSeverity::Warning: return FAppStyle::GetBrush(bLarge ? "Icons.WarningWithColor.Large" : "Icons.WarningWithColor");
		default: return FAppStyle::GetBrush("Icons.Info");
		}
	}

	TSharedRef<SWidget> Section(const FText& Title)
	{
		return SNew(STextBlock).Text(Title).Font(Font(11, true)).ColorAndOpacity(FStyleColors::Foreground);
	}

	/** Number + label tile, dimmed when zero. */
	TSharedRef<SWidget> CountTile(const FText& Label, int32 Value, const FText& Tooltip)
	{
		const FSlateColor ValueColor = Value > 0 ? FStyleColors::ForegroundHover : FStyleColors::Hover2;
		return SNew(SBox).MinDesiredWidth(104.f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
				.Padding(FMargin(10.f, 6.f))
				.ToolTipText(Tooltip)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock).Text(FText::AsNumber(Value)).Font(Font(16, true)).ColorAndOpacity(ValueColor)
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock).Text(Label).Font(Font(9)).ColorAndOpacity(FStyleColors::Foreground)
					]
				]
			];
	}

	/** Small colored label. */
	TSharedRef<SWidget> Badge(const FText& Text, const FSlateColor& Color, const FText& Tooltip = FText::GetEmpty())
	{
		return SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
			.Padding(FMargin(8.f, 2.f))
			.ToolTipText(Tooltip)
			[
				SNew(STextBlock).Text(Text).Font(Font(9, true)).ColorAndOpacity(Color)
			];
	}

	void OpenAsset(const FString& PackagePath)
	{
		const FString ObjectPath = PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
		if (UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath))
		{
			if (UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
			{
				Editors->OpenEditorForAsset(Asset);
			}
		}
	}

	FString EntryLine(const FDwoCanvasReport::FEntry& Entry)
	{
		FString Where = Entry.Document;
		if (!Entry.NodeName.IsEmpty())
		{
			Where += TEXT(" › ") + Entry.NodeName;
		}
		return Where.IsEmpty() ? Entry.Message : Where + TEXT("  —  ") + Entry.Message;
	}

	TSharedRef<SWidget> EntryRow(const FDwoCanvasReport::FEntry& Entry)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 2.f, 6.f, 0.f)
			[
				SNew(SImage).Image(SeverityIcon(Entry.Severity))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(STextBlock).Text(FText::FromString(EntryLine(Entry))).AutoWrapText(true).Font(Font(9))
			];
	}
}

void FDwoCanvasResultWindow::Show(const FDwoCanvasReport& Report, const FString& SourceFile, const FString& ReportFile, FSimpleDelegate OnReimport)
{
	using namespace DwoCanvasResultUI;

	const int32 Errors = Report.Count(EDwoCanvasSeverity::Error);
	const int32 Warnings = Report.Count(EDwoCanvasSeverity::Warning);
	const int32 Infos = Report.Count(EDwoCanvasSeverity::Info);

	// ---- Status
	FText Title;
	FText Subtitle;
	const FSlateBrush* StatusIcon = nullptr;
	if (Errors > 0)
	{
		Title = LOCTEXT("TitleFailed", "가져오기 실패");
		Subtitle = FText::Format(LOCTEXT("SubFailed", "오류 {0}건 때문에 일부 또는 전체가 반영되지 않았습니다. 아래 '확인할 항목'을 보세요."), Errors);
		StatusIcon = FAppStyle::GetBrush("Icons.ErrorWithColor.Large");
	}
	else if (Warnings > 0)
	{
		Title = LOCTEXT("TitleWarn", "가져오기 완료 · 확인 필요");
		Subtitle = FText::Format(LOCTEXT("SubWarn", "모두 반영됐지만 시안과 다르게 나올 수 있는 항목이 {0}건 있습니다."), Warnings);
		StatusIcon = FAppStyle::GetBrush("Icons.WarningWithColor.Large");
	}
	else
	{
		Title = LOCTEXT("TitleOk", "가져오기 완료");
		Subtitle = LOCTEXT("SubOk", "문제없이 반영됐습니다.");
		StatusIcon = FAppStyle::GetBrush("Icons.SuccessWithColor.Large");
	}

	// ---- Written blueprints (screens first)
	TArray<FDwoCanvasReport::FDocumentResult> Documents = Report.Documents.FilterByPredicate(
		[](const FDwoCanvasReport::FDocumentResult& Doc) { return !Doc.AssetPath.IsEmpty(); });
	Documents.StableSort([](const FDwoCanvasReport::FDocumentResult& A, const FDwoCanvasReport::FDocumentResult& B) { return !A.bComponent && B.bComponent; });

	const float WarnRatio = UDwoCanvasSettings::Get().VisualDiffWarnRatio;
	TSharedRef<SVerticalBox> DocumentList = SNew(SVerticalBox);
	for (const FDwoCanvasReport::FDocumentResult& Doc : Documents)
	{
		TSharedRef<SWrapBox> Badges = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(6.f, 4.f));
		if (Doc.bDraft)
		{
			Badges->AddSlot()[Badge(LOCTEXT("Draft", "추론 초안"), FStyleColors::Warning,
				LOCTEXT("DraftTip", "이미지·텍스트·낙서에서 추정해 만든 초안입니다. 새 블루프린트로 만들어지고 재임포트 보존 대상이 아닙니다"))];
		}
		if (Doc.BindingsMatched >= 0)
		{
			const bool bAll = Doc.BindingsMatched == Doc.BindingsTotal;
			Badges->AddSlot()[Badge(
				FText::Format(LOCTEXT("Bindings", "C++ 연결 {0}/{1}"), Doc.BindingsMatched, Doc.BindingsTotal),
				bAll ? FStyleColors::Success : FStyleColors::Error,
				LOCTEXT("BindingsTip", "부모 C++ 클래스가 이름으로 찾는 위젯(BindWidget)이 시안에 있는지"))];
		}
		if (Doc.DiffRatio >= 0.0)
		{
			const double Match = (1.0 - Doc.DiffRatio) * 100.0;
			const FSlateColor Color = Doc.DiffRatio <= WarnRatio * 0.5 ? FStyleColors::Success
				: (Doc.DiffRatio <= WarnRatio ? FStyleColors::Warning : FStyleColors::Error);
			Badges->AddSlot()[Badge(
				FText::Format(LOCTEXT("Match", "시안 일치 {0}%"), FText::AsNumber(Match, &FNumberFormattingOptions().SetMinimumFractionalDigits(1).SetMaximumFractionalDigits(1))),
				Color,
				LOCTEXT("MatchTip", "시안 미리보기와 UE 렌더 결과를 픽셀 단위로 비교한 값 (남는 차이는 보통 글자 렌더링)"))];
		}
		else if (!Doc.bComponent)
		{
			Badges->AddSlot()[Badge(LOCTEXT("NoPreview", "시안 비교 없음"), FStyleColors::Hover2,
				LOCTEXT("NoPreviewTip", "내보낸 파일에 시안 미리보기 이미지가 없거나, 에디터 밖(명령줄)에서 가져왔습니다"))];
		}

		const FString AssetPath = Doc.AssetPath;
		const FString DiffImage = Doc.DiffImage;
		const FString ExpectedImage = Doc.ExpectedImage;
		const FString ActualImage = Doc.ActualImage;

		DocumentList->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(10.f, 8.f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
						[
							SNew(STextBlock).Text(Doc.bComponent ? LOCTEXT("Component", "컴포넌트") : LOCTEXT("Screen", "화면"))
							.Font(Font(8, true)).ColorAndOpacity(FStyleColors::Hover2)
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(STextBlock).Text(FText::FromString(FPackageName::GetLongPackageAssetName(Doc.AssetPath))).Font(Font(11, true))
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 4.f)
					[
						SNew(STextBlock).Text(FText::FromString(Doc.AssetPath)).Font(Font(8)).ColorAndOpacity(FStyleColors::Hover2)
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						Badges
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Compare", "비교 보기"))
					.ToolTipText(LOCTEXT("CompareTip", "시안 / UE 렌더 / 차이(분홍) 이미지를 엽니다"))
					.Visibility(DiffImage.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
					.OnClicked_Lambda([DiffImage, ExpectedImage, ActualImage]()
					{
						for (const FString& Image : { ExpectedImage, ActualImage, DiffImage })
						{
							if (!Image.IsEmpty())
							{
								FPlatformProcess::LaunchFileInDefaultExternalApplication(*Image);
							}
						}
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f, 0.f, 0.f)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.Text(LOCTEXT("Open", "열기"))
					.ToolTipText(LOCTEXT("OpenTip", "위젯 블루프린트 에디터에서 엽니다"))
					.OnClicked_Lambda([AssetPath]() { OpenAsset(AssetPath); return FReply::Handled(); })
				]
			]
		];
	}
	if (Documents.Num() == 0)
	{
		DocumentList->AddSlot().AutoHeight()
		[
			SNew(STextBlock).Text(LOCTEXT("NoDocs", "저장된 위젯 블루프린트가 없습니다.")).ColorAndOpacity(FStyleColors::Hover2)
		];
	}

	// ---- Items to check (errors, warnings) and details (info)
	TSharedRef<SVerticalBox> IssueList = SNew(SVerticalBox);
	TSharedRef<SVerticalBox> InfoList = SNew(SVerticalBox);
	for (const EDwoCanvasSeverity Severity : { EDwoCanvasSeverity::Error, EDwoCanvasSeverity::Warning, EDwoCanvasSeverity::Info })
	{
		for (const FDwoCanvasReport::FEntry& Entry : Report.Entries)
		{
			if (Entry.Severity == Severity)
			{
				(Severity == EDwoCanvasSeverity::Info ? InfoList : IssueList)->AddSlot().AutoHeight().Padding(0.f, 2.f)[EntryRow(Entry)];
			}
		}
	}
	if (Errors + Warnings == 0)
	{
		IssueList->AddSlot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)[SNew(SImage).Image(FAppStyle::GetBrush("Icons.SuccessWithColor"))]
			+ SHorizontalBox::Slot().FillWidth(1.f)[SNew(STextBlock).Text(LOCTEXT("NoIssues", "확인할 항목이 없습니다.")).Font(Font(9))]
		];
	}

	const FString FullReport = FPaths::ConvertRelativePathToFull(ReportFile);
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("WindowTitle", "DwoCanvas - 가져오기 결과"))
		.ClientSize(FVector2D(760.f, 640.f))
		.SupportsMinimize(false)
		.SupportsMaximize(false);
	TWeakPtr<SWindow> WeakWindow = Window;

	Window->SetContent(
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(16.f)
		[
			SNew(SVerticalBox)

			// Status
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 12.f, 0.f)
				[
					SNew(SImage).Image(StatusIcon).DesiredSizeOverride(FVector2D(32.f, 32.f))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(Title).Font(Font(16, true))]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)[SNew(STextBlock).Text(Subtitle).AutoWrapText(true).Font(Font(10))]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock).Text(FText::FromString(FPaths::GetCleanFilename(SourceFile)
							+ (Report.SourceTool.IsEmpty() ? FString() : TEXT("  ·  원본: ") + Report.SourceTool)
							+ (Report.Adapter.IsEmpty() ? FString() : TEXT("  ·  ") + Report.Adapter)
							+ (Report.bDraft ? FString(TEXT("  ·  추론 초안")) : FString())))
						.Font(Font(8)).ColorAndOpacity(FStyleColors::Hover2)
					]
				]
			]

			// What changed
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 14.f, 0.f, 6.f)[Section(LOCTEXT("Changes", "변경 내용"))]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(6.f, 6.f))
				+ SWrapBox::Slot()[CountTile(LOCTEXT("Created", "새로 만듦"), Report.Created, LOCTEXT("CreatedTip", "시안에 새로 생긴 레이어로 만든 위젯"))]
				+ SWrapBox::Slot()[CountTile(LOCTEXT("Updated", "시안으로 갱신"), Report.Regenerated, LOCTEXT("UpdatedTip", "UE에서 손대지 않아 최신 시안으로 다시 만든 위젯 (이름은 그대로)"))]
				+ SWrapBox::Slot()[CountTile(LOCTEXT("KeptEdited", "UE 수정 보존"), Report.KeptEdited, LOCTEXT("KeptEditedTip", "UE에서 수정해서 덮어쓰지 않고 그대로 둔 위젯"))]
				+ SWrapBox::Slot()[CountTile(LOCTEXT("KeptAdded", "UE 추가 보존"), Report.KeptUserAdded, LOCTEXT("KeptAddedTip", "UE에서 직접 추가해서 원래 자리에 되돌려 놓은 위젯"))]
				+ SWrapBox::Slot()[CountTile(LOCTEXT("Removed", "삭제"), Report.Removed, LOCTEXT("RemovedTip", "시안에서 사라져서 지운 위젯 (아무도 수정·참조하지 않은 것만)"))]
				+ SWrapBox::Slot()[CountTile(LOCTEXT("Parked", "보관"), Report.Orphaned, LOCTEXT("ParkedTip", "시안에서 사라졌지만 수정·참조돼서 숨김 패널(DwoCanvas_Parked)로 옮긴 위젯"))]
			]

			// Results
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 14.f, 0.f, 6.f)[Section(LOCTEXT("Results", "만들어진 위젯 블루프린트"))]
			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()[DocumentList]
				+ SScrollBox::Slot().Padding(0.f, 8.f, 0.f, 6.f)[Section(FText::Format(LOCTEXT("Issues", "확인할 항목 ({0})"), Errors + Warnings))]
				+ SScrollBox::Slot()[IssueList]
				+ SScrollBox::Slot().Padding(0.f, 10.f, 0.f, 0.f)
				[
					SNew(SExpandableArea)
					.InitiallyCollapsed(true)
					.AreaTitle(FText::Format(LOCTEXT("Details", "자세한 정보 ({0})"), Infos))
					.BodyContent()[SNew(SBox).Padding(FMargin(4.f, 4.f))[InfoList]]
				]
			]

			// Actions
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 12.f, 0.f, 0.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton).Text(LOCTEXT("OpenReport", "리포트 파일"))
					.ToolTipText(FText::FromString(FullReport))
					.OnClicked_Lambda([FullReport]() { FPlatformProcess::LaunchFileInDefaultExternalApplication(*FullReport); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6.f, 0.f, 0.f, 0.f)
				[
					SNew(SButton).Text(LOCTEXT("OpenFolder", "폴더 열기"))
					.OnClicked_Lambda([FullReport]() { FPlatformProcess::ExploreFolder(*FullReport); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton).Text(LOCTEXT("Reimport", "다시 가져오기"))
					.ToolTipText(LOCTEXT("ReimportTip", "같은 파일을 다시 가져옵니다 (UE에서 수정한 위젯은 보존)"))
					.OnClicked_Lambda([WeakWindow, OnReimport]()
					{
						if (TSharedPtr<SWindow> Pinned = WeakWindow.Pin())
						{
							Pinned->RequestDestroyWindow();
						}
						OnReimport.ExecuteIfBound();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6.f, 0.f, 0.f, 0.f)
				[
					SNew(SButton).ButtonStyle(FAppStyle::Get(), "PrimaryButton").Text(LOCTEXT("Close", "닫기"))
					.OnClicked_Lambda([WeakWindow]()
					{
						if (TSharedPtr<SWindow> Pinned = WeakWindow.Pin())
						{
							Pinned->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]
			]
		]);

	FSlateApplication::Get().AddWindow(Window);
}

#undef LOCTEXT_NAMESPACE
