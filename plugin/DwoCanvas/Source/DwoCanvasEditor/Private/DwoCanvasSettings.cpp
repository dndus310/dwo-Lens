// DwoCanvas - mapping table as data

#include "DwoCanvasSettings.h"

#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"

UDwoCanvasSettings::UDwoCanvasSettings()
{
	// Defaults; the project ini overrides them (Config/DefaultDwoCanvas.ini)
	RoleRules = {
		{ TEXT("btn_"), EDwoCanvasRole::Button, true },
		{ TEXT("list_"), EDwoCanvasRole::ListView, true },
		{ TEXT("tile_"), EDwoCanvasRole::TileView, true },
		{ TEXT("scroll_"), EDwoCanvasRole::ScrollBox, true },
		{ TEXT("input_"), EDwoCanvasRole::TextInput, true },
		{ TEXT("chk_"), EDwoCanvasRole::CheckBox, true },
		{ TEXT("slider_"), EDwoCanvasRole::Slider, true },
		{ TEXT("progress_"), EDwoCanvasRole::ProgressBar, true },
		{ TEXT("switch_"), EDwoCanvasRole::Switcher, true },
		{ TEXT("slot_"), EDwoCanvasRole::NamedSlot, true },
		{ TEXT("spacer_"), EDwoCanvasRole::Spacer, false },
		{ TEXT("_"), EDwoCanvasRole::Ignore, false },
	};

	DefaultParentClass = UUserWidget::StaticClass();
	DefaultFont = FSoftObjectPath(TEXT("/Engine/EngineFonts/Roboto.Roboto"));

	FDwoCanvasFontRule Roboto;
	Roboto.Family = TEXT("Roboto");
	Roboto.Font = DefaultFont;
	Roboto.LineHeightRatio = 1.17f; // (ascender + descender) / em of Roboto
	Fonts.Add(Roboto);
}

const FDwoCanvasRoleRule* UDwoCanvasSettings::FindRoleRule(const FString& LayerName) const
{
	// Longest matching prefix wins ("_" must not shadow "btn_")
	const FDwoCanvasRoleRule* Best = nullptr;
	for (const FDwoCanvasRoleRule& Rule : RoleRules)
	{
		if (!Rule.Prefix.IsEmpty() && LayerName.StartsWith(Rule.Prefix, ESearchCase::CaseSensitive)
			&& (!Best || Rule.Prefix.Len() > Best->Prefix.Len()))
		{
			Best = &Rule;
		}
	}
	return Best;
}

const FDwoCanvasFontRule* UDwoCanvasSettings::FindFontRule(const FString& Family) const
{
	return Fonts.FindByPredicate([&Family](const FDwoCanvasFontRule& Rule) { return Rule.Family.Equals(Family, ESearchCase::IgnoreCase); });
}

UClass* UDwoCanvasSettings::FindComponentOverride(const FString& ComponentName) const
{
	const FDwoCanvasComponentOverride* Override = ComponentOverrides.FindByPredicate(
		[&ComponentName](const FDwoCanvasComponentOverride& Entry) { return Entry.ComponentName == ComponentName; });
	return Override ? Override->WidgetClass.LoadSynchronous() : nullptr;
}
