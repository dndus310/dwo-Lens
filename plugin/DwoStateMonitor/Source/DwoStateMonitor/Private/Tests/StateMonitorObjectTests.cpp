#if WITH_DEV_AUTOMATION_TESTS

#include "StateMonitorModel.h"
#include "Tests/StateMonitorTestTypes.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace StateMonitorObjectTests
{
bool HasChild(const TSharedPtr<FStateMonitorNode>& Root, const FString& Name)
{
	return Root.IsValid() && Root->Children.ContainsByPredicate(
	                             [&Name](const TSharedPtr<FStateMonitorNode>& Child)
	                             {
		                             return Child->Name == Name;
	                             });
}

TSharedPtr<FJsonObject> ParseObject(const FString& Json)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
	return Object;
}
} // namespace StateMonitorObjectTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStateMonitorObjectCaptureTest, "StateMonitor.Model.ObjectCapture",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStateMonitorObjectCaptureTest::RunTest(const FString& Parameters)
{
	using namespace StateMonitorObjectTests;

	FStateMonitorModel Model;
	UStateMonitorTestComponent* Component = NewObject<UStateMonitorTestComponent>(GetTransientPackage());
	UStateMonitorTestComponent* Other = NewObject<UStateMonitorTestComponent>(GetTransientPackage());
	Component->Health = 90.0f;
	Component->Nested.Text = TEXT("line\nbreak");
	Component->Target = Other;

	TestFalse(TEXT("A null object is rejected"), Model.SubmitObjectSnapshot(TEXT("Null"), FText(), nullptr));
	TestTrue(TEXT("An object snapshot is accepted"),
	         Model.SubmitObjectSnapshot(TEXT("Component"), FText::FromString(TEXT("Component")), Component));
	const TSharedPtr<FStateMonitorSource> Source = Model.FindSource(TEXT("Component"));
	if (!TestTrue(TEXT("Object source exists"), Source.IsValid()))
	{
		return false;
	}

	TestTrue(TEXT("Object sources are marked as objects"), Source->bObject);
	TestEqual(TEXT("Object path is recorded"), Source->ObjectPath, Component->GetPathName());
	TestTrue(TEXT("Type is the object's class"), Source->StructType.EndsWith(TEXT("StateMonitorTestComponent")));
	TestTrue(TEXT("Project property is captured"), HasChild(Source->Root, TEXT("Health")));
	TestTrue(TEXT("Nested struct property is captured"), HasChild(Source->Root, TEXT("Nested")));
	TestTrue(TEXT("Project transient property is kept"), HasChild(Source->Root, TEXT("RuntimeCounter")));
	TestFalse(TEXT("Delegates are skipped"), HasChild(Source->Root, TEXT("OnTestEvent")));
	TestFalse(TEXT("Engine properties are excluded by default"), HasChild(Source->Root, TEXT("ComponentTags")));

	const TSharedPtr<FJsonObject> Json = ParseObject(Source->CurrentJson);
	if (!TestTrue(TEXT("Object JSON is a valid object"), Json.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Numbers keep their value"), Json->GetNumberField(TEXT("Health")), 90.0);
	TestEqual(TEXT("Object references are object paths"), Json->GetStringField(TEXT("Target")),
	          Other->GetPathName());

	Component->Health = 80.0f;
	TestTrue(TEXT("Resubmitting a changed object succeeds"),
	         Model.SubmitObjectSnapshot(TEXT("Component"), FText::FromString(TEXT("Component")), Component));
	const TSharedPtr<FStateMonitorSource> Changed = Model.FindSource(TEXT("Component"));
	TestTrue(TEXT("Object changes are diffed like struct changes"),
	         Changed->Changes.ContainsByPredicate(
	             [](const FStateMonitorChange& Change)
	             {
		             return Change.Path == TEXT("$/Health") && Change.Change == EStateMonitorChange::Modified;
	             }));
	TestTrue(TEXT("A changed object advances the sequence"), Changed->Sequence > Source->Sequence);

	const uint64 Sequence = Changed->Sequence;
	Model.SubmitObjectSnapshot(TEXT("Component"), FText::FromString(TEXT("Component")), Component);
	TestEqual(TEXT("An unchanged object keeps its sequence"), Model.FindSource(TEXT("Component"))->Sequence, Sequence);

	FStateMonitorTestNested Struct;
	TestTrue(TEXT("Struct snapshots still work"),
	         Model.SubmitSnapshot(TEXT("Struct"), FText(), FStateMonitorTestNested::StaticStruct(), &Struct));
	TestFalse(TEXT("Struct sources are not objects"), Model.FindSource(TEXT("Struct"))->bObject);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStateMonitorObjectFilterTest, "StateMonitor.Model.ObjectFilter",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStateMonitorObjectFilterTest::RunTest(const FString& Parameters)
{
	using namespace StateMonitorObjectTests;

	TestTrue(TEXT("A plugin class outside the engine directory is project code"),
	         FStateMonitorModel::IsProjectType(UStateMonitorTestComponent::StaticClass()));
	TestFalse(TEXT("UActorComponent is engine code"), FStateMonitorModel::IsProjectType(UActorComponent::StaticClass()));
	TestFalse(TEXT("AActor is engine code"), FStateMonitorModel::IsProjectType(AActor::StaticClass()));
	TestFalse(TEXT("UObject is engine code"), FStateMonitorModel::IsProjectType(UObject::StaticClass()));
	TestEqual(TEXT("An engine class has no project properties"),
	          FStateMonitorModel::CountIncludedProperties(UActorComponent::StaticClass(), false), 0);
	TestTrue(TEXT("Engine properties are counted when requested"),
	         FStateMonitorModel::CountIncludedProperties(UActorComponent::StaticClass(), true) > 0);
	TestEqual(TEXT("Project properties of the test component"),
	          FStateMonitorModel::CountIncludedProperties(UStateMonitorTestComponent::StaticClass(), false), 4);

	UStateMonitorTestComponent* Component = NewObject<UStateMonitorTestComponent>(GetTransientPackage());
	FStateMonitorModel Model;
	TestTrue(TEXT("Engine-inclusive capture succeeds"),
	         Model.SubmitObjectSnapshot(TEXT("All"), FText(), Component, true, true));
	const TSharedPtr<FStateMonitorSource> All = Model.FindSource(TEXT("All"));
	TestTrue(TEXT("Engine properties are included on request"), HasChild(All->Root, TEXT("ComponentTags")));
	TestTrue(TEXT("Project properties remain"), HasChild(All->Root, TEXT("Health")));
	TestFalse(TEXT("Delegates stay skipped"), HasChild(All->Root, TEXT("OnTestEvent")));
	TestTrue(TEXT("Engine-inclusive JSON is valid"), ParseObject(All->CurrentJson).IsValid());

	FStateMonitorSettings Settings;
	Settings.MaxNodesPerSnapshot = 3;
	FStateMonitorModel Bounded(Settings);
	TestTrue(TEXT("A bounded object capture succeeds"), Bounded.SubmitObjectSnapshot(TEXT("Bounded"), FText(), Component));
	const TSharedPtr<FStateMonitorSource> Limited = Bounded.FindSource(TEXT("Bounded"));
	TestTrue(TEXT("Object capture respects the node budget"), Limited->bTruncated);
	TestTrue(TEXT("Truncated object JSON stays valid"), ParseObject(Limited->CurrentJson).IsValid());
	return true;
}

#endif
