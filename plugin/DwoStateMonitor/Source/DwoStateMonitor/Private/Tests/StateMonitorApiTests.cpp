#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "StateMonitorLibrary.h"
#include "StateMonitorModel.h"
#include "StateMonitorSubsystem.h"
#include "StateMonitorWidget.h"
#include "Tests/StateMonitorTestTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStateMonitorApiTest, "StateMonitor.Api.SubsystemAndLibrary",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStateMonitorApiTest::RunTest(const FString& Parameters)
{
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->InitializeStandalone();
	UWorld* World = GameInstance->GetWorld();
	ON_SCOPE_EXIT
	{
		GameInstance->Shutdown();
		if (World)
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
	};

	UStateMonitorSubsystem* Monitor = UStateMonitorSubsystem::Get(World);
	if (!TestNotNull(TEXT("Every game instance owns a state monitor"), Monitor))
	{
		return false;
	}

	FStateMonitorTestNested Snapshot;
	Snapshot.Text = TEXT("First");
	TestTrue(TEXT("Template submit accepts a USTRUCT value"), Monitor->Submit(TEXT("Template"), Snapshot));
	const TSharedPtr<FStateMonitorSource> TemplateSource = Monitor->GetModel()->FindSource(TEXT("Template"));
	TestTrue(TEXT("Submitted source is recorded"), TemplateSource.IsValid());
	TestEqual(TEXT("An empty label falls back to the source id"),
	          TemplateSource.IsValid() ? TemplateSource->Label.ToString() : FString(), FString(TEXT("Template")));

	Snapshot.Text = TEXT("Second");
	TestTrue(TEXT("Library submit routes through the world context"),
	         UStateMonitorLibrary::SubmitStructSnapshot(World, TEXT("Library"), FText::FromString(TEXT("Library Source")),
	                                                    FStateMonitorTestNested::StaticStruct(), &Snapshot));
	TestTrue(TEXT("Library submit reaches the shared model"),
	         Monitor->GetModel()->FindSource(TEXT("Library")).IsValid());
	TestFalse(TEXT("A missing world context is rejected"),
	          UStateMonitorLibrary::SubmitStructSnapshot(nullptr, TEXT("Orphan"), FText(),
	                                                     FStateMonitorTestNested::StaticStruct(), &Snapshot));

	UStateMonitorWidget* Widget = NewObject<UStateMonitorWidget>(World);
	Widget->bUseSharedModel = true;
	TestTrue(TEXT("A shared-model widget displays the subsystem model"), Widget->GetMonitorModel() == Monitor->GetModel());

	TestTrue(TEXT("Removing a source through the library succeeds"),
	         UStateMonitorLibrary::RemoveStateSource(World, TEXT("Library")));
	TestFalse(TEXT("The removed source is gone"), Monitor->GetModel()->FindSource(TEXT("Library")).IsValid());

	// Automation worlds have no game viewport, so showing must fail without side effects.
	const bool bShown = UStateMonitorLibrary::ShowStateMonitor(World);
	TestEqual(TEXT("Visibility follows the show result"), Monitor->IsMonitorVisible(), bShown);
	UStateMonitorLibrary::HideStateMonitor(World);
	TestFalse(TEXT("Hide always leaves the monitor hidden"), Monitor->IsMonitorVisible());
	return true;
}

#endif
