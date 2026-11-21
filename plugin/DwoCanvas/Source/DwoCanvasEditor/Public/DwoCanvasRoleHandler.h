// DwoCanvas - role handlers: let a project build its own widgets for a role ("button", "x-inventorySlot", ...)

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UPanelWidget;
class UWidget;
struct FDwoCanvasBuildContextData;

/**
 * Passed to a role handler while one node is built. Everything created through it takes part in
 * re-import (stable names, kept UE edits) exactly like the built-in widgets.
 */
class DWOCANVASEDITOR_API FDwoCanvasBuildContext
{
public:
	explicit FDwoCanvasBuildContext(FDwoCanvasBuildContextData& InData) : Data(InData) {}

	/** The node JSON (dwocanvas/2). */
	TSharedPtr<FJsonObject> GetNode() const;
	FString GetNodeId() const;
	FString GetRole() const;
	/** Sanitized widget name (role prefix stripped / widgetName). */
	FString GetWidgetName() const;
	/** Design box size in px. */
	FVector2D GetSize() const;
	/** Document name (for messages). */
	FString GetDocumentName() const;

	/**
	 * Creates the widget for this node (KeySuffix empty) or an extra part (KeySuffix like "bg").
	 * bOutKeptEdited = the widget was edited in UE since the last import: reuse it and do not apply design values.
	 */
	UWidget* MakeWidget(UClass* WidgetClass, const FString& KeySuffix, bool& bOutKeptEdited);

	template <typename T>
	T* MakeWidget(const FString& KeySuffix, bool& bOutKeptEdited)
	{
		return Cast<T>(MakeWidget(T::StaticClass(), KeySuffix, bOutKeptEdited));
	}

	/** Builds the node's children into Panel with the slot rules of the panel type (canvas anchors, box padding / gap...). */
	void BuildChildren(UPanelWidget* Panel);

	/** Design color of a paint / stroke / text JSON object (tokens resolved). */
	FLinearColor ResolveColor(const TSharedPtr<FJsonObject>& Paint) const;

	void Info(const FString& Message) const;
	void Warn(const FString& Message) const;

private:
	FDwoCanvasBuildContextData& Data;
};

class DWOCANVASEDITOR_API IDwoCanvasRoleHandler
{
public:
	virtual ~IDwoCanvasRoleHandler() = default;

	/** Role this handler builds: a built-in role ("button", "list", ...) to replace it, or "x-<name>". */
	virtual FString GetRole() const = 0;

	/**
	 * Build the node. Return the outermost widget (sizing / visibility / opacity are applied by the importer),
	 * or nullptr to fall back to the built-in mapping.
	 */
	virtual UWidget* Build(FDwoCanvasBuildContext& Context) = 0;
};

/** Registry. Register from your editor module's StartupModule, unregister in ShutdownModule. */
class DWOCANVASEDITOR_API FDwoCanvasRoleHandlers
{
public:
	static void Register(const TSharedRef<IDwoCanvasRoleHandler>& Handler);
	static void Unregister(const FString& Role);
	static TSharedPtr<IDwoCanvasRoleHandler> Find(const FString& Role);
	static TArray<FString> GetRegisteredRoles();
};
