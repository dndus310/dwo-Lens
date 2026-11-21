// DwoCanvas - shared core for writing widget trees into Widget Blueprint designer trees

#pragma once

#include "CoreMinimal.h"

class UClass;
class UUserWidget;
class UWidget;
class UWidgetBlueprint;

/**
 * The "write a tree into a WBP" half of every generator: create / load the asset, clear or edit its
 * designer tree, set variables from BindWidget names, compile and save.
 * Used by the design importer and by C++ -> designer exporters.
 */
class DWOCANVASEDITOR_API FDwoCanvasWidgetWriter
{
public:
	/** Loads the WBP at PackagePath (/Game/.../WBP_Name) or creates it with ParentClass. */
	static UWidgetBlueprint* LoadOrCreate(const FString& PackagePath, UClass* ParentClass, bool& bOutCreated, FString& OutError);

	/** Closes any open editor for the asset (its preview would hold stale widgets). */
	static void CloseEditors(UWidgetBlueprint* Blueprint);

	/** True when the tree has more than the factory's empty root panel. */
	static bool HasDesignerContent(const UWidgetBlueprint* Blueprint);

	/** Removes every widget from the designer tree (names become free). */
	static void ClearTree(UWidgetBlueprint* Blueprint);

	/** First native class in the parent chain. */
	static UClass* GetNativeParent(const UWidgetBlueprint* Blueprint);

	/** BindWidget (+ BindWidgetOptional) property names of a class. */
	static TMap<FName, UClass*> GetBindWidgetProperties(const UClass* Class, bool bIncludeOptional);

	/** Sets bIsVariable = true for widgets whose name matches a BindWidget property. */
	static void MarkBoundWidgetsAsVariables(UWidgetBlueprint* Blueprint);

	/** Structural modify + compile + save. */
	static bool CompileAndSave(UWidgetBlueprint* Blueprint, FString& OutError);

	static bool Compile(UWidgetBlueprint* Blueprint, FString& OutError);
	static bool Save(UWidgetBlueprint* Blueprint, FString& OutError);

	/** Creates a widget inside the blueprint's tree (user widgets via CreateWidget, others via NewObject). */
	static UWidget* ConstructInTree(UWidgetBlueprint* Blueprint, UClass* WidgetClass, FName Name);

	/** Moves an object out of the tree and marks it garbage. */
	static void Discard(UObject* Object);
};
