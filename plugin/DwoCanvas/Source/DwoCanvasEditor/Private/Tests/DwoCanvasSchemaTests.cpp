// DwoCanvas - automation tests for the dwocanvas/2 contract: schema, migration, roles, tokens, role handlers,
// inferred drafts, every bundled sample and the conformance cases.

#include "DwoCanvasImporter.h"
#include "DwoCanvasImportRecord.h"
#include "DwoCanvasRoleHandler.h"
#include "DwoCanvasSchema.h"
#include "DwoCanvasSettings.h"
#include "DwoCanvasVisualDiff.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/ListView.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "WidgetBlueprint.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace DwoCanvasContractTests
{
	const TCHAR* Root = TEXT("/Game/DwoCanvasTests/Contract");

	struct FScoped
	{
		UDwoCanvasSettings* S = GetMutableDefault<UDwoCanvasSettings>();
		FString WidgetFolder, ComponentFolder, TextureFolder, RecordDirectory, DraftFolder;
		TMap<FString, float> NumberTokens;
		TMap<FString, FLinearColor> ColorTokens;
		bool bStrict = true;

		explicit FScoped(const FString& Sub)
		{
			WidgetFolder = S->WidgetFolder; ComponentFolder = S->ComponentFolder; TextureFolder = S->TextureFolder;
			RecordDirectory = S->RecordDirectory; DraftFolder = S->DraftFolder; NumberTokens = S->NumberTokenOverrides;
			ColorTokens = S->ColorTokenOverrides; bStrict = S->bStrictSchema;
			const FString Dir = FString(Root) / Sub;
			S->WidgetFolder = Dir; S->ComponentFolder = Dir / TEXT("Components"); S->TextureFolder = Dir / TEXT("Textures");
			S->RecordDirectory = TEXT("Saved/DwoCanvasTests/Records"); S->DraftFolder = Dir / TEXT("Drafts");
			S->NumberTokenOverrides.Reset(); S->ColorTokenOverrides.Reset(); S->bStrictSchema = true;
		}
		~FScoped()
		{
			S->WidgetFolder = WidgetFolder; S->ComponentFolder = ComponentFolder; S->TextureFolder = TextureFolder;
			S->RecordDirectory = RecordDirectory; S->DraftFolder = DraftFolder; S->NumberTokenOverrides = NumberTokens;
			S->ColorTokenOverrides = ColorTokens; S->bStrictSchema = bStrict;
		}
	};

	void Clean(const FString& Sub)
	{
		if (UEditorAssetSubsystem* Assets = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr)
		{
			const FString Dir = FString(Root) / Sub;
			if (Assets->DoesDirectoryExist(Dir))
			{
				Assets->DeleteDirectory(Dir);
			}
		}
		IFileManager::Get().DeleteDirectory(*FPaths::Combine(FPaths::ProjectDir(), TEXT("Saved/DwoCanvasTests/Records/Game/DwoCanvasTests/Contract"), Sub), false, true);
	}

	bool Import(const FString& Json, FDwoCanvasReport& Report, const FString& Dir = FPaths::ProjectDir(), bool bVisualDiff = false)
	{
		FDwoCanvasImportOptions Options;
		Options.bVisualDiff = bVisualDiff;
		FDwoCanvasImporter Importer(Options);
		return Importer.ImportJson(Json, Dir, Report);
	}

	UWidgetBlueprint* Load(const FString& Path)
	{
		return LoadObject<UWidgetBlueprint>(nullptr, *(Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path)));
	}

	template <typename T = UWidget>
	T* Find(UWidgetBlueprint* Blueprint, const TCHAR* Name)
	{
		return Blueprint ? FindObjectFast<T>(Blueprint->WidgetTree, FName(Name)) : nullptr;
	}

	TSharedPtr<FJsonObject> Parse(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
		return Object;
	}

	bool HasEntry(const FDwoCanvasReport& Report, EDwoCanvasSeverity Severity, const TCHAR* Text)
	{
		return Report.Entries.ContainsByPredicate([Severity, Text](const FDwoCanvasReport::FEntry& E) { return E.Severity == Severity && E.Message.Contains(Text); });
	}

	FString PluginDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DwoCanvas"));
		return Plugin.IsValid() ? Plugin->GetBaseDir() : FString();
	}

	/** x-badge: a Border colored by the node's first fill, children inside. */
	class FBadgeHandler : public IDwoCanvasRoleHandler
	{
	public:
		virtual FString GetRole() const override { return TEXT("x-badge"); }
		virtual UWidget* Build(FDwoCanvasBuildContext& Context) override
		{
			bool bKept = false;
			UBorder* Border = Context.MakeWidget<UBorder>(TEXT(""), bKept);
			if (Border && !bKept)
			{
				const TArray<TSharedPtr<FJsonValue>>* Fills = nullptr;
				if (Context.GetNode()->TryGetArrayField(TEXT("fills"), Fills) && Fills->Num() > 0)
				{
					Border->SetBrushColor(Context.ResolveColor((*Fills)[0]->AsObject()));
				}
			}
			Context.BuildChildren(Border);
			return Border;
		}
	};
}

using namespace DwoCanvasContractTests;

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDwoCanvasSchemaValidationTest, "DwoCanvas.Schema.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDwoCanvasSchemaValidationTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Bad = Parse(TEXT(R"({"schema":"dwocanvas/2","bogus":1,"documents":[{"id":"","name":"X","root":
		{"id":"a","name":"A","type":"blob","role":"widgetish","layout":{"mode":"diagonal","padding":[1,2]},"opacity":3}}]})"));
	TArray<FString> Errors;
	TestFalse(TEXT("invalid document rejected"), FDwoCanvasSchema::Validate(Bad, Errors));
	auto Has = [&Errors](const TCHAR* Text) { return Errors.ContainsByPredicate([Text](const FString& E) { return E.Contains(Text); }); };
	TestTrue(TEXT("unknown top-level field"), Has(TEXT("unknown field \"bogus\"")));
	TestTrue(TEXT("empty id"), Has(TEXT("/documents/0/id")));
	TestTrue(TEXT("bad node type"), Has(TEXT("/documents/0/root/type")));
	TestTrue(TEXT("bad role"), Has(TEXT("/documents/0/root/role")));
	TestTrue(TEXT("bad layout mode"), Has(TEXT("/documents/0/root/layout/mode")));
	TestTrue(TEXT("short padding"), Has(TEXT("/documents/0/root/layout/padding")));
	TestTrue(TEXT("opacity range"), Has(TEXT("/documents/0/root/opacity")));

	const TSharedPtr<FJsonObject> Good = Parse(TEXT(R"({"schema":"dwocanvas/2","x-tool":{"a":1},"documents":[{"id":"d","name":"D","root":
		{"id":"r","name":"R","type":"frame","role":"x-panel","box":{"x":0,"y":0,"w":10,"h":10},"x-figma":{"k":1}}}]})"));
	Errors.Reset();
	TestTrue(TEXT("valid document with extensions"), FDwoCanvasSchema::Validate(Good, Errors));
	for (const FString& E : Errors) { AddInfo(E); }

	// Import refuses invalid documents (strict schema)
	FScoped Scoped(TEXT("Schema"));
	AddExpectedError(TEXT("Schema:"), EAutomationExpectedErrorFlags::Contains, 0);
	FDwoCanvasReport Report;
	TestFalse(TEXT("import refused"), Import(TEXT(R"({"schema":"dwocanvas/2","documents":[{"id":"d","name":"D","root":{"id":"r","name":"R","type":"blob"}}]})"), Report));
	TestTrue(TEXT("schema error reported"), Report.HasErrors());
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDwoCanvasMigrationTest, "DwoCanvas.Schema.Migration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDwoCanvasMigrationTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Doc = Parse(TEXT(R"({"schema":"designbridge/1","images":{"i":{"file":"a.png","scale":2}},"documents":[{"id":"d","name":"D","root":
		{"id":"r","name":"R","type":"frame","children":[
			{"id":"b","name":"Go","type":"frame","meta":{"role":"Button","variable":true},"fills":[{"type":"solid","color":[1,0,0,1],"token":"color.red"}]},
			{"id":"t","name":"T","type":"text","text":{"content":"x","font":{"family":"Roboto","style":"SemiBold Italic","size":12}}}
		]}}]})"));
	bool bMigrated = false;
	TestTrue(TEXT("migratable"), FDwoCanvasSchema::MigrateToCurrent(Doc, bMigrated));
	TestTrue(TEXT("migrated"), bMigrated);
	TestEqual(TEXT("schema id"), Doc->GetStringField(TEXT("schema")), FString(TEXT("dwocanvas/2")));
	TestFalse(TEXT("images removed"), Doc->HasField(TEXT("images")));
	TestEqual(TEXT("file -> uri"), Doc->GetObjectField(TEXT("assets"))->GetObjectField(TEXT("i"))->GetStringField(TEXT("uri")), FString(TEXT("a.png")));
	const TSharedPtr<FJsonObject> RootNode = Doc->GetArrayField(TEXT("documents"))[0]->AsObject()->GetObjectField(TEXT("root"));
	const TSharedPtr<FJsonObject> Button = RootNode->GetArrayField(TEXT("children"))[0]->AsObject();
	TestEqual(TEXT("meta.role -> role"), Button->GetStringField(TEXT("role")), FString(TEXT("button")));
	TestTrue(TEXT("meta.variable -> variable"), Button->GetBoolField(TEXT("variable")));
	TestEqual(TEXT("token -> colorToken"), Button->GetArrayField(TEXT("fills"))[0]->AsObject()->GetStringField(TEXT("colorToken")), FString(TEXT("color.red")));
	const TSharedPtr<FJsonObject> Font = RootNode->GetArrayField(TEXT("children"))[1]->AsObject()->GetObjectField(TEXT("text"))->GetObjectField(TEXT("font"));
	TestEqual(TEXT("style -> weight"), (int32)Font->GetNumberField(TEXT("weight")), 600);
	TestTrue(TEXT("style -> italic"), Font->GetBoolField(TEXT("italic")));

	TArray<FString> Errors;
	TestTrue(TEXT("migrated document is valid"), FDwoCanvasSchema::Validate(Doc, Errors));
	for (const FString& E : Errors) { AddError(E); }

	bool bAgain = false;
	FDwoCanvasSchema::MigrateToCurrent(Doc, bAgain);
	TestFalse(TEXT("idempotent"), bAgain);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDwoCanvasRolesTest, "DwoCanvas.Import.V2Roles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDwoCanvasRolesTest::RunTest(const FString& Parameters)
{
	Clean(TEXT("Roles"));
	{
		FScoped Scoped(TEXT("Roles"));
		// No naming prefixes: role / widgetName / variable come from the document (e.g. an HTML adapter)
		const FString Json = TEXT(R"({"schema":"dwocanvas/2","capabilities":["layout.flow","stable-ids"],"source":{"tool":"html","adapter":"test/1","mode":"extract"},
			"components":[{"id":"c","name":"Row","root":{"id":"c","name":"Row","type":"component","box":{"x":0,"y":0,"w":200,"h":30},
				"children":[{"id":"c1","name":"Label","type":"text","box":{"x":0,"y":0,"w":100,"h":20},"text":{"content":"Row"}}]}}],
			"documents":[{"id":"d","name":"Roles","root":{"id":"d","name":"Roles","type":"frame","box":{"x":0,"y":0,"w":400,"h":300},
				"layout":{"mode":"vertical","gap":8},"children":[
				{"id":"b","name":"Apply button","type":"frame","role":"button","widgetName":"Apply","box":{"x":0,"y":0,"w":120,"h":32},
					"fills":[{"type":"solid","color":[0.2,0.4,0.9,1]}],"children":[{"id":"bt","name":"label","type":"text","box":{"x":0,"y":0,"w":40,"h":16},"text":{"content":"Apply"}}]},
				{"id":"s","name":"volume","type":"frame","role":"slider","widgetName":"Volume","variable":false,"box":{"x":0,"y":40,"w":200,"h":16}},
				{"id":"l","name":"rows","type":"frame","role":"list","widgetName":"Rows","box":{"x":0,"y":60,"w":200,"h":100},"layout":{"mode":"vertical","gap":4},
					"children":[{"id":"l1","name":"r1","type":"instance","component":{"ref":"c","name":"Row"},"box":{"x":0,"y":0,"w":200,"h":30}}]},
				{"id":"n","name":"btn_NotARole","type":"frame","role":"auto","box":{"x":0,"y":170,"w":50,"h":20},"fills":[{"type":"solid","color":[1,1,1,1]}]}
			]}}]})");
		FDwoCanvasReport Report;
		TestTrue(TEXT("import"), Import(Json, Report));
		UWidgetBlueprint* Blueprint = Load(FString(Root) / TEXT("Roles/WBP_Roles"));
		UButton* Apply = Find<UButton>(Blueprint, TEXT("Apply"));
		TestTrue(TEXT("role button + widgetName"), Apply && Apply->bIsVariable);
		USlider* Volume = Find<USlider>(Blueprint, TEXT("Volume"));
		TestTrue(TEXT("role slider, variable:false honored"), Volume && !Volume->bIsVariable);
		TestNotNull(TEXT("role list"), Find<UListView>(Blueprint, TEXT("Rows")));
		TestNull(TEXT("explicit role auto beats the btn_ prefix"), Find<UButton>(Blueprint, TEXT("btn_NotARole")));
		TestEqual(TEXT("source recorded"), Report.SourceTool, FString(TEXT("html")));
	}
	Clean(TEXT("Roles"));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDwoCanvasTokensTest, "DwoCanvas.Import.Tokens",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDwoCanvasTokensTest::RunTest(const FString& Parameters)
{
	Clean(TEXT("Tokens"));
	{
		FScoped Scoped(TEXT("Tokens"));
		const FString Json = TEXT(R"({"schema":"dwocanvas/2","capabilities":["tokens","stable-ids"],
			"tokens":{"color":{"primary":{"$value":"#3366ff","$type":"color"},"accent":{"$value":"{color.primary}","$type":"color"}},
				"space":{"md":{"$value":"12px","$type":"dimension"}},"radius":{"lg":{"$value":16,"$type":"dimension"}}},
			"documents":[{"id":"d","name":"Tokens","root":{"id":"d","name":"Tokens","type":"frame","box":{"x":0,"y":0,"w":300,"h":100},
				"layout":{"mode":"horizontal"},"tokens":{"gap":"space.md"},"children":[
				{"id":"a","name":"Swatch","type":"rectangle","box":{"x":0,"y":0,"w":40,"h":40},"tokens":{"radius":"radius.lg"},
					"fills":[{"type":"solid","color":[0,0,0,1],"colorToken":"color.accent"}]},
				{"id":"b","name":"Second","type":"rectangle","box":{"x":52,"y":0,"w":40,"h":40},"fills":[{"type":"solid","color":[0,0,0,1],"colorToken":"color.missing"}]}
			]}}]})");
		FDwoCanvasReport Report;
		TestTrue(TEXT("import"), Import(Json, Report));
		TestTrue(TEXT("undefined token warned"), HasEntry(Report, EDwoCanvasSeverity::Warning, TEXT("color.missing")));
		UWidgetBlueprint* Blueprint = Load(FString(Root) / TEXT("Tokens/WBP_Tokens"));
		UImage* Swatch = Find<UImage>(Blueprint, TEXT("Swatch"));
		TestNotNull(TEXT("swatch"), Swatch);
		if (Swatch)
		{
			const FLinearColor Expected = FLinearColor::FromSRGBColor(FColor(0x33, 0x66, 0xFF));
			TestTrue(TEXT("alias color token"), Swatch->GetBrush().TintColor.GetSpecifiedColor().Equals(Expected, 0.01f));
			TestEqual(TEXT("radius token"), (float)Swatch->GetBrush().OutlineSettings.CornerRadii.X, 16.f);
		}
		UImage* Second = Find<UImage>(Blueprint, TEXT("Second"));
		const UHorizontalBoxSlot* Slot = Second ? Cast<UHorizontalBoxSlot>(Second->Slot) : nullptr;
		TestTrue(TEXT("gap token 12"), Slot && FMath::IsNearlyEqual(Slot->GetPadding().Left, 12.f));

		// Project overrides win over the document
		Scoped.S->NumberTokenOverrides.Add(TEXT("space.md"), 20.f);
		FDwoCanvasReport Report2;
		TestTrue(TEXT("reimport"), Import(Json, Report2));
		Second = Find<UImage>(Blueprint, TEXT("Second"));
		Slot = Second ? Cast<UHorizontalBoxSlot>(Second->Slot) : nullptr;
		TestTrue(TEXT("override gap 20"), Slot && FMath::IsNearlyEqual(Slot->GetPadding().Left, 20.f));
	}
	Clean(TEXT("Tokens"));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDwoCanvasRoleHandlerTest, "DwoCanvas.Import.RoleHandler",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDwoCanvasRoleHandlerTest::RunTest(const FString& Parameters)
{
	Clean(TEXT("Handler"));
	{
		FScoped Scoped(TEXT("Handler"));
		FDwoCanvasRoleHandlers::Register(MakeShared<FBadgeHandler>());
		ON_SCOPE_EXIT { FDwoCanvasRoleHandlers::Unregister(TEXT("x-badge")); };

		const FString Json = TEXT(R"({"schema":"dwocanvas/2","capabilities":["stable-ids"],"documents":[{"id":"d","name":"Handler","root":
			{"id":"d","name":"Handler","type":"frame","box":{"x":0,"y":0,"w":300,"h":100},"children":[
				{"id":"x","name":"New badge","type":"frame","role":"x-badge","widgetName":"NewBadge","box":{"x":10,"y":10,"w":60,"h":24},
					"fills":[{"type":"solid","color":[1,0,0,1]}],"children":[{"id":"xt","name":"BadgeText","type":"text","box":{"x":4,"y":4,"w":40,"h":16},"text":{"content":"NEW"}}]},
				{"id":"y","name":"Unknown","type":"frame","role":"x-unknown","box":{"x":100,"y":10,"w":60,"h":24},"fills":[{"type":"solid","color":[0,1,0,1]}]}
			]}}]})");
		FDwoCanvasReport Report;
		TestTrue(TEXT("import"), Import(Json, Report));
		UWidgetBlueprint* Blueprint = Load(FString(Root) / TEXT("Handler/WBP_Handler"));
		UBorder* Badge = Find<UBorder>(Blueprint, TEXT("NewBadge"));
		TestNotNull(TEXT("handler built a Border"), Badge);
		TestTrue(TEXT("handler colored it"), Badge && Badge->GetBrushColor().Equals(FLinearColor::Red, 0.01f));
		UTextBlock* Text = Find<UTextBlock>(Blueprint, TEXT("BadgeText"));
		TestTrue(TEXT("children built into it"), Text && Text->GetParent() == Badge);
		TestTrue(TEXT("unknown x- role warned"), HasEntry(Report, EDwoCanvasSeverity::Warning, TEXT("x-unknown")));

		// Kept across re-import like built-in widgets
		FDwoCanvasReport Report2;
		TestTrue(TEXT("reimport"), Import(Json, Report2));
		TestEqual(TEXT("nothing kept as edited"), Report2.KeptEdited, 0);
	}
	Clean(TEXT("Handler"));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDwoCanvasDraftTest, "DwoCanvas.Import.InferredDraft",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDwoCanvasDraftTest::RunTest(const FString& Parameters)
{
	Clean(TEXT("Draft"));
	{
		FScoped Scoped(TEXT("Draft"));
		const FString Json = TEXT(R"({"schema":"dwocanvas/2","capabilities":["layout.flow","inferred"],"source":{"tool":"inference","adapter":"test","mode":"draft","model":"mock"},
			"documents":[{"id":"d","name":"Sketch","root":{"id":"d","name":"Sketch","type":"frame","box":{"x":0,"y":0,"w":300,"h":200},"confidence":0.9,"children":[
				{"id":"b","name":"Guess","type":"frame","role":"button","widgetName":"Guess","confidence":0.3,"inferred":["role","box"],"box":{"x":10,"y":10,"w":80,"h":30},
					"fills":[{"type":"solid","color":[0.5,0.5,0.5,1]}]}
			]}}]})");
		FDwoCanvasReport Report;
		TestTrue(TEXT("import"), Import(Json, Report));
		TestTrue(TEXT("draft flagged"), Report.bDraft);
		TestTrue(TEXT("low confidence warned"), HasEntry(Report, EDwoCanvasSeverity::Warning, TEXT("low confidence (30%)")));
		const FString Draft = FString(Root) / TEXT("Draft/Drafts/WBP_Sketch");
		TestNotNull(TEXT("written to the draft folder"), Load(Draft));

		FDwoCanvasReport Report2;
		TestTrue(TEXT("second import"), Import(Json, Report2));
		TestNotNull(TEXT("second draft is a new blueprint"), Load(Draft + TEXT("_2")));
		TestFalse(TEXT("no re-import record"), IFileManager::Get().FileExists(*FDwoCanvasImportRecord::GetRecordFile(Draft)));
	}
	Clean(TEXT("Draft"));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDwoCanvasAllSamplesTest, "DwoCanvas.Import.AllSamples",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDwoCanvasAllSamplesTest::RunTest(const FString& Parameters)
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(PluginDir(), TEXT("Samples"), TEXT("*.json")), true, false);
	TestTrue(TEXT("samples found"), Files.Num() > 0);
	for (const FString& File : Files)
	{
		const FString Sub = TEXT("Samples_") + FPaths::GetBaseFilename(File).Replace(TEXT("."), TEXT("_"));
		Clean(Sub);
		{
			FScoped Scoped(Sub);
			FString Json;
			const FString Path = FPaths::Combine(PluginDir(), TEXT("Samples"), File);
			FFileHelper::LoadFileToString(Json, *Path);
			FDwoCanvasReport Report;
			const bool bOk = Import(Json, Report, FPaths::GetPath(Path));
			TestTrue(FString::Printf(TEXT("%s imports"), *File), bOk);
			for (const FDwoCanvasReport::FEntry& Entry : Report.Entries)
			{
				if (Entry.Severity == EDwoCanvasSeverity::Error)
				{
					AddError(FString::Printf(TEXT("%s: %s"), *File, *Entry.Message));
				}
			}
			AddInfo(FString::Printf(TEXT("%s: %d asset(s), %d warning(s)"), *File, Report.WrittenAssets.Num(), Report.Count(EDwoCanvasSeverity::Warning)));
		}
		Clean(Sub);
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Conformance: Conformance/<case>/<case>.dwocanvas.json, each with a preview rendered by its source tool.
// The UE render must match within x-conformance.maxDiff (default 5%).

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDwoCanvasConformanceTest, "DwoCanvas.Conformance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDwoCanvasConformanceTest::RunTest(const FString& Parameters)
{
	const FString Dir = FPaths::Combine(PluginDir(), TEXT("Conformance"));
	TArray<FString> Cases;
	IFileManager::Get().FindFilesRecursive(Cases, *Dir, TEXT("*.dwocanvas.json"), true, false);
	if (Cases.Num() == 0)
	{
		AddInfo(TEXT("No conformance cases"));
		return true;
	}
	if (!FDwoCanvasVisualDiff::CanRender())
	{
		AddInfo(TEXT("No renderer (commandlet / NullRHI): conformance skipped"));
		return true;
	}

	for (const FString& Case : Cases)
	{
		const FString Name = FPaths::GetBaseFilename(Case).Replace(TEXT(".dwocanvas"), TEXT(""));
		const FString Sub = TEXT("Conformance_") + Name;
		Clean(Sub);
		{
			FScoped Scoped(Sub);
			FString Json;
			FFileHelper::LoadFileToString(Json, *Case);
			const TSharedPtr<FJsonObject> Doc = Parse(Json);
			double MaxDiff = 0.05;
			const TSharedPtr<FJsonObject>* Extension = nullptr;
			if (Doc.IsValid() && Doc->TryGetObjectField(TEXT("x-conformance"), Extension))
			{
				(*Extension)->TryGetNumberField(TEXT("maxDiff"), MaxDiff);
			}

			FDwoCanvasReport Report;
			TestTrue(FString::Printf(TEXT("%s imports"), *Name), Import(Json, Report, FPaths::GetPath(Case), true));
			bool bCompared = false;
			for (const FDwoCanvasReport::FDocumentResult& Result : Report.Documents)
			{
				if (Result.DiffRatio < 0.0)
				{
					continue;
				}
				bCompared = true;
				AddInfo(FString::Printf(TEXT("%s/%s: %.2f%% differ (limit %.1f%%)"), *Name, *Result.Document, Result.DiffRatio * 100.0, MaxDiff * 100.0));
				TestTrue(FString::Printf(TEXT("%s/%s within %.1f%%"), *Name, *Result.Document, MaxDiff * 100.0), Result.DiffRatio <= MaxDiff);
			}
			TestTrue(FString::Printf(TEXT("%s has a preview to compare"), *Name), bCompared);
		}
		Clean(Sub);
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Text metrics found by the HTML conformance cases: weight without a style name, letter spacing in whole pixels

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDwoCanvasTextMetricsTest, "DwoCanvas.Import.TextMetrics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDwoCanvasTextMetricsTest::RunTest(const FString& Parameters)
{
	Clean(TEXT("Text"));
	{
		FScoped Scoped(TEXT("Text"));
		const FString Json = TEXT(R"({"schema":"dwocanvas/2","capabilities":["stable-ids"],"documents":[{"id":"d","name":"Text","root":
			{"id":"d","name":"Text","type":"frame","box":{"x":0,"y":0,"w":300,"h":100},"children":[
				{"id":"a","name":"Heavy","type":"text","box":{"x":0,"y":0,"w":100,"h":16},"text":{"content":"Bold by weight","font":{"family":"Roboto","weight":700,"size":12}}},
				{"id":"b","name":"Spaced","type":"text","box":{"x":0,"y":20,"w":200,"h":16},"text":{"content":"SPACED","font":{"family":"Roboto","weight":400,"size":12},"letterSpacing":{"unit":"px","value":2}}},
				{"id":"c","name":"SpacedPercent","type":"text","box":{"x":0,"y":40,"w":200,"h":16},"text":{"content":"SPACED","font":{"family":"Roboto","weight":400,"size":16},"letterSpacing":{"unit":"percent","value":10}}}
			]}}]})");
		FDwoCanvasReport Report;
		TestTrue(TEXT("import"), Import(Json, Report));
		UWidgetBlueprint* Blueprint = Load(FString(Root) / TEXT("Text/WBP_Text"));

		UTextBlock* Heavy = Find<UTextBlock>(Blueprint, TEXT("Heavy"));
		TestTrue(TEXT("weight 700 without style -> Bold"), Heavy && Heavy->GetFont().TypefaceFontName == FName(TEXT("Bold")));

		// Slate per-glyph spacing = trunc(LetterSpacing * Size(pt) / 1000) must equal the design px
		auto GlyphPx = [](const UTextBlock* Text) { return Text ? (int32)(Text->GetFont().LetterSpacing * Text->GetFont().Size / 1000.f) : -1; };
		TestEqual(TEXT("2px at 12px"), GlyphPx(Find<UTextBlock>(Blueprint, TEXT("Spaced"))), 2);
		TestEqual(TEXT("10% of 16px = 1.6 -> 2px"), GlyphPx(Find<UTextBlock>(Blueprint, TEXT("SpacedPercent"))), 2);
	}
	Clean(TEXT("Text"));
	return true;
}

#endif
