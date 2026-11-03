#if WITH_DEV_AUTOMATION_TESTS

#include "StateMonitorLibrary.h"
#include "StateMonitorLiveProtocol.h"
#include "StateMonitorModel.h"
#include "StateMonitorSubsystem.h"
#include "Tests/StateMonitorTestTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace StateMonitorLiveTests
{
TSharedPtr<FJsonObject> ParseLine(const FString& Line)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Line), Object);
	return Object;
}
} // namespace StateMonitorLiveTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStateMonitorLiveHelloTest, "StateMonitor.Live.HelloWorldRemoveLines",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStateMonitorLiveHelloTest::RunTest(const FString& Parameters)
{
	using namespace StateMonitorLiveTests;

	FStateMonitorLiveHello Hello;
	Hello.Project = TEXT("temp_proj");
	Hello.World = TEXT("Map \"A\"");
	Hello.NetMode = TEXT("ListenServer");
	Hello.PieInstance = 1;
	Hello.ProcessId = 1234;
	Hello.EngineVersion = TEXT("5.7.4");
	Hello.BuildConfiguration = TEXT("Development");
	Hello.TimeSeconds = 12.5;
	const FString HelloLine = StateMonitorLive::MakeHelloLine(Hello);
	TestFalse(TEXT("A hello line has no raw newline"), HelloLine.Contains(TEXT("\n")));
	const TSharedPtr<FJsonObject> HelloJson = ParseLine(HelloLine);
	if (!TestTrue(TEXT("Hello is valid JSON"), HelloJson.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Hello type"), HelloJson->GetStringField(TEXT("t")), FString(TEXT("hello")));
	TestEqual(TEXT("Protocol version"), static_cast<int32>(HelloJson->GetNumberField(TEXT("v"))), 1);
	TestEqual(TEXT("Project"), HelloJson->GetStringField(TEXT("project")), Hello.Project);
	TestEqual(TEXT("World is escaped"), HelloJson->GetStringField(TEXT("world")), Hello.World);
	TestEqual(TEXT("Net mode"), HelloJson->GetStringField(TEXT("netMode")), Hello.NetMode);
	TestEqual(TEXT("PIE instance"), static_cast<int32>(HelloJson->GetNumberField(TEXT("pie"))), 1);
	TestEqual(TEXT("Process id"), static_cast<int32>(HelloJson->GetNumberField(TEXT("pid"))), 1234);
	TestEqual(TEXT("Engine"), HelloJson->GetStringField(TEXT("engine")), Hello.EngineVersion);
	TestEqual(TEXT("Build"), HelloJson->GetStringField(TEXT("build")), Hello.BuildConfiguration);
	TestEqual(TEXT("Time"), HelloJson->GetNumberField(TEXT("time")), 12.5);

	const TSharedPtr<FJsonObject> World = ParseLine(StateMonitorLive::MakeWorldLine(TEXT("Lobby")));
	TestTrue(TEXT("World line is valid"), World.IsValid() && World->GetStringField(TEXT("t")) == TEXT("world") &&
	                                          World->GetStringField(TEXT("world")) == TEXT("Lobby"));

	const TSharedPtr<FJsonObject> Remove = ParseLine(StateMonitorLive::MakeRemoveLine(TEXT("PC0.Pawn")));
	TestTrue(TEXT("Remove line is valid"), Remove.IsValid() && Remove->GetStringField(TEXT("t")) == TEXT("remove") &&
	                                           Remove->GetStringField(TEXT("id")) == TEXT("PC0.Pawn"));

	TestEqual(TEXT("Compacting removes formatting only"),
	          StateMonitorLive::CompactJson(TEXT("{\n  \"A\": \"x  y\",\n  \"B\": [\n    1\n  ]\n}")),
	          FString(TEXT("{\"A\": \"x  y\",\"B\": [1]}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStateMonitorLiveSnapTest, "StateMonitor.Live.SnapLine",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStateMonitorLiveSnapTest::RunTest(const FString& Parameters)
{
	using namespace StateMonitorLiveTests;

	FStateMonitorModel Model;
	FStateMonitorTestState State;
	State.Nested.Text = TEXT("quote \" newline\n한국어");
	State.Items = {1, 2};
	Model.SubmitSnapshot(TEXT("Struct"), FText::FromString(TEXT("Struct label")), FStateMonitorTestState::StaticStruct(),
	                     &State);
	State.Items[1] = 5;
	Model.SubmitSnapshot(TEXT("Struct"), FText::FromString(TEXT("Struct label")), FStateMonitorTestState::StaticStruct(),
	                     &State);
	const TSharedPtr<FStateMonitorSource> StructSource = Model.FindSource(TEXT("Struct"));
	const FString StructLine = StateMonitorLive::MakeSnapLine(*StructSource, false);
	TestFalse(TEXT("A snap line has no raw newline"), StructLine.Contains(TEXT("\n")));
	const TSharedPtr<FJsonObject> Snap = ParseLine(StructLine);
	if (!TestTrue(TEXT("Struct snap is valid JSON"), Snap.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Snap type"), Snap->GetStringField(TEXT("t")), FString(TEXT("snap")));
	TestEqual(TEXT("Snap id"), Snap->GetStringField(TEXT("id")), FString(TEXT("Struct")));
	TestEqual(TEXT("Snap label"), Snap->GetStringField(TEXT("label")), FString(TEXT("Struct label")));
	TestEqual(TEXT("Struct type is the short name"), Snap->GetStringField(TEXT("type")),
	          FString(TEXT("StateMonitorTestState")));
	TestEqual(TEXT("Struct kind"), Snap->GetStringField(TEXT("kind")), FString(TEXT("struct")));
	TestEqual(TEXT("Struct sources have no object"), Snap->GetStringField(TEXT("object")), FString());
	TestFalse(TEXT("Manual sources are not auto"), Snap->GetBoolField(TEXT("auto")));
	TestEqual(TEXT("Sequence"), static_cast<uint64>(Snap->GetNumberField(TEXT("seq"))), StructSource->Sequence);
	TestFalse(TEXT("Not truncated"), Snap->GetBoolField(TEXT("truncated")));

	const TSharedPtr<FJsonObject>* Value = nullptr;
	if (TestTrue(TEXT("Value is embedded as an object"), Snap->TryGetObjectField(TEXT("value"), Value)))
	{
		const TSharedPtr<FJsonObject>* Nested = nullptr;
		TestTrue(TEXT("Nested text survives escaping"),
		         (*Value)->TryGetObjectField(TEXT("Nested"), Nested) &&
		             (*Nested)->GetStringField(TEXT("Text")) == State.Nested.Text);
	}

	const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
	if (TestTrue(TEXT("Changes is an array"), Snap->TryGetArrayField(TEXT("changes"), Changes)))
	{
		const bool bFound = Changes->ContainsByPredicate(
		    [](const TSharedPtr<FJsonValue>& Change)
		    {
			    const TSharedPtr<FJsonObject> Object = Change->AsObject();
			    return Object.IsValid() && Object->GetStringField(TEXT("path")) == TEXT("Items/1") &&
			           Object->GetStringField(TEXT("old")) == TEXT("2") &&
			           Object->GetStringField(TEXT("new")) == TEXT("5") &&
			           Object->GetStringField(TEXT("change")) == TEXT("modified");
		    });
		TestTrue(TEXT("Changes carry viewer paths, old and new values"), bFound);
	}

	UStateMonitorTestComponent* Component = NewObject<UStateMonitorTestComponent>(GetTransientPackage());
	UStateMonitorTestComponent* Other = NewObject<UStateMonitorTestComponent>(GetTransientPackage());
	Component->Target = Other;
	Model.SubmitObjectSnapshot(TEXT("PC0.Pawn.Health"), FText(), Component);
	const TSharedPtr<FJsonObject> ObjectSnap =
	    ParseLine(StateMonitorLive::MakeSnapLine(*Model.FindSource(TEXT("PC0.Pawn.Health")), true));
	if (!TestTrue(TEXT("Object snap is valid JSON"), ObjectSnap.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Object kind"), ObjectSnap->GetStringField(TEXT("kind")), FString(TEXT("object")));
	TestEqual(TEXT("Object class name"), ObjectSnap->GetStringField(TEXT("type")),
	          FString(TEXT("StateMonitorTestComponent")));
	TestEqual(TEXT("Object path"), ObjectSnap->GetStringField(TEXT("object")), Component->GetPathName());
	TestTrue(TEXT("Auto flag"), ObjectSnap->GetBoolField(TEXT("auto")));
	const TSharedPtr<FJsonObject>* ObjectValue = nullptr;
	TestTrue(TEXT("Object references inside values are object paths"),
	         ObjectSnap->TryGetObjectField(TEXT("value"), ObjectValue) &&
	             (*ObjectValue)->GetStringField(TEXT("Target")) == Other->GetPathName());

	FStateMonitorModel EmptyModel;
	FStateMonitorTestNested Empty;
	EmptyModel.SubmitSnapshot(TEXT("Nested"), FText(), FStateMonitorTestNested::StaticStruct(), &Empty);
	TestTrue(TEXT("Every snap embeds an object value"),
	         ParseLine(StateMonitorLive::MakeSnapLine(*EmptyModel.FindSource(TEXT("Nested")), false)).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStateMonitorLiveCommandTest, "StateMonitor.Live.ParseCommands",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStateMonitorLiveCommandTest::RunTest(const FString& Parameters)
{
	FStateMonitorLiveCommand Command;
	TestTrue(TEXT("auto parses"),
	         StateMonitorLive::ParseCommand(TEXT("{\"t\":\"auto\",\"on\":true,\"interval\":0.5,\"engine\":true}"), Command));
	TestTrue(TEXT("auto fields"), Command.Type == EStateMonitorLiveCommand::Auto && Command.bOn && Command.bHasInterval &&
	                                  FMath::IsNearlyEqual(Command.Interval, 0.5) && Command.bHasEngine &&
	                                  Command.bIncludeEngineProperties);

	TestTrue(TEXT("auto without options parses"), StateMonitorLive::ParseCommand(TEXT("{\"t\":\"auto\",\"on\":false}"), Command));
	TestTrue(TEXT("missing options are reported"), !Command.bOn && !Command.bHasInterval && !Command.bHasEngine);

	TestTrue(TEXT("interval is clamped"),
	         StateMonitorLive::ParseCommand(TEXT("{\"t\":\"auto\",\"on\":true,\"interval\":0}"), Command) &&
	             Command.Interval >= 0.05);

	TestTrue(TEXT("watch parses"),
	         StateMonitorLive::ParseCommand(TEXT("{\"t\":\"watch\",\"class\":\"BP_Enemy_C\",\"max\":4}"), Command));
	TestTrue(TEXT("watch fields"), Command.Type == EStateMonitorLiveCommand::Watch &&
	                                   Command.ClassName == TEXT("BP_Enemy_C") && Command.Max == 4);
	TestTrue(TEXT("watch defaults max"),
	         StateMonitorLive::ParseCommand(TEXT("{\"t\":\"watch\",\"class\":\"BP_Enemy_C\"}"), Command) &&
	             Command.Max == 16);
	TestFalse(TEXT("watch requires a class"), StateMonitorLive::ParseCommand(TEXT("{\"t\":\"watch\"}"), Command));

	TestTrue(TEXT("unwatch parses"),
	         StateMonitorLive::ParseCommand(TEXT("{\"t\":\"unwatch\",\"class\":\"BP_Enemy_C\"}"), Command) &&
	             Command.Type == EStateMonitorLiveCommand::Unwatch);
	TestTrue(TEXT("pause parses"),
	         StateMonitorLive::ParseCommand(TEXT("{\"t\":\"pause\",\"on\":true}"), Command) &&
	             Command.Type == EStateMonitorLiveCommand::Pause && Command.bOn);
	TestTrue(TEXT("resync parses"), StateMonitorLive::ParseCommand(TEXT(" {\"t\":\"resync\",\"extra\":[1,2]} "), Command) &&
	                                    Command.Type == EStateMonitorLiveCommand::Resync);

	TestFalse(TEXT("unknown commands are ignored"), StateMonitorLive::ParseCommand(TEXT("{\"t\":\"explode\"}"), Command));
	TestFalse(TEXT("non-JSON is ignored"), StateMonitorLive::ParseCommand(TEXT("hello"), Command));
	TestFalse(TEXT("missing type is ignored"), StateMonitorLive::ParseCommand(TEXT("{\"on\":true}"), Command));
	TestFalse(TEXT("wrong field types are ignored"), StateMonitorLive::ParseCommand(TEXT("{\"t\":5}"), Command));
	TestFalse(TEXT("arrays are ignored"), StateMonitorLive::ParseCommand(TEXT("[1,2]"), Command));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStateMonitorLiveWatchTest, "StateMonitor.Live.WatchedObjectsAndManualSources",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStateMonitorLiveWatchTest::RunTest(const FString& Parameters)
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
	if (!TestNotNull(TEXT("Monitor exists"), Monitor))
	{
		return false;
	}

	FStateMonitorTestNested Manual;
	Monitor->Submit(TEXT("Collide"), Manual);
	UStateMonitorTestComponent* Watched = NewObject<UStateMonitorTestComponent>(World, TEXT("WatchedComponent"));
	UStateMonitorTestComponent* Colliding = NewObject<UStateMonitorTestComponent>(World, TEXT("Collide"));
	TestTrue(TEXT("Watch State Object accepts an object"), UStateMonitorLibrary::WatchStateObject(World, Watched));
	TestTrue(TEXT("A colliding object can be watched"), UStateMonitorLibrary::WatchStateObject(World, Colliding));
	TestFalse(TEXT("A null object cannot be watched"), UStateMonitorLibrary::WatchStateObject(World, nullptr));
	TestTrue(TEXT("Watched objects are tracked"), Monitor->IsWatchingObject(Watched));

	Monitor->UpdateAutoSources(true);
	const TSharedPtr<FStateMonitorModel> Model = Monitor->GetModel();
	const TSharedPtr<FStateMonitorSource> WatchedSource = Model->FindSource(TEXT("WatchedComponent"));
	TestTrue(TEXT("A watched object becomes an object source"), WatchedSource.IsValid() && WatchedSource->bObject);
	TestTrue(TEXT("Watched sources are auto sources"), Monitor->IsAutoSource(TEXT("WatchedComponent")));
	const TSharedPtr<FStateMonitorSource> ManualSource = Model->FindSource(TEXT("Collide"));
	TestTrue(TEXT("Auto observation never replaces a manual source"), ManualSource.IsValid() && !ManualSource->bObject);
	TestFalse(TEXT("The manual source is not auto"), Monitor->IsAutoSource(TEXT("Collide")));

	Watched->MarkAsGarbage();
	Monitor->UpdateAutoSources(true);
	TestFalse(TEXT("Sources of dead objects are removed"), Model->FindSource(TEXT("WatchedComponent")).IsValid());
	TestFalse(TEXT("Dead objects leave the watch list"), Monitor->IsWatchingObject(Watched));

	UStateMonitorTestComponent* Again = NewObject<UStateMonitorTestComponent>(World, TEXT("WatchedAgain"));
	Monitor->WatchObject(Again);
	Monitor->UpdateAutoSources(true);
	TestTrue(TEXT("A second watched object appears"), Model->FindSource(TEXT("WatchedAgain")).IsValid());
	UStateMonitorLibrary::UnwatchStateObject(World, Again);
	Monitor->UpdateAutoSources(true);
	TestFalse(TEXT("Unwatching removes the auto source"), Model->FindSource(TEXT("WatchedAgain")).IsValid());

	Monitor->UpdateAutoSources(false);
	TestTrue(TEXT("Stopping auto observation keeps manual sources"), Model->FindSource(TEXT("Collide")).IsValid());
	return true;
}

#endif
