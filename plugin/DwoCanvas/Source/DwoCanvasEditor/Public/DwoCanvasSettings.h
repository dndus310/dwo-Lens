// DwoCanvas - mapping table as data (Project Settings > Plugins > DwoCanvas, saved to Config/DefaultDwoCanvas.ini)

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UObject/SoftObjectPtr.h"
#include "DwoCanvasSettings.generated.h"

class UUserWidget;
class UWidget;

/** What a design node becomes beyond its plain visual mapping. Resolved from the layer name prefix or meta.role. */
UENUM()
enum class EDwoCanvasRole : uint8
{
	Auto,
	Button,
	ListView,
	TileView,
	ScrollBox,
	TextInput,
	CheckBox,
	Slider,
	ProgressBar,
	Switcher,
	NamedSlot,
	Spacer,
	Ignore
};

/** How auto-layout frames are built. */
UENUM()
enum class EDwoCanvasLayoutMode : uint8
{
	/** HorizontalBox / VerticalBox / WrapBox + Border / SizeBox / Spacer as needed (stock UMG only). */
	Native,
	/** One UDwoCanvasFrame per auto-layout frame (shallower tree, needs the DwoCanvas runtime module). */
	DwoCanvasFrame
};

UENUM()
enum class EDwoCanvasInstancePolicy : uint8
{
	/** Reference the component WBP; build the instance inline only when it overrides text / visibility / fills. */
	InlineWhenOverridden,
	AlwaysReference,
	AlwaysInline
};

USTRUCT()
struct FDwoCanvasRoleRule
{
	GENERATED_BODY()

	/** Case-sensitive layer name prefix, e.g. "btn_". */
	UPROPERTY(EditAnywhere, Category = "Role")
	FString Prefix;

	UPROPERTY(EditAnywhere, Category = "Role")
	EDwoCanvasRole Role = EDwoCanvasRole::Auto;

	/** Expose the widget as a Blueprint variable. */
	UPROPERTY(EditAnywhere, Category = "Role")
	bool bMakeVariable = true;
};

USTRUCT()
struct FDwoCanvasFontRule
{
	GENERATED_BODY()

	/** Font family name as used in the design tool. */
	UPROPERTY(EditAnywhere, Category = "Font")
	FString Family;

	/** UFont (composite font) asset. */
	UPROPERTY(EditAnywhere, Category = "Font", meta = (AllowedClasses = "/Script/Engine.Font"))
	FSoftObjectPath Font;

	/** Design-tool style name (e.g. "SemiBold") -> typeface name in the UFont (e.g. "Bold"). */
	UPROPERTY(EditAnywhere, Category = "Font")
	TMap<FString, FName> TypefaceByStyle;

	/** CSS weight (400, 600, 700...) -> typeface name, used when TypefaceByStyle has no entry. */
	UPROPERTY(EditAnywhere, Category = "Font")
	TMap<int32, FName> TypefaceByWeight;

	/** Font's natural line height / font size (used to convert line height). */
	UPROPERTY(EditAnywhere, Category = "Font", meta = (ClampMin = 0.5))
	float LineHeightRatio = 1.17f;
};

USTRUCT()
struct FDwoCanvasComponentOverride
{
	GENERATED_BODY()

	/** Component name in the design file. */
	UPROPERTY(EditAnywhere, Category = "Component")
	FString ComponentName;

	/** Existing widget class to use instead of generating a WBP (e.g. a CommonUI button). */
	UPROPERTY(EditAnywhere, Category = "Component", meta = (AllowAbstract = "false"))
	TSoftClassPtr<UWidget> WidgetClass;
};

UCLASS(Config = DwoCanvas, DefaultConfig, meta = (DisplayName = "DwoCanvas"))
class DWOCANVASEDITOR_API UDwoCanvasSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UDwoCanvasSettings();

	static const UDwoCanvasSettings& Get() { return *GetDefault<UDwoCanvasSettings>(); }

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	// ---- Naming rules (agreed with the designers) ----

	UPROPERTY(Config, EditAnywhere, Category = "Naming")
	TArray<FDwoCanvasRoleRule> RoleRules;

	/** "btn_Apply" becomes the widget "Apply" (so it can match a C++ BindWidget name). */
	UPROPERTY(Config, EditAnywhere, Category = "Naming")
	bool bStripRolePrefix = true;

	/** Layers whose name starts with this become Blueprint variables without a role (e.g. "$Title"). */
	UPROPERTY(Config, EditAnywhere, Category = "Naming")
	FString VariablePrefix = TEXT("$");

	// ---- Output ----

	UPROPERTY(Config, EditAnywhere, Category = "Output", meta = (ContentDir))
	FString WidgetFolder = TEXT("/Game/UI/Generated");

	UPROPERTY(Config, EditAnywhere, Category = "Output", meta = (ContentDir))
	FString ComponentFolder = TEXT("/Game/UI/Generated/Components");

	UPROPERTY(Config, EditAnywhere, Category = "Output", meta = (ContentDir))
	FString TextureFolder = TEXT("/Game/UI/Generated/Textures");

	/** Parent class when the document does not name one. */
	UPROPERTY(Config, EditAnywhere, Category = "Output")
	TSoftClassPtr<UUserWidget> DefaultParentClass;

	/** Inferred drafts (source.mode = draft) are written here, one new blueprint per import. */
	UPROPERTY(Config, EditAnywhere, Category = "Output", meta = (ContentDir))
	FString DraftFolder = TEXT("/Game/UI/Drafts");

	UPROPERTY(Config, EditAnywhere, Category = "Output")
	EDwoCanvasLayoutMode LayoutMode = EDwoCanvasLayoutMode::Native;

	UPROPERTY(Config, EditAnywhere, Category = "Output")
	EDwoCanvasInstancePolicy InstancePolicy = EDwoCanvasInstancePolicy::InlineWhenOverridden;

	/** Map components to existing widget classes (CommonUI buttons, shared widgets...). */
	UPROPERTY(Config, EditAnywhere, Category = "Output")
	TArray<FDwoCanvasComponentOverride> ComponentOverrides;

	// ---- Text ----

	UPROPERTY(Config, EditAnywhere, Category = "Text")
	TArray<FDwoCanvasFontRule> Fonts;

	/** Fallback when a family has no rule. */
	UPROPERTY(Config, EditAnywhere, Category = "Text", meta = (AllowedClasses = "/Script/Engine.Font"))
	FSoftObjectPath DefaultFont;

	/** Design px -> UMG font size. UMG sizes are points at 96 DPI, so 1 px = 0.75. */
	UPROPERTY(Config, EditAnywhere, Category = "Text", meta = (ClampMin = 0.1))
	float FontSizeScale = 0.75f;

	UPROPERTY(Config, EditAnywhere, Category = "Text")
	FString LocalizationNamespace = TEXT("DwoCanvas");

	// ---- Tokens ----

	/** Design variable / style name -> color. Wins over the exported value (theme fixes without re-export). */
	UPROPERTY(Config, EditAnywhere, Category = "Tokens")
	TMap<FString, FLinearColor> ColorTokenOverrides;

	/** Number / dimension token path (space.md, radius.lg, font.body.size) -> value. Wins over the document's tokens. */
	UPROPERTY(Config, EditAnywhere, Category = "Tokens")
	TMap<FString, float> NumberTokenOverrides;

	// ---- Validation ----

	/** Refuse documents that fail the dwocanvas/2 schema (otherwise import them with warnings). */
	UPROPERTY(Config, EditAnywhere, Category = "Validation")
	bool bStrictSchema = true;

	/** Inferred nodes below this confidence are listed as warnings to check. */
	UPROPERTY(Config, EditAnywhere, Category = "Validation", meta = (ClampMin = 0, ClampMax = 1))
	float LowConfidenceThreshold = 0.6f;

	// ---- Re-import ----

	/** Project-relative folder for the node-id <-> widget records (commit it with the WBPs). */
	UPROPERTY(Config, EditAnywhere, Category = "Reimport")
	FString RecordDirectory = TEXT("DwoCanvas/Records");

	/** Delete widgets whose design node is gone when nobody edited or references them. */
	UPROPERTY(Config, EditAnywhere, Category = "Reimport")
	bool bRemoveUntouchedOrphans = true;

	// ---- Verification ----

	/** Visual diff: per-pixel channel difference (0-255) that counts as "different". */
	UPROPERTY(Config, EditAnywhere, Category = "Verification", meta = (ClampMin = 0, ClampMax = 255))
	int32 VisualDiffPixelThreshold = 40;

	/** Visual diff: share of different pixels that is reported as a warning. */
	UPROPERTY(Config, EditAnywhere, Category = "Verification", meta = (ClampMin = 0, ClampMax = 1))
	float VisualDiffWarnRatio = 0.05f;

	/** Rule for a layer name, or null. */
	const FDwoCanvasRoleRule* FindRoleRule(const FString& LayerName) const;
	const FDwoCanvasFontRule* FindFontRule(const FString& Family) const;
	UClass* FindComponentOverride(const FString& ComponentName) const;
};
