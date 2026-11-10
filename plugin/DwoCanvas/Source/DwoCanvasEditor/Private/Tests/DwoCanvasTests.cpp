// DwoCanvas - automation tests (Session Frontend > Automation > DwoCanvas)

#include "DwoCanvasImporter.h"
#include "DwoCanvasSettings.h"
#include "DwoCanvasTestWidget.h"
#include "DwoCanvasVisualDiff.h"
#include "DwoCanvasWidgetWriter.h"
#include "DwoCanvasFrame.h"
#include "DwoCanvasShape.h"

#include "Blueprint/IUserObjectListEntry.h"
#include "Blueprint/WidgetTree.h"
#include "Components/BackgroundBlur.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/HorizontalBox.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/TileView.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "WidgetBlueprint.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace DwoCanvasTests
{
	const TCHAR* TestRoot = TEXT("/Game/DwoCanvasTests");

	/** Points every output folder at /Game/DwoCanvasTests and restores the settings afterwards. */
	struct FScopedTestSettings
	{
		UDwoCanvasSettings* Settings;
		FString WidgetFolder, ComponentFolder, TextureFolder, RecordDirectory, DraftFolder;
		EDwoCanvasLayoutMode LayoutMode;

		explicit FScopedTestSettings(const FString& Sub, EDwoCanvasLayoutMode Mode = EDwoCanvasLayoutMode::Native)
			: Settings(GetMutableDefault<UDwoCanvasSettings>())
		{
			WidgetFolder = Settings->WidgetFolder;
			ComponentFolder = Settings->ComponentFolder;
			TextureFolder = Settings->TextureFolder;
			RecordDirectory = Settings->RecordDirectory;
			LayoutMode = Settings->LayoutMode;
			DraftFolder = Settings->DraftFolder;

			const FString Root = FString(TestRoot) / Sub;
			Settings->WidgetFolder = Root;
			Settings->ComponentFolder = Root / TEXT("Components");
			Settings->TextureFolder = Root / TEXT("Textures");
			Settings->RecordDirectory = TEXT("Saved/DwoCanvasTests/Records");
			Settings->LayoutMode = Mode;
			Settings->DraftFolder = Root / TEXT("Drafts");
		}

		~FScopedTestSettings()
		{
			Settings->WidgetFolder = WidgetFolder;
			Settings->ComponentFolder = ComponentFolder;
			Settings->TextureFolder = TextureFolder;
			Settings->RecordDirectory = RecordDirectory;
			Settings->LayoutMode = LayoutMode;
			Settings->DraftFolder = DraftFolder;
		}
	};

	void DeleteTestAssets(const FString& Sub)
	{
		if (UEditorAssetSubsystem* Assets = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr)
		{
			const FString Dir = FString(TestRoot) / Sub;
			if (Assets->DoesDirectoryExist(Dir))
			{
				Assets->DeleteDirectory(Dir);
			}
		}
		IFileManager::Get().DeleteDirectory(*FPaths::Combine(FPaths::ProjectDir(), TEXT("Saved/DwoCanvasTests/Records/Game/DwoCanvasTests"), Sub), false, true);
	}

	UWidgetBlueprint* LoadWbp(const FString& Path)
	{
		return LoadObject<UWidgetBlueprint>(nullptr, *(Path + TEXT(".") + FPaths::GetBaseFilename(Path)));
	}

	template <typename T = UWidget>
	T* FindWidget(UWidgetBlueprint* Blueprint, const TCHAR* Name)
	{
		return Blueprint ? FindObjectFast<T>(Blueprint->WidgetTree, FName(Name)) : nullptr;
	}

	FString SamplePath()
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DwoCanvas"));
		return Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Samples/sample_shop.json")) : FString();
	}

	/** Small canvas document: { name, children[] } helpers for the re-import test. */
	FString ReimportDoc(const FString& TitleText, const FString& LineText, bool bWithGone, bool bWithNew)
	{
		FString Children = FString::Printf(TEXT(R"(
			{"id":"R:2","name":"Title","type":"text","box":{"x":10,"y":10,"w":200,"h":24},"text":{"content":"%s","font":{"family":"Roboto","style":"Bold","size":20},"color":[1,1,1,1],"autoResize":"width"}},
			{"id":"R:3","name":"Body","type":"frame","box":{"x":10,"y":50,"w":300,"h":200},"layout":{"mode":"vertical","gap":8,"padding":[4,4,4,4]},"children":[
				{"id":"R:4","name":"Line1","type":"text","box":{"x":4,"y":4,"w":200,"h":20},"text":{"content":"%s","font":{"family":"Roboto","style":"Regular","size":14},"autoResize":"width"}},
				{"id":"R:5","name":"Box","type":"rectangle","box":{"x":4,"y":32,"w":100,"h":40},"fills":[{"type":"solid","color":[0.2,0.4,0.8,1]}],"radius":[6,6,6,6]}
			]})"), *TitleText, *LineText);
		if (bWithGone)
		{
			Children += TEXT(R"(,{"id":"R:6","name":"Gone","type":"rectangle","box":{"x":320,"y":10,"w":40,"h":40},"fills":[{"type":"solid","color":[1,0,0,1]}]})");
		}
		if (bWithNew)
		{
			Children += TEXT(R"(,{"id":"R:7","name":"New","type":"rectangle","box":{"x":320,"y":60,"w":40,"h":40},"fills":[{"type":"solid","color":[0,1,0,1]}]})");
		}
		return FString::Printf(TEXT(R"({"schema":"designbridge/1","documents":[{"id":"R:1","name":"Reimport","root":
			{"id":"R:1","name":"Reimport","type":"frame","box":{"x":0,"y":0,"w":400,"h":300},"fills":[{"type":"solid","color":[0.1,0.1,0.1,1]}],"children":[%s]}}]})"), *Children);
	}

	bool Import(const FString& Json, FDwoCanvasReport& Report, bool bReset = false)
	{
		FDwoCanvasImportOptions Options;
		Options.bResetOverrides = bReset;
		Options.bVisualDiff = false;
		FDwoCanvasImporter Importer(Options);
		return Importer.ImportJson(Json, FPaths::ProjectDir(), Report);
	}
}

using namespace DwoCanvasTests;

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDwoCanvasSampleTest, "DwoCanvas.Import.Sample",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDwoCanvasSampleTest::RunTest(const FString& Parameters)
{
	DeleteTestAssets(TEXT("Sample"));
	{
		FScopedTestSettings Scoped(TEXT("Sample"));
		FString Json;
		TestTrue(TEXT("sample file"), FFileHelper::LoadFileToString(Json, *SamplePath()));

		FDwoCanvasReport Report;
		FDwoCanvasImportOptions Options;
		Options.bVisualDiff = false;
		FDwoCanvasImporter Importer(Options);
		TestTrue(TEXT("import succeeded"), Importer.ImportJson(Json, FPaths::GetPath(SamplePath()), Report));
		TestEqual(TEXT("no errors"), Report.Count(EDwoCanvasSeverity::Error), 0);

		UWidgetBlueprint* Shop = LoadWbp(FString(TestRoot) / TEXT("Sample/WBP_Shop"));
		UWidgetBlueprint* Card = LoadWbp(FString(TestRoot) / TEXT("Sample/Components/WBP_ItemCard"));
		TestNotNull(TEXT("WBP_Shop"), Shop);
		TestNotNull(TEXT("WBP_ItemCard"), Card);
		if (!Shop || !Card)
		{
			return false;
		}
		TestTrue(TEXT("Shop compiled"), Shop->Status != BS_Error);

		// Roles and names
		UButton* Close = FindWidget<UButton>(Shop, TEXT("Close"));
		TestNotNull(TEXT("btn_Close -> Button 'Close'"), Close);
		TestTrue(TEXT("Close is a variable"), Close && Close->bIsVariable);
		TestNotNull(TEXT("btn_Buy -> Button 'Buy'"), FindWidget<UButton>(Shop, TEXT("Buy")));

		UTileView* Items = FindWidget<UTileView>(Shop, TEXT("Items"));
		TestNotNull(TEXT("tile_Items -> TileView"), Items);
		TestTrue(TEXT("tile entry class = WBP_ItemCard"), Items && Items->GetEntryWidgetClass() == Card->GeneratedClass);

		UTextBlock* Score = FindWidget<UTextBlock>(Shop, TEXT("Score"));
		TestTrue(TEXT("$Score -> variable text"), Score && Score->bIsVariable);
		TestTrue(TEXT("upper case title"), FindWidget<UTextBlock>(Shop, TEXT("Title")) != nullptr);

		// Layout mapping
		TestNotNull(TEXT("Header -> HorizontalBox"), FindWidget<UHorizontalBox>(Shop, TEXT("Header")));
		TestNotNull(TEXT("Swatches -> HorizontalBox"), FindWidget<UHorizontalBox>(Shop, TEXT("Swatches")));
		TestNotNull(TEXT("gradient leaf -> DwoCanvasShape"), FindWidget<UDwoCanvasShape>(Shop, TEXT("GradientChip")));
		TestNotNull(TEXT("dashed stroke -> DwoCanvasShape"), FindWidget<UDwoCanvasShape>(Shop, TEXT("DashedChip")));
		TestNotNull(TEXT("inner shadow -> DwoCanvasShape"), FindWidget<UDwoCanvasShape>(Shop, TEXT("InsetChip")));
		TestNotNull(TEXT("circle -> Image (rounded box)"), FindWidget<UImage>(Shop, TEXT("Dot")));
		TestNotNull(TEXT("background blur"), FindWidget<UBackgroundBlur>(Shop, TEXT("Frosted_Blur")));
		TestNotNull(TEXT("baked vector"), FindWidget<UImage>(Shop, TEXT("StarIcon")));
		TestNull(TEXT("_notes ignored"), FindWidget(Shop, TEXT("notes")));
		TestNull(TEXT("group flattened"), FindWidget(Shop, TEXT("Decor")));
		TestNotNull(TEXT("group child kept"), FindWidget(Shop, TEXT("DecorA")));

		UWidget* Debug = FindWidget(Shop, TEXT("DebugOverlay"));
		TestTrue(TEXT("hidden layer collapsed"), Debug && Debug->GetVisibility() == ESlateVisibility::Collapsed);

		TestTrue(TEXT("ItemCard implements list entry"), Card->GeneratedClass->ImplementsInterface(UUserObjectListEntry::StaticClass()));
	}
	DeleteTestAssets(TEXT("Sample"));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDwoCanvasReimportTest, "DwoCanvas.Import.Reimport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDwoCanvasReimportTest::RunTest(const FString& Parameters)
{
	DeleteTestAssets(TEXT("Reimport"));
	{
		FScopedTestSettings Scoped(TEXT("Reimport"));
		const FString Path = FString(TestRoot) / TEXT("Reimport/WBP_Reimport");

		FDwoCanvasReport Report1;
		TestTrue(TEXT("v1 import"), Import(ReimportDoc(TEXT("Hello"), TEXT("Line"), true, false), Report1));
		UWidgetBlueprint* Blueprint = LoadWbp(Path);
		TestNotNull(TEXT("blueprint"), Blueprint);
		if (!Blueprint)
		{
			return false;
		}

		// Work done in UE: recolor the title, add a spacer into Body
		UTextBlock* Title = FindWidget<UTextBlock>(Blueprint, TEXT("Title"));
		UVerticalBox* Body = FindWidget<UVerticalBox>(Blueprint, TEXT("Body"));
		TestNotNull(TEXT("Title"), Title);
		TestNotNull(TEXT("Body"), Body);
		if (!Title || !Body)
		{
			return false;
		}
		Title->SetColorAndOpacity(FSlateColor(FLinearColor::Red));
		USpacer* UserSpacer = Cast<USpacer>(FDwoCanvasWidgetWriter::ConstructInTree(Blueprint, USpacer::StaticClass(), TEXT("UserSpacer")));
		Body->InsertChildAt(1, UserSpacer);
		Blueprint->OnVariableAdded(UserSpacer->GetFName()); // what the UMG designer does when a widget is added
		FString Error;
		FDwoCanvasWidgetWriter::CompileAndSave(Blueprint, Error);

		// Designer changes: title + line text, "Gone" removed, "New" added
		FDwoCanvasReport Report2;
		TestTrue(TEXT("v2 import"), Import(ReimportDoc(TEXT("Hello v2"), TEXT("Line v2"), false, true), Report2));
		TestEqual(TEXT("kept edited"), Report2.KeptEdited, 1);
		TestEqual(TEXT("kept user added"), Report2.KeptUserAdded, 1);
		TestTrue(TEXT("removed"), Report2.Removed >= 1);

		Title = FindWidget<UTextBlock>(Blueprint, TEXT("Title"));
		TestTrue(TEXT("edited title kept its color"), Title && Title->GetColorAndOpacity().GetSpecifiedColor() == FLinearColor::Red);
		TestTrue(TEXT("edited title kept its text"), Title && Title->GetText().ToString() == TEXT("Hello"));
		UTextBlock* Line = FindWidget<UTextBlock>(Blueprint, TEXT("Line1"));
		TestTrue(TEXT("untouched line updated"), Line && Line->GetText().ToString() == TEXT("Line v2"));
		UWidget* Spacer = FindWidget(Blueprint, TEXT("UserSpacer"));
		TestTrue(TEXT("user spacer back in Body"), Spacer && Spacer->GetParent() == FindWidget(Blueprint, TEXT("Body")));
		TestTrue(TEXT("user spacer index"), Spacer && Spacer->GetParent() && Spacer->GetParent()->GetChildIndex(Spacer) == 1);
		TestNull(TEXT("Gone removed"), FindWidget(Blueprint, TEXT("Gone")));
		TestNotNull(TEXT("New added"), FindWidget(Blueprint, TEXT("New")));

		// Lock persists across an unchanged import
		FDwoCanvasReport Report3;
		TestTrue(TEXT("v2 again"), Import(ReimportDoc(TEXT("Hello v2"), TEXT("Line v2"), false, true), Report3));
		Title = FindWidget<UTextBlock>(Blueprint, TEXT("Title"));
		TestTrue(TEXT("still locked"), Title && Title->GetText().ToString() == TEXT("Hello"));
		TestEqual(TEXT("only the locked widget kept"), Report3.KeptEdited, 1);

		// Reset overrides takes the design again
		FDwoCanvasReport Report4;
		TestTrue(TEXT("reset import"), Import(ReimportDoc(TEXT("Hello v2"), TEXT("Line v2"), false, true), Report4, true));
		Title = FindWidget<UTextBlock>(Blueprint, TEXT("Title"));
		TestTrue(TEXT("reset applied text"), Title && Title->GetText().ToString() == TEXT("Hello v2"));
		TestTrue(TEXT("reset applied color"), Title && Title->GetColorAndOpacity().GetSpecifiedColor() != FLinearColor::Red);
		TestTrue(TEXT("user widget survives reset"), FindWidget(Blueprint, TEXT("UserSpacer")) != nullptr);
	}
	DeleteTestAssets(TEXT("Reimport"));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDwoCanvasBindingTest, "DwoCanvas.Import.CppParentBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDwoCanvasBindingTest::RunTest(const FString& Parameters)
{
	DeleteTestAssets(TEXT("Bindings"));
	{
		FScopedTestSettings Scoped(TEXT("Bindings"));
		const FString Parent = UDwoCanvasTestWidget::StaticClass()->GetPathName();

		const FString Good = FString::Printf(TEXT(R"({"schema":"designbridge/1","documents":[{"id":"B:1","name":"Bound","parentClass":"%s","root":
			{"id":"B:1","name":"Bound","type":"frame","box":{"x":0,"y":0,"w":300,"h":120},"layout":{"mode":"vertical","gap":8,"padding":[8,8,8,8]},"children":[
				{"id":"B:2","name":"$Title","type":"text","box":{"x":8,"y":8,"w":100,"h":20},"text":{"content":"Bound title","font":{"family":"Roboto","style":"Regular","size":16}}},
				{"id":"B:3","name":"btn_Confirm","type":"frame","box":{"x":8,"y":36,"w":120,"h":36},"fills":[{"type":"solid","color":[0.1,0.5,0.9,1]}]}
			]}}]})"), *Parent);

		FDwoCanvasReport Report;
		TestTrue(TEXT("bound import"), Import(Good, Report));
		UWidgetBlueprint* Blueprint = LoadWbp(FString(TestRoot) / TEXT("Bindings/WBP_Bound"));
		TestTrue(TEXT("compiled"), Blueprint && Blueprint->Status != BS_Error);
		UTextBlock* Title = FindWidget<UTextBlock>(Blueprint, TEXT("Title"));
		TestTrue(TEXT("Title bound as variable"), Title && Title->bIsVariable);

		// Missing required BindWidget -> reported as an error (and the blueprint does not compile)
		const FString Bad = FString::Printf(TEXT(R"({"schema":"designbridge/1","documents":[{"id":"B:9","name":"Unbound","parentClass":"%s","root":
			{"id":"B:9","name":"Unbound","type":"frame","box":{"x":0,"y":0,"w":300,"h":120},"children":[
				{"id":"B:10","name":"Heading","type":"text","box":{"x":8,"y":8,"w":100,"h":20},"text":{"content":"x","font":{"family":"Roboto","style":"Regular","size":16}}}
			]}}]})"), *Parent);
		AddExpectedError(TEXT("expects a TextBlock named 'Title'"), EAutomationExpectedErrorFlags::Contains, 1);
		AddExpectedError(TEXT("Title"), EAutomationExpectedErrorFlags::Contains, 0);
		FDwoCanvasReport BadReport;
		Import(Bad, BadReport);
		TestTrue(TEXT("missing binding reported"), BadReport.HasErrors());
	}
	DeleteTestAssets(TEXT("Bindings"));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDwoCanvasFrameModeTest, "DwoCanvas.Import.DwoCanvasFrameMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDwoCanvasFrameModeTest::RunTest(const FString& Parameters)
{
	DeleteTestAssets(TEXT("FrameMode"));
	{
		FScopedTestSettings Scoped(TEXT("FrameMode"), EDwoCanvasLayoutMode::DwoCanvasFrame);
		FString Json;
		FFileHelper::LoadFileToString(Json, *SamplePath());
		FDwoCanvasReport Report;
		FDwoCanvasImportOptions Options;
		Options.bVisualDiff = false;
		FDwoCanvasImporter Importer(Options);
		TestTrue(TEXT("import"), Importer.ImportJson(Json, FPaths::GetPath(SamplePath()), Report));

		UWidgetBlueprint* Shop = LoadWbp(FString(TestRoot) / TEXT("FrameMode/WBP_Shop"));
		UDwoCanvasFrame* Header = FindWidget<UDwoCanvasFrame>(Shop, TEXT("Header"));
		TestNotNull(TEXT("Header -> DwoCanvasFrame"), Header);
		TestTrue(TEXT("Header draws its background"), Header && Header->bDrawBackground);
		TestNull(TEXT("no separate background widget"), FindWidget(Shop, TEXT("Header_Bg")));
		UDwoCanvasFrame* Panel = FindWidget<UDwoCanvasFrame>(Shop, TEXT("OfferPanel"));
		TestTrue(TEXT("panel gradient needs material"), Panel && Panel->Background.NeedsMaterial() && Panel->ShapeMaterial != nullptr);
	}
	DeleteTestAssets(TEXT("FrameMode"));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDwoCanvasGuardTest, "DwoCanvas.Import.ExistingContentGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDwoCanvasGuardTest::RunTest(const FString& Parameters)
{
	DeleteTestAssets(TEXT("Guard"));
	{
		FScopedTestSettings Scoped(TEXT("Guard"));
		const FString Path = FString(TestRoot) / TEXT("Guard/WBP_Reimport");

		// Hand-made widget blueprint with content
		bool bCreated = false;
		FString Error;
		UWidgetBlueprint* Blueprint = FDwoCanvasWidgetWriter::LoadOrCreate(Path, UUserWidget::StaticClass(), bCreated, Error);
		TestNotNull(TEXT("created"), Blueprint);
		if (!Blueprint)
		{
			return false;
		}
		FDwoCanvasWidgetWriter::ClearTree(Blueprint);
		UCanvasPanel* Root = Cast<UCanvasPanel>(FDwoCanvasWidgetWriter::ConstructInTree(Blueprint, UCanvasPanel::StaticClass(), TEXT("HandMadeRoot")));
		Root->AddChild(FDwoCanvasWidgetWriter::ConstructInTree(Blueprint, UTextBlock::StaticClass(), TEXT("HandMadeText")));
		Blueprint->WidgetTree->RootWidget = Root;
		FDwoCanvasWidgetWriter::CompileAndSave(Blueprint, Error);

		AddExpectedError(TEXT("did not create"), EAutomationExpectedErrorFlags::Contains, 1);
		FDwoCanvasReport Report;
		Import(ReimportDoc(TEXT("Hello"), TEXT("Line"), false, false), Report);
		TestTrue(TEXT("refused"), Report.HasErrors());
		TestNotNull(TEXT("hand-made content untouched"), FindWidget(Blueprint, TEXT("HandMadeText")));
	}
	DeleteTestAssets(TEXT("Guard"));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Fidelity details found by comparing a real Figma export with the UE render

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDwoCanvasFidelityTest, "DwoCanvas.Import.Fidelity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDwoCanvasFidelityTest::RunTest(const FString& Parameters)
{
	DeleteTestAssets(TEXT("Fidelity"));
	{
		FScopedTestSettings Scoped(TEXT("Fidelity"));
		const FString Json = TEXT(R"({"schema":"designbridge/1","documents":[{"id":"F:1","name":"Fidelity","root":
			{"id":"F:1","name":"Fidelity","type":"frame","box":{"x":0,"y":0,"w":400,"h":300},"fills":[{"type":"solid","color":[0.4,0.5,0.7,1]}],"children":[
				{"id":"F:2","name":"Card","type":"frame","box":{"x":10,"y":10,"w":200,"h":150},
					"fills":[{"type":"solid","color":[0.1,0.1,0.1,1],"opacity":0.9}],"radius":[8,8,8,8],
					"stroke":{"color":[1,1,1,0.08],"width":2,"align":"inside"},
					"effects":[{"type":"drop-shadow","color":[0,0,0,0.35],"offset":[0,6],"blur":18,"spread":0,"visible":true,"showBehind":true}],
					"layout":{"mode":"vertical","gap":4,"padding":[6,6,6,6],"strokesIncluded":true},
					"children":[
						{"id":"F:3","name":"Small","type":"text","box":{"x":8,"y":8,"w":40,"h":11},
							"text":{"content":"small","font":{"family":"Roboto","style":"Regular","size":9},"autoResize":"width"}}
					]}
			]}}]})");

		FDwoCanvasReport Report;
		TestTrue(TEXT("import"), Import(Json, Report));
		UWidgetBlueprint* Blueprint = LoadWbp(FString(TestRoot) / TEXT("Fidelity/WBP_Fidelity"));
		TestNotNull(TEXT("blueprint"), Blueprint);
		if (!Blueprint)
		{
			return false;
		}

		UTextBlock* Small = FindWidget<UTextBlock>(Blueprint, TEXT("Small"));
		TestNotNull(TEXT("Small text"), Small);
		if (Small)
		{
			// 9 px x 0.75 = 6.75 pt (was rounded to 7 pt = 9.33 px)
			TestEqual(TEXT("fractional font size"), Small->GetFont().Size, 6.75f, 0.001f);
			// Default (1, 1) shadow offset made every text 1 px taller / wider than designed
			TestEqual(TEXT("no shadow offset"), Small->GetShadowOffset(), FVector2D::ZeroVector);

			// strokesIncluded: padding 6 + inside stroke 2
			const FMargin Padding = Small->Slot && Small->Slot->IsA<UVerticalBoxSlot>() ? Cast<UVerticalBoxSlot>(Small->Slot)->GetPadding() : FMargin(-1.f);
			TestEqual(TEXT("padding includes the stroke (left)"), Padding.Left, 8.f);
			TestEqual(TEXT("padding includes the stroke (top)"), Padding.Top, 8.f);
		}

		const UDwoCanvasShape* Shape = nullptr;
		Blueprint->WidgetTree->ForEachWidget([&Shape](UWidget* Widget)
		{
			const UDwoCanvasShape* Candidate = Cast<UDwoCanvasShape>(Widget);
			if (Candidate && Candidate->GetStyle().DropShadow.bEnabled) { Shape = Candidate; }
		});
		TestNotNull(TEXT("card background shape"), Shape);
		if (Shape)
		{
			TestTrue(TEXT("drop shadow shows behind the translucent fill"), Shape->GetStyle().DropShadow.bShowBehindFill);
		}

		// Translucent stroke on a rounded shape goes through the SDF material (Slate's outline brightens the arc)
		FDwoCanvasShapeStyle Stroked;
		Stroked.FillType = EDwoCanvasFillType::Solid;
		Stroked.CornerRadii = FVector4(6.f, 6.f, 6.f, 6.f);
		Stroked.StrokeWidth = 1.f;
		Stroked.StrokeColor = FLinearColor(1.f, 1.f, 1.f, 0.08f);
		TestTrue(TEXT("translucent rounded stroke needs material"), Stroked.NeedsMaterial());
		Stroked.StrokeColor.A = 1.f;
		TestFalse(TEXT("opaque rounded stroke stays on the brush"), Stroked.NeedsMaterial());
	}
	DeleteTestAssets(TEXT("Fidelity"));

	// Wrap layout -> tile view: entries keep the design size, gaps go between them (left aligned)
	DeleteTestAssets(TEXT("FidelityTiles"));
	{
		FScopedTestSettings Scoped(TEXT("FidelityTiles"));
		FString Json;
		FFileHelper::LoadFileToString(Json, *SamplePath());
		FDwoCanvasReport Report;
		FDwoCanvasImportOptions Options;
		Options.bVisualDiff = false;
		FDwoCanvasImporter Importer(Options);
		Importer.ImportJson(Json, FPaths::GetPath(SamplePath()), Report);

		UTileView* Items = FindWidget<UTileView>(LoadWbp(FString(TestRoot) / TEXT("FidelityTiles/WBP_Shop")), TEXT("Items"));
		TestNotNull(TEXT("tile view"), Items);
		if (Items)
		{
			const FProperty* Alignment = FindFProperty<FProperty>(UTileView::StaticClass(), TEXT("TileAlignment"));
			TestTrue(TEXT("left aligned"), Alignment && *Alignment->ContainerPtrToValuePtr<EListItemAlignment>(Items) == EListItemAlignment::LeftAligned);
			TestEqual(TEXT("horizontal gap"), Items->GetHorizontalEntrySpacing(), 16.f);
			TestEqual(TEXT("vertical gap"), Items->GetVerticalEntrySpacing(), 16.f);
		}
	}
	DeleteTestAssets(TEXT("FidelityTiles"));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDwoCanvasRenderTest, "DwoCanvas.Render.Sample",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDwoCanvasRenderTest::RunTest(const FString& Parameters)
{
	if (!FDwoCanvasVisualDiff::CanRender())
	{
		AddInfo(TEXT("No renderer (commandlet / NullRHI): skipped"));
		return true;
	}

	DeleteTestAssets(TEXT("Render"));
	for (const EDwoCanvasLayoutMode Mode : { EDwoCanvasLayoutMode::Native, EDwoCanvasLayoutMode::DwoCanvasFrame })
	{
		const FString Sub = Mode == EDwoCanvasLayoutMode::Native ? TEXT("Render/Native") : TEXT("Render/Frame");
		FScopedTestSettings Scoped(Sub, Mode);
		FString Json;
		FFileHelper::LoadFileToString(Json, *SamplePath());
		FDwoCanvasReport Report;
		FDwoCanvasImportOptions Options;
		Options.bVisualDiff = false;
		FDwoCanvasImporter Importer(Options);
		Importer.ImportJson(Json, FPaths::GetPath(SamplePath()), Report);

		UWidgetBlueprint* Shop = LoadWbp(FString(TestRoot) / Sub / TEXT("WBP_Shop"));
		TArray<FColor> Pixels;
		const FIntPoint Size(1280, 720);
		TestTrue(TEXT("rendered"), FDwoCanvasVisualDiff::Render(Shop, Size, Pixels));
		if (Pixels.Num() != Size.X * Size.Y)
		{
			continue;
		}
		for (FColor& Pixel : Pixels) { Pixel.A = 255; }

		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
		const FString OutDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DwoCanvasTests"));
		IFileManager::Get().MakeDirectory(*OutDir, true);
		FFileHelper::SaveArrayToFile(Png, *FPaths::Combine(OutDir, Mode == EDwoCanvasLayoutMode::Native ? TEXT("Shop_Native.png") : TEXT("Shop_Frame.png")));

		// The diff pipeline against its own render must report ~0%
		FDwoCanvasReport DiffReport;
		TArray<uint8> Png32(Png.GetData(), (int32)Png.Num());
		FDwoCanvasVisualDiff::Compare(Shop, Png32, Size, 1.0, TEXT("SelfDiff"), DiffReport);
		const bool bZero = DiffReport.Entries.Num() == 1 && DiffReport.Entries[0].Severity == EDwoCanvasSeverity::Info && DiffReport.Entries[0].Message.Contains(TEXT(" 0.0%"));
		TestTrue(TEXT("self diff is 0%"), bZero);
		if (!bZero && DiffReport.Entries.Num() > 0)
		{
			AddInfo(DiffReport.Entries[0].Message);
		}
	}
	DeleteTestAssets(TEXT("Render"));
	return true;
}

#endif
