// DwoCanvas - role handlers

#include "DwoCanvasRoleHandler.h"

#include "DwoCanvasBuildContextData.h"

#include "Dom/JsonObject.h"

namespace
{
	TMap<FString, TSharedRef<IDwoCanvasRoleHandler>>& Registry()
	{
		static TMap<FString, TSharedRef<IDwoCanvasRoleHandler>> Handlers;
		return Handlers;
	}
}

void FDwoCanvasRoleHandlers::Register(const TSharedRef<IDwoCanvasRoleHandler>& Handler)
{
	Registry().Add(Handler->GetRole(), Handler);
}

void FDwoCanvasRoleHandlers::Unregister(const FString& Role)
{
	Registry().Remove(Role);
}

TSharedPtr<IDwoCanvasRoleHandler> FDwoCanvasRoleHandlers::Find(const FString& Role)
{
	if (const TSharedRef<IDwoCanvasRoleHandler>* Found = Registry().Find(Role))
	{
		return *Found;
	}
	return nullptr;
}

TArray<FString> FDwoCanvasRoleHandlers::GetRegisteredRoles()
{
	TArray<FString> Roles;
	Registry().GetKeys(Roles);
	return Roles;
}

TSharedPtr<FJsonObject> FDwoCanvasBuildContext::GetNode() const { return Data.Node; }
FString FDwoCanvasBuildContext::GetNodeId() const { return Data.NodeId; }
FString FDwoCanvasBuildContext::GetRole() const { return Data.Role; }
FString FDwoCanvasBuildContext::GetWidgetName() const { return Data.WidgetName; }
FVector2D FDwoCanvasBuildContext::GetSize() const { return Data.Size; }
FString FDwoCanvasBuildContext::GetDocumentName() const { return Data.DocumentName; }

UWidget* FDwoCanvasBuildContext::MakeWidget(UClass* WidgetClass, const FString& KeySuffix, bool& bOutKeptEdited)
{
	bOutKeptEdited = false;
	return Data.Make ? Data.Make(WidgetClass, KeySuffix, bOutKeptEdited) : nullptr;
}

void FDwoCanvasBuildContext::BuildChildren(UPanelWidget* Panel)
{
	if (Data.BuildChildren && Panel)
	{
		Data.BuildChildren(Panel);
	}
}

FLinearColor FDwoCanvasBuildContext::ResolveColor(const TSharedPtr<FJsonObject>& Paint) const
{
	return Data.Color ? Data.Color(Paint) : FLinearColor::White;
}

void FDwoCanvasBuildContext::Info(const FString& Message) const
{
	if (Data.Log) { Data.Log(false, Message); }
}

void FDwoCanvasBuildContext::Warn(const FString& Message) const
{
	if (Data.Log) { Data.Log(true, Message); }
}
