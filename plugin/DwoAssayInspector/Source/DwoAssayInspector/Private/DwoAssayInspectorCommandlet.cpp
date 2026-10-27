#include "DwoAssayInspectorCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Variable.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ProjectDescriptor.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogDwoAssay, Log, All);

namespace DwoAssayInspector
{
	static const TCHAR* SchemaVersion = TEXT("assay.raw/0.1");

	using FJsonRef = TSharedRef<FJsonObject>;
	using FJsonValues = TArray<TSharedPtr<FJsonValue>>;

	static void PushString(FJsonValues& Array, const FString& Value)
	{
		Array.Add(MakeShared<FJsonValueString>(Value));
	}

	static void PushObject(FJsonValues& Array, const FJsonRef& Value)
	{
		Array.Add(MakeShared<FJsonValueObject>(Value));
	}

	static bool WriteJson(const FString& Path, const FJsonRef& Root)
	{
		FString Text;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		if (!FJsonSerializer::Serialize(Root, Writer))
		{
			return false;
		}
		return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	static FString PathOf(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString();
	}

	/** Modules that belong to the project itself (project modules and project plugins, excluding this plugin). */
	static TSet<FString> CollectProjectModules(TArray<FString>& OutContentRoots)
	{
		TSet<FString> Modules;
		if (const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject())
		{
			for (const FModuleDescriptor& Module : Project->Modules)
			{
				Modules.Add(Module.Name.ToString());
			}
		}
		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
		{
			if (Plugin->GetLoadedFrom() != EPluginLoadedFrom::Project || Plugin->GetName() == TEXT("DwoAssayInspector"))
			{
				continue;
			}
			for (const FModuleDescriptor& Module : Plugin->GetDescriptor().Modules)
			{
				Modules.Add(Module.Name.ToString());
			}
			if (Plugin->CanContainContent())
			{
				FString Root = Plugin->GetMountedAssetPath();
				Root.RemoveFromEnd(TEXT("/"));
				OutContentRoots.AddUnique(Root);
			}
		}
		return Modules;
	}

	static FJsonValues FunctionFlags(const UFunction* Function)
	{
		struct FFlagName { EFunctionFlags Flag; const TCHAR* Name; };
		static const FFlagName Names[] = {
			{ FUNC_Native, TEXT("Native") },
			{ FUNC_Event, TEXT("Event") },
			{ FUNC_BlueprintCallable, TEXT("BlueprintCallable") },
			{ FUNC_BlueprintEvent, TEXT("BlueprintEvent") },
			{ FUNC_BlueprintPure, TEXT("BlueprintPure") },
			{ FUNC_Net, TEXT("Net") },
			{ FUNC_NetReliable, TEXT("NetReliable") },
			{ FUNC_NetServer, TEXT("NetServer") },
			{ FUNC_NetClient, TEXT("NetClient") },
			{ FUNC_NetMulticast, TEXT("NetMulticast") },
			{ FUNC_NetValidate, TEXT("NetValidate") },
			{ FUNC_Static, TEXT("Static") },
			{ FUNC_Delegate, TEXT("Delegate") },
			{ FUNC_MulticastDelegate, TEXT("MulticastDelegate") },
		};
		FJsonValues Out;
		for (const FFlagName& Entry : Names)
		{
			if (Function->HasAnyFunctionFlags(Entry.Flag))
			{
				PushString(Out, Entry.Name);
			}
		}
		return Out;
	}

	/** Records the type references a property makes (hard, soft, weak, interface, struct, delegate). */
	static void CollectPropertyRefs(const FProperty* Property, FJsonValues& OutRefs)
	{
		auto AddRef = [&OutRefs](const TCHAR* Kind, const UObject* Target)
		{
			if (!Target)
			{
				return;
			}
			FJsonRef Ref = MakeShared<FJsonObject>();
			Ref->SetStringField(TEXT("kind"), Kind);
			Ref->SetStringField(TEXT("target"), Target->GetPathName());
			PushObject(OutRefs, Ref);
		};

		if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
		{
			CollectPropertyRefs(Array->Inner, OutRefs);
		}
		else if (const FSetProperty* Set = CastField<FSetProperty>(Property))
		{
			CollectPropertyRefs(Set->ElementProp, OutRefs);
		}
		else if (const FMapProperty* Map = CastField<FMapProperty>(Property))
		{
			CollectPropertyRefs(Map->KeyProp, OutRefs);
			CollectPropertyRefs(Map->ValueProp, OutRefs);
		}
		else if (const FSoftClassProperty* SoftClass = CastField<FSoftClassProperty>(Property))
		{
			AddRef(TEXT("soft"), SoftClass->MetaClass);
		}
		else if (const FSoftObjectProperty* SoftObject = CastField<FSoftObjectProperty>(Property))
		{
			AddRef(TEXT("soft"), SoftObject->PropertyClass);
		}
		else if (const FClassProperty* ClassProp = CastField<FClassProperty>(Property))
		{
			AddRef(TEXT("hard"), ClassProp->MetaClass);
		}
		else if (const FWeakObjectProperty* Weak = CastField<FWeakObjectProperty>(Property))
		{
			AddRef(TEXT("weak"), Weak->PropertyClass);
		}
		else if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Property))
		{
			AddRef(TEXT("hard"), Object->PropertyClass);
		}
		else if (const FInterfaceProperty* Interface = CastField<FInterfaceProperty>(Property))
		{
			AddRef(TEXT("interface"), Interface->InterfaceClass);
		}
		else if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
		{
			AddRef(TEXT("struct"), Struct->Struct);
		}
		else if (const FDelegateProperty* Delegate = CastField<FDelegateProperty>(Property))
		{
			AddRef(TEXT("delegate"), Delegate->SignatureFunction);
		}
		else if (const FMulticastDelegateProperty* Multicast = CastField<FMulticastDelegateProperty>(Property))
		{
			AddRef(TEXT("delegate"), Multicast->SignatureFunction);
		}
	}

	static FJsonRef DescribeClass(const UClass* Class, const FString& Origin)
	{
		FJsonRef Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("path"), Class->GetPathName());
		Out->SetStringField(TEXT("name"), Class->GetName());
		Out->SetStringField(TEXT("package"), Class->GetOutermost()->GetName());
		Out->SetStringField(TEXT("origin"), Origin);
		Out->SetStringField(TEXT("super"), PathOf(Class->GetSuperClass()));
		Out->SetBoolField(TEXT("abstract"), Class->HasAnyClassFlags(CLASS_Abstract));
		Out->SetBoolField(TEXT("interface"), Class->HasAnyClassFlags(CLASS_Interface));
#if WITH_EDITORONLY_DATA
		Out->SetBoolField(TEXT("blueprintable"), Class->GetBoolMetaDataHierarchical(TEXT("IsBlueprintBase")));
#endif

		FJsonValues Interfaces;
		for (const FImplementedInterface& Implemented : Class->Interfaces)
		{
			if (Implemented.Class)
			{
				PushString(Interfaces, Implemented.Class->GetPathName());
			}
		}
		Out->SetArrayField(TEXT("interfaces"), Interfaces);

		FJsonValues Functions;
		for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			const UFunction* Function = *It;
			FJsonRef Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Function->GetName());
			Entry->SetArrayField(TEXT("flags"), FunctionFlags(Function));
			Entry->SetStringField(TEXT("overrides"), PathOf(Function->GetSuperFunction()));
			FJsonValues Params;
			for (TFieldIterator<FProperty> ParamIt(Function); ParamIt; ++ParamIt)
			{
				CollectPropertyRefs(*ParamIt, Params);
			}
			Entry->SetArrayField(TEXT("paramRefs"), Params);
			PushObject(Functions, Entry);
		}
		Out->SetArrayField(TEXT("functions"), Functions);

		FJsonValues Properties;
		for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			const FProperty* Property = *It;
			FJsonRef Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Property->GetName());
			Entry->SetStringField(TEXT("cppType"), Property->GetCPPType());
			Entry->SetBoolField(TEXT("replicated"), Property->HasAnyPropertyFlags(CPF_Net));
			Entry->SetBoolField(TEXT("blueprintVisible"), Property->HasAnyPropertyFlags(CPF_BlueprintVisible));
			FJsonValues Refs;
			CollectPropertyRefs(Property, Refs);
			Entry->SetArrayField(TEXT("refs"), Refs);
			PushObject(Properties, Entry);
		}
		Out->SetArrayField(TEXT("properties"), Properties);
		return Out;
	}

	static const TCHAR* GraphKind(const UBlueprint* Blueprint, const UEdGraph* Graph)
	{
		if (Blueprint->UbergraphPages.Contains(Graph)) { return TEXT("event"); }
		if (Blueprint->FunctionGraphs.Contains(Graph)) { return TEXT("function"); }
		if (Blueprint->MacroGraphs.Contains(Graph)) { return TEXT("macro"); }
		if (Blueprint->DelegateSignatureGraphs.Contains(Graph)) { return TEXT("delegate"); }
		return TEXT("sub");
	}

	/** Semantic summary of the node kinds the analyzer cares about. */
	static void DescribeNodeSemantics(const UEdGraphNode* Node, const FJsonRef& Out)
	{
		if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
		{
			Out->SetStringField(TEXT("role"), TEXT("call"));
			if (const UFunction* Target = Call->GetTargetFunction())
			{
				Out->SetStringField(TEXT("member"), Target->GetName());
				Out->SetStringField(TEXT("memberOwner"), PathOf(Target->GetOwnerClass()));
				Out->SetArrayField(TEXT("memberFlags"), FunctionFlags(Target));
			}
			else
			{
				Out->SetStringField(TEXT("member"), Call->FunctionReference.GetMemberName().ToString());
			}
		}
		else if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
		{
			Out->SetStringField(TEXT("role"), TEXT("event"));
			Out->SetStringField(TEXT("member"), Event->GetFunctionName().ToString());
			Out->SetStringField(TEXT("memberOwner"), PathOf(Event->EventReference.GetMemberParentClass()));
			Out->SetBoolField(TEXT("override"), Event->bOverrideFunction);
		}
		else if (const UK2Node_BaseMCDelegate* Delegate = Cast<UK2Node_BaseMCDelegate>(Node))
		{
			Out->SetStringField(TEXT("role"), TEXT("delegate"));
			Out->SetStringField(TEXT("member"), Delegate->GetPropertyName().ToString());
			Out->SetStringField(TEXT("memberOwner"), PathOf(Delegate->DelegateReference.GetMemberParentClass(Delegate->GetBlueprintClassFromNode())));
		}
		else if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
		{
			Out->SetStringField(TEXT("role"), TEXT("variable"));
			Out->SetStringField(TEXT("member"), Variable->GetVarNameString());
			Out->SetStringField(TEXT("memberOwner"), PathOf(Variable->GetVariableSourceClass()));
		}
		else if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
		{
			Out->SetStringField(TEXT("role"), TEXT("macro"));
			Out->SetStringField(TEXT("member"), PathOf(Macro->GetMacroGraph()));
		}
		else if (const UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
		{
			Out->SetStringField(TEXT("role"), TEXT("cast"));
			Out->SetStringField(TEXT("member"), PathOf(CastNode->TargetType.Get()));
		}
		else
		{
			Out->SetStringField(TEXT("role"), TEXT("other"));
		}
	}

	static FJsonRef DescribeGraph(const UBlueprint* Blueprint, const UEdGraph* Graph)
	{
		FJsonRef Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("name"), Graph->GetName());
		Out->SetStringField(TEXT("path"), Graph->GetPathName());
		Out->SetStringField(TEXT("kind"), GraphKind(Blueprint, Graph));

		FJsonValues Nodes;
		FJsonValues Links;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			const FString NodeId = Node->NodeGuid.ToString(EGuidFormats::Digits);
			FJsonRef Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("id"), NodeId);
			Entry->SetStringField(TEXT("class"), Node->GetClass()->GetName());
			Entry->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			DescribeNodeSemantics(Node, Entry);
			PushObject(Nodes, Entry);

			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output)
				{
					continue;
				}
				const bool bExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (!Linked || !Linked->GetOwningNodeUnchecked())
					{
						continue;
					}
					FJsonRef Link = MakeShared<FJsonObject>();
					Link->SetStringField(TEXT("from"), NodeId);
					Link->SetStringField(TEXT("fromPin"), Pin->PinName.ToString());
					Link->SetStringField(TEXT("to"), Linked->GetOwningNodeUnchecked()->NodeGuid.ToString(EGuidFormats::Digits));
					Link->SetStringField(TEXT("toPin"), Linked->PinName.ToString());
					Link->SetStringField(TEXT("kind"), bExec ? TEXT("exec") : TEXT("data"));
					PushObject(Links, Link);
				}
			}
		}
		Out->SetArrayField(TEXT("nodes"), Nodes);
		Out->SetArrayField(TEXT("links"), Links);
		return Out;
	}

	static FJsonRef DescribeBlueprint(const UBlueprint* Blueprint, const FAssetData& Asset)
	{
		FJsonRef Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("asset"), Asset.GetObjectPathString());
		Out->SetStringField(TEXT("package"), Asset.PackageName.ToString());
		Out->SetStringField(TEXT("assetClass"), Blueprint->GetClass()->GetName());
		Out->SetStringField(TEXT("blueprintType"), StaticEnum<EBlueprintType>()->GetNameStringByValue(static_cast<int64>(Blueprint->BlueprintType)));
		Out->SetStringField(TEXT("parent"), PathOf(Blueprint->ParentClass));
		Out->SetStringField(TEXT("generatedClass"), PathOf(Blueprint->GeneratedClass));

		FJsonValues Interfaces;
		for (const FBPInterfaceDescription& Implemented : Blueprint->ImplementedInterfaces)
		{
			if (Implemented.Interface)
			{
				PushString(Interfaces, Implemented.Interface->GetPathName());
			}
		}
		Out->SetArrayField(TEXT("interfaces"), Interfaces);

		FJsonValues Variables;
		for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
		{
			FJsonRef Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Variable.VarName.ToString());
			Entry->SetStringField(TEXT("category"), Variable.VarType.PinCategory.ToString());
			Entry->SetStringField(TEXT("subType"), PathOf(Variable.VarType.PinSubCategoryObject.Get()));
			Entry->SetBoolField(TEXT("replicated"), (Variable.PropertyFlags & CPF_Net) != 0);
			PushObject(Variables, Entry);
		}
		Out->SetArrayField(TEXT("variables"), Variables);

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		FJsonValues GraphValues;
		for (const UEdGraph* Graph : Graphs)
		{
			if (Graph)
			{
				PushObject(GraphValues, DescribeGraph(Blueprint, Graph));
			}
		}
		Out->SetArrayField(TEXT("graphs"), GraphValues);
		return Out;
	}

	static bool IsUnderRoots(const FString& PackageName, const TArray<FString>& Roots)
	{
		for (const FString& Root : Roots)
		{
			if (PackageName == Root || PackageName.StartsWith(Root + TEXT("/")))
			{
				return true;
			}
		}
		return false;
	}

	static FJsonValues DependenciesOf(IAssetRegistry& Registry, FName Package, UE::AssetRegistry::EDependencyQuery Query)
	{
		TArray<FName> Dependencies;
		Registry.GetDependencies(Package, Dependencies, UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::FDependencyQuery(Query));
		FJsonValues Out;
		for (const FName& Dependency : Dependencies)
		{
			PushString(Out, Dependency.ToString());
		}
		return Out;
	}
}

UDwoAssayInspectorCommandlet::UDwoAssayInspectorCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	ShowErrorCount = true;
}

int32 UDwoAssayInspectorCommandlet::Main(const FString& Params)
{
	using namespace DwoAssayInspector;

	FString OutDir;
	if (!FParse::Value(*Params, TEXT("AssayOut="), OutDir) || OutDir.IsEmpty())
	{
		UE_LOG(LogDwoAssay, Error, TEXT("Missing -AssayOut=<dir>"));
		return 1;
	}
	OutDir = FPaths::ConvertRelativePathToFull(OutDir);
	IFileManager::Get().MakeDirectory(*OutDir, true);

	TArray<FString> ContentRoots = { TEXT("/Game") };
	const TSet<FString> ProjectModules = CollectProjectModules(ContentRoots);
	FString RootsOverride;
	if (FParse::Value(*Params, TEXT("AssayContentRoots="), RootsOverride))
	{
		ContentRoots.Reset();
		RootsOverride.ParseIntoArray(ContentRoots, TEXT(","), true);
	}

	// ---- Native reflection of project modules ----
	FJsonValues NativeClasses;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		const UClass* Class = *It;
		const FString Package = Class->GetOutermost()->GetName();
		FString Module;
		if (!Package.Split(TEXT("/Script/"), nullptr, &Module) || !ProjectModules.Contains(Module))
		{
			continue;
		}
		if (Class->HasAnyClassFlags(CLASS_NewerVersionExists | CLASS_Deprecated))
		{
			continue;
		}
		PushObject(NativeClasses, DescribeClass(Class, TEXT("native")));
	}

	// ---- Assets and Blueprints ----
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.SearchAllAssets(true);

	FJsonValues Assets;
	for (const FString& Root : ContentRoots)
	{
		TArray<FAssetData> Found;
		Registry.GetAssetsByPath(FName(*Root), Found, true);
		for (const FAssetData& Asset : Found)
		{
			FJsonRef Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("package"), Asset.PackageName.ToString());
			Entry->SetStringField(TEXT("asset"), Asset.GetObjectPathString());
			Entry->SetStringField(TEXT("class"), Asset.AssetClassPath.ToString());
			Entry->SetArrayField(TEXT("hard"), DependenciesOf(Registry, Asset.PackageName, UE::AssetRegistry::EDependencyQuery::Hard));
			Entry->SetArrayField(TEXT("soft"), DependenciesOf(Registry, Asset.PackageName, UE::AssetRegistry::EDependencyQuery::Soft));
			PushObject(Assets, Entry);
		}
	}

	TArray<FAssetData> BlueprintAssets;
	Registry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), BlueprintAssets, true);

	FJsonValues Blueprints;
	FJsonValues BlueprintClasses;
	int32 Failed = 0;
	for (const FAssetData& Asset : BlueprintAssets)
	{
		if (!IsUnderRoots(Asset.PackageName.ToString(), ContentRoots))
		{
			continue;
		}
		const UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
		if (!Blueprint)
		{
			UE_LOG(LogDwoAssay, Warning, TEXT("Could not load Blueprint %s"), *Asset.GetObjectPathString());
			++Failed;
			continue;
		}
		PushObject(Blueprints, DescribeBlueprint(Blueprint, Asset));
		if (Blueprint->GeneratedClass)
		{
			PushObject(BlueprintClasses, DescribeClass(Blueprint->GeneratedClass, TEXT("blueprint")));
		}
	}

	// ---- Write ----
	FJsonRef Meta = MakeShared<FJsonObject>();
	Meta->SetStringField(TEXT("schema"), SchemaVersion);
	Meta->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	Meta->SetStringField(TEXT("project"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
	Meta->SetStringField(TEXT("extractedAtUtc"), FDateTime::UtcNow().ToIso8601());
	FJsonValues ModuleValues;
	for (const FString& Module : ProjectModules)
	{
		PushString(ModuleValues, Module);
	}
	Meta->SetArrayField(TEXT("projectModules"), ModuleValues);
	FJsonValues RootValues;
	for (const FString& Root : ContentRoots)
	{
		PushString(RootValues, Root);
	}
	Meta->SetArrayField(TEXT("contentRoots"), RootValues);
	Meta->SetNumberField(TEXT("blueprintLoadFailures"), Failed);

	FJsonRef Reflection = MakeShared<FJsonObject>();
	Reflection->SetArrayField(TEXT("native"), NativeClasses);
	Reflection->SetArrayField(TEXT("blueprint"), BlueprintClasses);

	FJsonRef BlueprintRoot = MakeShared<FJsonObject>();
	BlueprintRoot->SetArrayField(TEXT("blueprints"), Blueprints);

	FJsonRef AssetRoot = MakeShared<FJsonObject>();
	AssetRoot->SetArrayField(TEXT("assets"), Assets);

	const bool bWritten =
		WriteJson(OutDir / TEXT("meta.json"), Meta) &&
		WriteJson(OutDir / TEXT("reflection.json"), Reflection) &&
		WriteJson(OutDir / TEXT("blueprints.json"), BlueprintRoot) &&
		WriteJson(OutDir / TEXT("assets.json"), AssetRoot);

	UE_LOG(LogDwoAssay, Display, TEXT("dwo-Assay: %d native classes, %d blueprints, %d assets -> %s"),
		NativeClasses.Num(), Blueprints.Num(), Assets.Num(), *OutDir);

	return bWritten ? 0 : 1;
}
