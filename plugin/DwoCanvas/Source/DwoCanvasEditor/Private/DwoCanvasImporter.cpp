// DwoCanvas - imports a design export (designbridge/1 JSON) into Widget Blueprints

#include "DwoCanvasImporter.h"

#include "DwoCanvasBuildContextData.h"
#include "DwoCanvasRoleHandler.h"
#include "DwoCanvasSchema.h"

#include "DwoCanvasSettings.h"
#include "DwoCanvasVisualDiff.h"
#include "DwoCanvasWidgetWriter.h"
#include "DwoCanvasFrame.h"
#include "DwoCanvasShape.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/IUserObjectListEntry.h"
#include "Components/BackgroundBlur.h"
#include "Components/BackgroundBlurSlot.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/CheckBox.h"
#include "Components/EditableTextBox.h"
#include "Components/GridPanel.h"
#include "Components/GridSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/ListView.h"
#include "Components/NamedSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ProgressBar.h"
#include "Components/ScaleBox.h"
#include "Components/ScaleBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/SizeBoxSlot.h"
#include "Components/Slider.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/TileView.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "Components/WidgetSwitcherSlot.h"
#include "Components/WrapBox.h"
#include "Components/WrapBoxSlot.h"
#include "Dom/JsonObject.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "FileHelpers.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "ImageCore.h"
#include "Rendering/SlateRenderer.h"
#include "ImageUtils.h"
#include "Internationalization/StringTableRegistry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/GCObject.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "WidgetBlueprint.h"

namespace DwoCanvasImport
{
	using FJson = TSharedPtr<FJsonObject>;
	using FJsonArray = TArray<TSharedPtr<FJsonValue>>;

	FString JStr(const FJson& Object, const TCHAR* Key, const FString& Default = FString())
	{
		FString Value;
		return Object.IsValid() && Object->TryGetStringField(Key, Value) ? Value : Default;
	}

	double JNum(const FJson& Object, const TCHAR* Key, double Default = 0.0)
	{
		double Value = Default;
		return Object.IsValid() && Object->TryGetNumberField(Key, Value) ? Value : Default;
	}

	bool JBool(const FJson& Object, const TCHAR* Key, bool Default = false)
	{
		bool Value = Default;
		return Object.IsValid() && Object->TryGetBoolField(Key, Value) ? Value : Default;
	}

	FJson JObj(const FJson& Object, const TCHAR* Key)
	{
		const FJson* Value = nullptr;
		return Object.IsValid() && Object->TryGetObjectField(Key, Value) ? *Value : FJson();
	}

	const FJsonArray* JArr(const FJson& Object, const TCHAR* Key)
	{
		const FJsonArray* Value = nullptr;
		return Object.IsValid() && Object->TryGetArrayField(Key, Value) ? Value : nullptr;
	}

	TArray<double> JNums(const FJson& Object, const TCHAR* Key)
	{
		TArray<double> Result;
		if (const FJsonArray* Array = JArr(Object, Key))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				Result.Add(Value->AsNumber());
			}
		}
		return Result;
	}

	FLinearColor JColor(const FJson& Object, const TCHAR* Key, const FLinearColor& Default = FLinearColor::Transparent)
	{
		const TArray<double> V = JNums(Object, Key);
		if (V.Num() < 3)
		{
			return Default;
		}
		// Design colors are sRGB 0..1
		const FColor Srgb(
			(uint8)FMath::RoundToInt(FMath::Clamp(V[0], 0.0, 1.0) * 255.0),
			(uint8)FMath::RoundToInt(FMath::Clamp(V[1], 0.0, 1.0) * 255.0),
			(uint8)FMath::RoundToInt(FMath::Clamp(V[2], 0.0, 1.0) * 255.0));
		FLinearColor Linear = FLinearColor::FromSRGBColor(Srgb);
		Linear.A = V.Num() > 3 ? (float)V[3] : 1.f;
		return Linear;
	}

	FMargin JMargin(const FJson& Object, const TCHAR* Key)
	{
		const TArray<double> V = JNums(Object, Key);
		return V.Num() >= 4 ? FMargin(V[0], V[1], V[2], V[3]) : FMargin(0.f);
	}

	FString SanitizeName(const FString& In)
	{
		FString Out;
		for (const TCHAR Char : In)
		{
			Out.AppendChar(FChar::IsAlnum(Char) || Char == TEXT('_') ? Char : TEXT('_'));
		}
		while (Out.ReplaceInline(TEXT("__"), TEXT("_")) > 0) {}
		Out.TrimCharInline(TEXT('_'), nullptr);
		if (Out.IsEmpty())
		{
			Out = TEXT("Widget");
		}
		if (FChar::IsDigit(Out[0]))
		{
			Out = TEXT("W_") + Out;
		}
		return Out;
	}

	EDwoCanvasRole RoleFromName(const FString& Name)
	{
		static const TMap<FString, EDwoCanvasRole> Map = {
			{ TEXT("auto"), EDwoCanvasRole::Auto }, { TEXT("button"), EDwoCanvasRole::Button }, { TEXT("list"), EDwoCanvasRole::ListView },
			{ TEXT("tile"), EDwoCanvasRole::TileView }, { TEXT("scroll"), EDwoCanvasRole::ScrollBox }, { TEXT("textInput"), EDwoCanvasRole::TextInput },
			{ TEXT("checkBox"), EDwoCanvasRole::CheckBox }, { TEXT("slider"), EDwoCanvasRole::Slider }, { TEXT("progressBar"), EDwoCanvasRole::ProgressBar },
			{ TEXT("switcher"), EDwoCanvasRole::Switcher }, { TEXT("namedSlot"), EDwoCanvasRole::NamedSlot }, { TEXT("spacer"), EDwoCanvasRole::Spacer },
			{ TEXT("ignore"), EDwoCanvasRole::Ignore },
		};
		const EDwoCanvasRole* Found = Map.Find(Name);
		return Found ? *Found : EDwoCanvasRole::Auto;
	}

	FString RoleToName(EDwoCanvasRole Role)
	{
		switch (Role)
		{
		case EDwoCanvasRole::Button: return TEXT("button");
		case EDwoCanvasRole::ListView: return TEXT("list");
		case EDwoCanvasRole::TileView: return TEXT("tile");
		case EDwoCanvasRole::ScrollBox: return TEXT("scroll");
		case EDwoCanvasRole::TextInput: return TEXT("textInput");
		case EDwoCanvasRole::CheckBox: return TEXT("checkBox");
		case EDwoCanvasRole::Slider: return TEXT("slider");
		case EDwoCanvasRole::ProgressBar: return TEXT("progressBar");
		case EDwoCanvasRole::Switcher: return TEXT("switcher");
		case EDwoCanvasRole::NamedSlot: return TEXT("namedSlot");
		case EDwoCanvasRole::Spacer: return TEXT("spacer");
		case EDwoCanvasRole::Ignore: return TEXT("ignore");
		default: return FString();
		}
	}

	/** Parsed node fields used by every builder. */
	struct FNode
	{
		FJson J;
		FString Id;
		FString Name;
		FString Type;
		EDwoCanvasRole Role = EDwoCanvasRole::Auto;
		/** Role as written in the document ("button", "x-badge"...). Empty = none. */
		FString RoleName;
		bool bMakeVariable = false;
		FString WidgetName;
		/** Inferred documents: 0..1, -1 = not inferred. */
		float Confidence = -1.f;
		TArray<FString> InferredFields;

		bool bVisible = true;
		float Opacity = 1.f;
		float Rotation = 0.f;
		bool bClip = false;

		float X = 0.f, Y = 0.f, W = 0.f, H = 0.f;

		FString LayoutMode = TEXT("none");
		float Gap = 0.f;
		float CrossGap = 0.f;
		FMargin Padding;
		FString MainAlign = TEXT("start");
		FString CrossAlign = TEXT("start");
		int32 Columns = 0;

		FString SizingH = TEXT("fixed");
		FString SizingV = TEXT("fixed");
		float MinW = 0.f, MaxW = 0.f, MinH = 0.f, MaxH = 0.f;

		bool bAbsolute = false;
		FString ConstraintH = TEXT("min");
		FString ConstraintV = TEXT("min");

		bool IsFixedH() const { return SizingH == TEXT("fixed"); }
		bool IsFixedV() const { return SizingV == TEXT("fixed"); }
		bool IsFlowLayout() const { return LayoutMode == TEXT("horizontal") || LayoutMode == TEXT("vertical") || LayoutMode == TEXT("wrap") || LayoutMode == TEXT("grid"); }
	};

	struct FSlotSnapshot
	{
		UClass* SlotClass = nullptr;
		TMap<FName, FString> Values;
	};

	FSlotSnapshot SnapshotSlot(const UPanelSlot* Slot)
	{
		FSlotSnapshot Snapshot;
		if (!Slot)
		{
			return Snapshot;
		}
		Snapshot.SlotClass = Slot->GetClass();
		for (TFieldIterator<FProperty> It(Slot->GetClass()); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Edit) && It->GetFName() != TEXT("Parent") && It->GetFName() != TEXT("Content"))
			{
				FString Value;
				It->ExportText_InContainer(0, Value, Slot, nullptr, const_cast<UPanelSlot*>(Slot), PPF_None);
				Snapshot.Values.Add(It->GetFName(), Value);
			}
		}
		return Snapshot;
	}

	bool RestoreSlot(UPanelSlot* Slot, const FSlotSnapshot& Snapshot)
	{
		if (!Slot || Slot->GetClass() != Snapshot.SlotClass)
		{
			return false;
		}
		for (const auto& Pair : Snapshot.Values)
		{
			if (FProperty* Property = Slot->GetClass()->FindPropertyByName(Pair.Key))
			{
				Property->ImportText_InContainer(*Pair.Value, Slot, Slot, PPF_None);
			}
		}
		Slot->SynchronizeProperties();
		return true;
	}

	/** Sets alignment / padding on the slot of a single-content widget (Border, Button, SizeBox...). */
	void SetContentSlot(UPanelSlot* Slot, EHorizontalAlignment HAlign, EVerticalAlignment VAlign, const FMargin& Padding)
	{
		if (UBorderSlot* S = Cast<UBorderSlot>(Slot)) { S->SetHorizontalAlignment(HAlign); S->SetVerticalAlignment(VAlign); S->SetPadding(Padding); }
		else if (UButtonSlot* S2 = Cast<UButtonSlot>(Slot)) { S2->SetHorizontalAlignment(HAlign); S2->SetVerticalAlignment(VAlign); S2->SetPadding(Padding); }
		else if (USizeBoxSlot* S3 = Cast<USizeBoxSlot>(Slot)) { S3->SetHorizontalAlignment(HAlign); S3->SetVerticalAlignment(VAlign); S3->SetPadding(Padding); }
		else if (UScaleBoxSlot* S4 = Cast<UScaleBoxSlot>(Slot)) { S4->SetHorizontalAlignment(HAlign); S4->SetVerticalAlignment(VAlign); }
		else if (UBackgroundBlurSlot* S5 = Cast<UBackgroundBlurSlot>(Slot)) { S5->SetHorizontalAlignment(HAlign); S5->SetVerticalAlignment(VAlign); S5->SetPadding(Padding); }
		else if (UOverlaySlot* S6 = Cast<UOverlaySlot>(Slot)) { S6->SetHorizontalAlignment(HAlign); S6->SetVerticalAlignment(VAlign); S6->SetPadding(Padding); }
		else if (UWidgetSwitcherSlot* S7 = Cast<UWidgetSwitcherSlot>(Slot)) { S7->SetHorizontalAlignment(HAlign); S7->SetVerticalAlignment(VAlign); S7->SetPadding(Padding); }
	}

	EHorizontalAlignment ToHAlign(const FString& Align)
	{
		if (Align == TEXT("center")) return HAlign_Center;
		if (Align == TEXT("end")) return HAlign_Right;
		if (Align == TEXT("stretch")) return HAlign_Fill;
		return HAlign_Left;
	}

	EVerticalAlignment ToVAlign(const FString& Align)
	{
		if (Align == TEXT("center")) return VAlign_Center;
		if (Align == TEXT("end")) return VAlign_Bottom;
		if (Align == TEXT("stretch")) return VAlign_Fill;
		return VAlign_Top;
	}

	template <typename T>
	void SetPropertyByName(UObject* Object, const TCHAR* Name, const T& Value)
	{
		if (FProperty* Property = Object->GetClass()->FindPropertyByName(Name))
		{
			if (T* Ptr = Property->ContainerPtrToValuePtr<T>(Object))
			{
				*Ptr = Value;
			}
		}
	}

	void SetClassPropertyByName(UObject* Object, const TCHAR* Name, UClass* Value)
	{
		if (FClassProperty* Property = FindFProperty<FClassProperty>(Object->GetClass(), Name))
		{
			Property->SetObjectPropertyValue_InContainer(Object, Value);
		}
	}
}

using namespace DwoCanvasImport;

enum class EParentKind : uint8
{
	Root,
	RootComponent,
	Canvas,
	HBox,
	VBox,
	Wrap,
	Grid,
	Scroll,
	Frame,
	Single,
	Switcher
};

struct FDwoCanvasImporter::FDocContext
{
	FString DocId;
	FString DocName;
	FString AssetPath;
	bool bComponent = false;

	UWidgetBlueprint* Blueprint = nullptr;
	UWidgetTree* Tree = nullptr;

	FDwoCanvasImportRecord OldRecord;
	FDwoCanvasImportRecord NewRecord;

	TSet<FName> ReservedNames;
	TSet<FName> UsedNames;

	/** Edited (or referenced) generated widgets, detached, waiting to be placed again. */
	TMap<FName, UWidget*> Locked;
	TMap<UWidget*, FSlotSnapshot> LockedSlots;

	struct FUserWidget
	{
		UWidget* Widget = nullptr;
		FName ParentName;
		int32 Index = 0;
		FSlotSnapshot Slot;
	};
	TArray<FUserWidget> UserAdded;

	TSet<UWidget*> Created;
	TSet<FName> VariableNames;
	TSet<FString> NewNodeIds;
	TMap<FName, UClass*> BindAll;
	TMap<FName, UClass*> BindRequired;

	/** List / tile view name -> texts of each design entry (visual diff fills the preview rows with them). */
	FDwoCanvasListPreview ListPreview;
};

struct FDwoCanvasImporter::FImpl : public FGCObject
{
	/** Everything created or detached during the import. Saving a nested component can run GC while
	 *  a parent document's widgets are not attached to its tree yet. */
	TArray<TObjectPtr<UObject>> KeepAlive;

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override { Collector.AddReferencedObjects(KeepAlive); }
	virtual FString GetReferencerName() const override { return TEXT("FDwoCanvasImporter"); }

	FDwoCanvasImportOptions Options;
	FDwoCanvasReport* Report = nullptr;
	FString SourceDirectory;
	FString SourceName;

	/** "assets" (designbridge/1 "images" is migrated to it on load). */
	TMap<FString, FJson> Images;
	TMap<FString, UTexture2D*> Textures;
	TSet<FString> Capabilities;
	bool bDraft = false;
	/** Flattened design tokens: "color.primary" -> $value. */
	TMap<FString, TSharedPtr<FJsonValue>> TokenValues;
	int32 TokensApplied = 0;
	TMap<FString, FJson> ComponentDocs;
	TMap<FString, UClass*> ComponentClasses;
	TSet<FString> InProgress;
	TSet<FString> ListEntryComponents;
	TMap<FString, FLinearColor> Tokens;
	TSet<FString> WarnedFonts;

	const UDwoCanvasSettings& Settings = UDwoCanvasSettings::Get();

	// ----------------------------------------------------------------------------------------------------------------
	// Reporting

	void Warn(const FDocContext& C, const FNode& N, const FString& Message) { Report->Add(EDwoCanvasSeverity::Warning, C.DocName, N.Id, N.Name, Message); }
	void Info(const FDocContext& C, const FNode& N, const FString& Message) { Report->Add(EDwoCanvasSeverity::Info, C.DocName, N.Id, N.Name, Message); }
	void Error(const FDocContext& C, const FNode& N, const FString& Message) { Report->Add(EDwoCanvasSeverity::Error, C.DocName, N.Id, N.Name, Message); }

	// ----------------------------------------------------------------------------------------------------------------
	// Parsing

	FNode Parse(const FJson& J) const
	{
		FNode N;
		N.J = J;
		N.Id = JStr(J, TEXT("id"));
		N.Name = JStr(J, TEXT("name"), TEXT("Node"));
		N.Type = JStr(J, TEXT("type"), TEXT("frame"));
		N.bVisible = JBool(J, TEXT("visible"), true);
		N.Opacity = (float)JNum(J, TEXT("opacity"), 1.0);
		N.Rotation = (float)JNum(J, TEXT("rotation"), 0.0);
		N.bClip = JBool(J, TEXT("clip"), false);

		if (const FJson Box = JObj(J, TEXT("box")))
		{
			N.X = (float)JNum(Box, TEXT("x"));
			N.Y = (float)JNum(Box, TEXT("y"));
			N.W = (float)JNum(Box, TEXT("w"));
			N.H = (float)JNum(Box, TEXT("h"));
		}

		if (const FJson Layout = JObj(J, TEXT("layout")))
		{
			N.LayoutMode = JStr(Layout, TEXT("mode"), TEXT("none"));
			N.Gap = (float)JNum(Layout, TEXT("gap"));
			N.CrossGap = (float)JNum(Layout, TEXT("crossGap"));
			N.Padding = JMargin(Layout, TEXT("padding"));
			// Figma "strokes included in layout": children keep clear of the stroke's inner part
			if (JBool(Layout, TEXT("strokesIncluded"), false))
			{
				if (const FJson Stroke = JObj(J, TEXT("stroke")))
				{
					const FString Align = JStr(Stroke, TEXT("align"), TEXT("inside"));
					const float Inner = Align == TEXT("inside") ? 1.f : (Align == TEXT("center") ? 0.5f : 0.f);
					const TArray<double> Sides = JNums(Stroke, TEXT("sides")); // left, top, right, bottom
					const float Width = (float)JNum(Stroke, TEXT("width"));
					auto Side = [&](int32 Index) { return Inner * (Sides.Num() == 4 ? (float)Sides[Index] : Width); };
					N.Padding = N.Padding + FMargin(Side(0), Side(1), Side(2), Side(3));
				}
			}
			N.MainAlign = JStr(Layout, TEXT("mainAlign"), TEXT("start"));
			N.CrossAlign = JStr(Layout, TEXT("crossAlign"), TEXT("start"));
			N.Columns = (int32)JNum(Layout, TEXT("columns"));
		}

		if (const FJson Sizing = JObj(J, TEXT("sizing")))
		{
			N.SizingH = JStr(Sizing, TEXT("h"), TEXT("fixed"));
			N.SizingV = JStr(Sizing, TEXT("v"), TEXT("fixed"));
			N.MinW = (float)JNum(Sizing, TEXT("minW"));
			N.MaxW = (float)JNum(Sizing, TEXT("maxW"));
			N.MinH = (float)JNum(Sizing, TEXT("minH"));
			N.MaxH = (float)JNum(Sizing, TEXT("maxH"));
		}
		else if (N.Type == TEXT("text"))
		{
			N.SizingH = N.SizingV = TEXT("hug");
		}

		N.bAbsolute = JStr(J, TEXT("positioning"), TEXT("auto")) == TEXT("absolute");
		if (const FJson Constraints = JObj(J, TEXT("constraints")))
		{
			N.ConstraintH = JStr(Constraints, TEXT("h"), TEXT("min"));
			N.ConstraintV = JStr(Constraints, TEXT("v"), TEXT("min"));
		}

		// Role: the document's role field (set by the adapter) wins; name prefixes are the fallback
		FString LayerName = N.Name;
		const FString RoleField = JStr(J, TEXT("role"));
		const FDwoCanvasRoleRule* PrefixRule = Settings.FindRoleRule(LayerName);
		if (!RoleField.IsEmpty())
		{
			N.RoleName = RoleField;
			N.Role = RoleFromName(RoleField);
			N.bMakeVariable = N.Role != EDwoCanvasRole::Auto && N.Role != EDwoCanvasRole::Spacer && N.Role != EDwoCanvasRole::Ignore;
			// Adapters keep the source layer name: strip the prefix that produced this role
			if (PrefixRule && Settings.bStripRolePrefix && PrefixRule->Role == N.Role && N.Role != EDwoCanvasRole::Ignore)
			{
				LayerName.RightChopInline(PrefixRule->Prefix.Len());
			}
		}
		else if (PrefixRule)
		{
			N.Role = PrefixRule->Role;
			N.RoleName = RoleToName(PrefixRule->Role);
			N.bMakeVariable = PrefixRule->bMakeVariable;
			if (Settings.bStripRolePrefix && PrefixRule->Role != EDwoCanvasRole::Ignore)
			{
				LayerName.RightChopInline(PrefixRule->Prefix.Len());
			}
		}

		if (!Settings.VariablePrefix.IsEmpty() && LayerName.StartsWith(Settings.VariablePrefix))
		{
			LayerName.RightChopInline(Settings.VariablePrefix.Len());
			N.bMakeVariable = true;
		}
		bool bVariable = false;
		if (J->TryGetBoolField(TEXT("variable"), bVariable))
		{
			N.bMakeVariable = bVariable;
		}

		const FString ExplicitName = JStr(J, TEXT("widgetName"));
		if (!ExplicitName.IsEmpty())
		{
			LayerName = ExplicitName;
		}

		N.Confidence = (float)JNum(J, TEXT("confidence"), -1.0);
		if (const FJsonArray* Inferred = JArr(J, TEXT("inferred")))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Inferred)
			{
				N.InferredFields.Add(Value->AsString());
			}
		}

		N.WidgetName = SanitizeName(LayerName);
		return N;
	}

	static void CollectNodeIds(const FJson& Node, TSet<FString>& OutIds)
	{
		OutIds.Add(JStr(Node, TEXT("id")));
		if (const FJsonArray* Children = JArr(Node, TEXT("children")))
		{
			for (const TSharedPtr<FJsonValue>& Child : *Children)
			{
				CollectNodeIds(Child->AsObject(), OutIds);
			}
		}
	}

	void CollectListEntries(const FJson& Node)
	{
		const FNode N = Parse(Node);
		const FJsonArray* Children = JArr(Node, TEXT("children"));
		if ((N.Role == EDwoCanvasRole::ListView || N.Role == EDwoCanvasRole::TileView) && Children && Children->Num() > 0)
		{
			const FString ComponentId = JStr(JObj((*Children)[0]->AsObject(), TEXT("component")), TEXT("ref"));
			if (!ComponentId.IsEmpty())
			{
				ListEntryComponents.Add(ComponentId);
			}
		}
		if (Children)
		{
			for (const TSharedPtr<FJsonValue>& Child : *Children)
			{
				CollectListEntries(Child->AsObject());
			}
		}
	}

	// ----------------------------------------------------------------------------------------------------------------
	// Assets

	bool LoadImageBytes(const FString& ImageId, TArray<uint8>& OutBytes, FJson& OutInfo) const
	{
		const FJson* Info = Images.Find(ImageId);
		if (!Info)
		{
			return false;
		}
		OutInfo = *Info;
		const FString Data = JStr(*Info, TEXT("data"));
		if (!Data.IsEmpty())
		{
			return FBase64::Decode(Data, OutBytes);
		}
		FString File = JStr(*Info, TEXT("uri"), JStr(*Info, TEXT("file")));
		if (File.StartsWith(TEXT("file:///")))
		{
			File.RightChopInline(8);
		}
		else if (File.StartsWith(TEXT("file://")))
		{
			File.RightChopInline(7);
		}
		if (File.IsEmpty())
		{
			return false;
		}
		const FString Path = FPaths::IsRelative(File) ? FPaths::Combine(SourceDirectory, File) : File;
		return FFileHelper::LoadFileToArray(OutBytes, *Path);
	}

	UTexture2D* GetTexture(const FString& ImageId)
	{
		if (UTexture2D** Cached = Textures.Find(ImageId))
		{
			return *Cached;
		}

		TArray<uint8> Bytes;
		FJson Info;
		if (!LoadImageBytes(ImageId, Bytes, Info))
		{
			Report->Add(EDwoCanvasSeverity::Error, SourceName, ImageId, TEXT(""), TEXT("Image data missing"));
			Textures.Add(ImageId, nullptr);
			return nullptr;
		}

		// Same pixels -> same asset, across documents and imports
		FSHA1 Sha;
		Sha.Update(Bytes.GetData(), Bytes.Num());
		Sha.Final();
		FSHAHash Hash;
		Sha.GetHash(Hash.Hash);
		const FString AssetName = TEXT("T_DB_") + Hash.ToString().Left(16);
		const FString PackagePath = Settings.TextureFolder / AssetName;

		UTexture2D* Texture = nullptr;
		if (FPackageName::DoesPackageExist(PackagePath))
		{
			Texture = LoadObject<UTexture2D>(nullptr, *(PackagePath + TEXT(".") + AssetName));
		}

		if (!Texture)
		{
			FImage Image;
			if (!FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Image))
			{
				Report->Add(EDwoCanvasSeverity::Error, SourceName, ImageId, TEXT(""), TEXT("Image could not be decoded (PNG / JPG expected)"));
				Textures.Add(ImageId, nullptr);
				return nullptr;
			}
			Image.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);

			UPackage* Package = CreatePackage(*PackagePath);
			Package->FullyLoad();
			Texture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
			Texture->Source.Init(Image);
			Texture->LODGroup = TEXTUREGROUP_UI;
			Texture->CompressionSettings = TC_EditorIcon;
			Texture->MipGenSettings = TMGS_NoMipmaps;
			Texture->SRGB = true;
			Texture->NeverStream = true;
			Texture->PostEditChange();
			FAssetRegistryModule::AssetCreated(Texture);
			Package->MarkPackageDirty();
			UEditorLoadingAndSavingUtils::SavePackages({ Package }, false);
		}

		Textures.Add(ImageId, Texture);
		KeepAlive.Add(Texture);
		return Texture;
	}

	FVector2D GetTextureSize(UTexture2D* Texture, const FString& ImageId) const
	{
		if (!Texture)
		{
			return FVector2D(1.f, 1.f);
		}
		const FJson* Info = Images.Find(ImageId);
		const double Scale = Info ? FMath::Max(JNum(*Info, TEXT("scale"), 1.0), 0.01) : 1.0;
		return FVector2D(Texture->Source.GetSizeX() / Scale, Texture->Source.GetSizeY() / Scale);
	}

	// ----------------------------------------------------------------------------------------------------------------
	// Styles

	FLinearColor ResolveColor(const FJson& Paint, const TCHAR* Key = TEXT("color")) const
	{
		FLinearColor Color = JColor(Paint, Key, FLinearColor::White);
		const FString Token = JStr(Paint, TEXT("colorToken"), JStr(Paint, TEXT("token")));
		if (!Token.IsEmpty())
		{
			if (const FLinearColor* Override = Settings.ColorTokenOverrides.Find(Token))
			{
				const float Alpha = Color.A;
				Color = *Override;
				Color.A *= Alpha;
			}
		}
		Color.A *= (float)JNum(Paint, TEXT("opacity"), 1.0);
		return Color;
	}

	/** Visible paints of a "fills" array, top-most last. */
	static TArray<FJson> VisibleFills(const FJson& Node)
	{
		TArray<FJson> Result;
		if (const FJsonArray* Fills = JArr(Node, TEXT("fills")))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Fills)
			{
				const FJson Fill = Value->AsObject();
				if (Fill.IsValid() && JBool(Fill, TEXT("visible"), true))
				{
					Result.Add(Fill);
				}
			}
		}
		return Result;
	}

	struct FVisual
	{
		FDwoCanvasShapeStyle Style;
		bool bHasFill = false;
		bool bHasStroke = false;
		bool bHasShadow = false;
		FJson ImageFill;
		float BackgroundBlur = 0.f;

		bool IsVisible() const { return bHasFill || bHasStroke || bHasShadow || ImageFill.IsValid(); }
	};

	FVisual ReadVisual(const FDocContext& C, const FNode& N, const FJson& Source) const
	{
		FVisual V;
		FDwoCanvasShapeStyle& S = V.Style;
		S.FillType = EDwoCanvasFillType::None;

		const TArray<FJson> Fills = VisibleFills(Source);
		if (Fills.Num() > 1)
		{
			const_cast<FImpl*>(this)->Warn(C, N, FString::Printf(TEXT("%d stacked fills: only the top one is used (bake the node to keep them all)"), Fills.Num()));
		}
		if (Fills.Num() > 0)
		{
			const FJson Fill = Fills.Last();
			const FString Type = JStr(Fill, TEXT("type"), TEXT("solid"));
			if (Type == TEXT("image"))
			{
				V.ImageFill = Fill;
			}
			else if (Type == TEXT("solid"))
			{
				S.FillType = EDwoCanvasFillType::Solid;
				S.FillColor = ResolveColor(Fill);
				V.bHasFill = S.FillColor.A > 0.f;
			}
			else
			{
				S.FillType = Type == TEXT("linear") ? EDwoCanvasFillType::Linear
					: Type == TEXT("radial") ? EDwoCanvasFillType::Radial
					: Type == TEXT("angular") ? EDwoCanvasFillType::Angular
					: Type == TEXT("diamond") ? EDwoCanvasFillType::Diamond : EDwoCanvasFillType::Linear;
				const float PaintOpacity = (float)JNum(Fill, TEXT("opacity"), 1.0);
				if (const FJsonArray* Stops = JArr(Fill, TEXT("stops")))
				{
					for (const TSharedPtr<FJsonValue>& StopValue : *Stops)
					{
						const FJson Stop = StopValue->AsObject();
						FDwoCanvasGradientStop GradientStop;
						GradientStop.Position = (float)JNum(Stop, TEXT("position"));
						GradientStop.Color = JColor(Stop, TEXT("color"), FLinearColor::White);
						GradientStop.Color.A *= PaintOpacity;
						S.GradientStops.Add(GradientStop);
					}
				}
				if (S.GradientStops.Num() > 4)
				{
					const_cast<FImpl*>(this)->Warn(C, N, TEXT("Gradient has more than 4 stops: extra stops are dropped"));
				}
				const TArray<double> Handles = JNums(Fill, TEXT("handles"));
				if (Handles.Num() >= 6)
				{
					S.GradientStart = FVector2D(Handles[0], Handles[1]);
					S.GradientEnd = FVector2D(Handles[2], Handles[3]);
					S.GradientWidth = FVector2D(Handles[4], Handles[5]);
				}
				V.bHasFill = true;
			}
		}

		if (const FJson Stroke = JObj(Source, TEXT("stroke")))
		{
			S.StrokeWidth = (float)JNum(Stroke, TEXT("width"));
			S.StrokeColor = ResolveColor(Stroke);
			const FString Align = JStr(Stroke, TEXT("align"), TEXT("inside"));
			S.StrokeAlign = Align == TEXT("center") ? EDwoCanvasStrokeAlign::Center : (Align == TEXT("outside") ? EDwoCanvasStrokeAlign::Outside : EDwoCanvasStrokeAlign::Inside);
			const TArray<double> Dash = JNums(Stroke, TEXT("dash"));
			if (Dash.Num() >= 2)
			{
				S.DashLength = (float)Dash[0];
				S.DashGap = (float)Dash[1];
			}
			const TArray<double> Sides = JNums(Stroke, TEXT("sides"));
			if (Sides.Num() >= 4 && !(Sides[0] == Sides[1] && Sides[1] == Sides[2] && Sides[2] == Sides[3]))
			{
				const_cast<FImpl*>(this)->Warn(C, N, TEXT("Per-side stroke widths are not supported: using the uniform width (bake for exact result)"));
			}
			V.bHasStroke = S.StrokeWidth > 0.f && S.StrokeColor.A > 0.f;
		}

		const TArray<double> Radius = JNums(Source, TEXT("radius"));
		if (Radius.Num() >= 4)
		{
			S.CornerRadii = FVector4(Radius[0], Radius[1], Radius[2], Radius[3]);
		}
		else if (Radius.Num() == 1)
		{
			S.CornerRadii = FVector4(Radius[0], Radius[0], Radius[0], Radius[0]);
		}
		if (N.Type == TEXT("ellipse"))
		{
			const float R = FMath::Min(N.W, N.H) * 0.5f;
			S.CornerRadii = FVector4(R, R, R, R);
			if (!FMath::IsNearlyEqual(N.W, N.H, 0.5f))
			{
				const_cast<FImpl*>(this)->Warn(C, N, TEXT("Non-circular ellipse drawn as a pill shape (bake it for an exact ellipse)"));
			}
		}

		if (const FJsonArray* Effects = JArr(Source, TEXT("effects")))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Effects)
			{
				const FJson Effect = Value->AsObject();
				if (!JBool(Effect, TEXT("visible"), true))
				{
					continue;
				}
				const FString Type = JStr(Effect, TEXT("type"));
				if (Type == TEXT("drop-shadow") || Type == TEXT("inner-shadow"))
				{
					FDwoCanvasShadow& Shadow = Type == TEXT("drop-shadow") ? S.DropShadow : S.InnerShadow;
					if (Shadow.bEnabled)
					{
						const_cast<FImpl*>(this)->Warn(C, N, FString::Printf(TEXT("Only one %s is supported"), *Type));
						continue;
					}
					Shadow.bEnabled = true;
					Shadow.Color = JColor(Effect, TEXT("color"), FLinearColor(0, 0, 0, 0.25f));
					const TArray<double> Offset = JNums(Effect, TEXT("offset"));
					Shadow.Offset = Offset.Num() >= 2 ? FVector2D(Offset[0], Offset[1]) : FVector2D::ZeroVector;
					Shadow.Blur = (float)JNum(Effect, TEXT("blur"));
					Shadow.Spread = (float)JNum(Effect, TEXT("spread"));
					Shadow.bShowBehindFill = Type == TEXT("drop-shadow") && JBool(Effect, TEXT("showBehind"), false);
					V.bHasShadow = true;
				}
				else if (Type == TEXT("background-blur"))
				{
					V.BackgroundBlur = (float)JNum(Effect, TEXT("radius"));
				}
				else if (Type == TEXT("layer-blur"))
				{
					const_cast<FImpl*>(this)->Warn(C, N, TEXT("Layer blur is not supported live: bake the node"));
				}
			}
		}

		const FString Blend = JStr(Source, TEXT("blendMode"), TEXT("normal"));
		if (Blend != TEXT("normal") && Blend != TEXT("pass-through"))
		{
			const_cast<FImpl*>(this)->Warn(C, N, FString::Printf(TEXT("Blend mode '%s' is not supported live: bake the node"), *Blend));
		}

		if (S.FillType == EDwoCanvasFillType::None && !V.bHasFill)
		{
			S.FillType = EDwoCanvasFillType::None;
		}
		return V;
	}

	/** Brush for an image fill (texture, crop / tile, rounded corners, stroke). */
	FSlateBrush MakeImageBrush(const FDocContext& C, const FNode& N, const FVisual& V, FVector2D DrawSize, bool& bOutNeedsScaleBox)
	{
		bOutNeedsScaleBox = false;
		FSlateBrush Brush;
		const FString ImageId = JStr(V.ImageFill, TEXT("image"));
		UTexture2D* Texture = GetTexture(ImageId);
		Brush.SetResourceObject(Texture);
		Brush.ImageSize = DrawSize;
		Brush.TintColor = FSlateColor(FLinearColor(1.f, 1.f, 1.f, (float)JNum(V.ImageFill, TEXT("opacity"), 1.0)));

		const FVector2D TextureSize = GetTextureSize(Texture, ImageId);
		const FString ScaleMode = JStr(V.ImageFill, TEXT("scaleMode"), TEXT("fill"));
		if (ScaleMode == TEXT("tile"))
		{
			Brush.Tiling = ESlateBrushTileType::Both;
			Brush.ImageSize = TextureSize * JNum(V.ImageFill, TEXT("scalingFactor"), 1.0);
		}
		else if (ScaleMode == TEXT("fit"))
		{
			bOutNeedsScaleBox = true;
			Brush.ImageSize = TextureSize;
		}
		else
		{
			if (ScaleMode == TEXT("crop"))
			{
				Info(C, N, TEXT("Image crop is approximated as 'fill' (center crop)"));
			}
			// Cover: crop the texture to the box aspect
			const double TexAspect = TextureSize.X / FMath::Max(TextureSize.Y, 1.0);
			const double BoxAspect = DrawSize.X / FMath::Max(DrawSize.Y, 1.0);
			if (TexAspect > BoxAspect + KINDA_SMALL_NUMBER)
			{
				const float U = (float)(BoxAspect / TexAspect);
				Brush.SetUVRegion(FBox2f(FVector2f(0.5f - U * 0.5f, 0.f), FVector2f(0.5f + U * 0.5f, 1.f)));
			}
			else if (TexAspect < BoxAspect - KINDA_SMALL_NUMBER)
			{
				const float Vv = (float)(TexAspect / BoxAspect);
				Brush.SetUVRegion(FBox2f(FVector2f(0.f, 0.5f - Vv * 0.5f), FVector2f(1.f, 0.5f + Vv * 0.5f)));
			}
		}

		const FJson* Info = Images.Find(ImageId);
		const TArray<double> NineSlice = Info ? JNums(*Info, TEXT("nineSlice")) : TArray<double>();
		const bool bRounded = V.Style.CornerRadii != FVector4(0, 0, 0, 0) || V.bHasStroke;
		if (NineSlice.Num() >= 4 && Texture)
		{
			Brush.DrawAs = ESlateBrushDrawType::Box;
			Brush.Margin = FMargin(NineSlice[0] / TextureSize.X, NineSlice[1] / TextureSize.Y, NineSlice[2] / TextureSize.X, NineSlice[3] / TextureSize.Y);
		}
		else if (bRounded)
		{
			Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
			Brush.OutlineSettings = FSlateBrushOutlineSettings(V.Style.CornerRadii, FSlateColor(V.Style.StrokeColor), V.Style.StrokeWidth);
			Brush.OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
		}
		else
		{
			Brush.DrawAs = ESlateBrushDrawType::Image;
		}
		return Brush;
	}

	/** Brush from a visual JSON (button states). Complex looks are approximated. */
	FSlateBrush MakeStateBrush(const FDocContext& C, const FNode& N, const FJson& Source, FVector2D Size)
	{
		const FString Bake = JStr(JObj(Source, TEXT("bake")), TEXT("image"));
		if (!Bake.IsEmpty())
		{
			FSlateBrush Brush;
			Brush.SetResourceObject(GetTexture(Bake));
			Brush.ImageSize = Size;
			Brush.DrawAs = ESlateBrushDrawType::Image;
			return Brush;
		}

		const FVisual V = ReadVisual(C, N, Source);
		if (V.ImageFill.IsValid())
		{
			bool bScaleBox = false;
			return MakeImageBrush(C, N, V, Size, bScaleBox);
		}
		FDwoCanvasShapeStyle Style = V.Style;
		if (Style.NeedsMaterial())
		{
			Warn(C, N, TEXT("Button state uses a gradient / shadow / complex stroke: approximated with a flat brush (export the states baked for an exact look)"));
			if (Style.FillType != EDwoCanvasFillType::None && Style.FillType != EDwoCanvasFillType::Solid && Style.GradientStops.Num() > 0)
			{
				Style.FillColor = Style.GradientStops[0].Color;
				Style.FillType = EDwoCanvasFillType::Solid;
			}
		}
		FSlateBrush Brush = Style.MakeSimpleBrush();
		Brush.ImageSize = Size;
		return Brush;
	}

	// ----------------------------------------------------------------------------------------------------------------
	// Widget creation (record aware)

	FName UniqueName(FDocContext& C, const FString& Base) const
	{
		FName Name(*Base);
		int32 Suffix = 1;
		while (C.UsedNames.Contains(Name) || C.ReservedNames.Contains(Name) || FindObjectFast<UObject>(C.Tree, Name))
		{
			Name = FName(*FString::Printf(TEXT("%s_%d"), *Base, Suffix++));
		}
		return Name;
	}

	/**
	 * Creates (or takes back) the widget for a node part. bOutLocked = the widget was edited in UE:
	 * reuse it as is and do not apply design properties.
	 */
	UWidget* Make(FDocContext& C, const FString& Key, UClass* Class, const FString& BaseName, bool& bOutLocked)
	{
		bOutLocked = false;
		FName Name;
		const FDwoCanvasRecordEntry* Old = C.OldRecord.Entries.Find(Key);
		if (Old)
		{
			if (UWidget** Kept = C.Locked.Find(Old->WidgetName))
			{
				if ((*Kept)->GetClass() == Class)
				{
					UWidget* Widget = *Kept;
					C.Locked.Remove(Old->WidgetName);
					C.UsedNames.Add(Widget->GetFName());
					FDwoCanvasRecordEntry Entry = *Old;
					Entry.bLocked = true;
					C.NewRecord.Entries.Add(Key, Entry);
					bOutLocked = true;
					++Report->KeptEdited;
					return Widget;
				}
				Report->Add(EDwoCanvasSeverity::Warning, C.DocName, Key, Old->WidgetName.ToString(),
					FString::Printf(TEXT("Widget was edited in UE but the design now needs a %s: the edited widget is parked"), *Class->GetName()));
			}

			C.ReservedNames.Remove(Old->WidgetName);
			Name = (!C.UsedNames.Contains(Old->WidgetName) && !FindObjectFast<UObject>(C.Tree, Old->WidgetName)) ? Old->WidgetName : UniqueName(C, BaseName);
			++Report->Regenerated;
		}
		else
		{
			Name = UniqueName(C, BaseName);
			++Report->Created;
		}

		UWidget* Widget = FDwoCanvasWidgetWriter::ConstructInTree(C.Blueprint, Class, Name);
		KeepAlive.Add(Widget);
		C.UsedNames.Add(Name);
		C.Created.Add(Widget);

		FDwoCanvasRecordEntry Entry;
		Entry.WidgetName = Name;
		Entry.ClassPath = Class->GetPathName();
		C.NewRecord.Entries.Add(Key, Entry);
		return Widget;
	}

	template <typename T>
	T* Make(FDocContext& C, const FString& Key, const FString& BaseName, bool& bOutLocked)
	{
		return Cast<T>(Make(C, Key, T::StaticClass(), BaseName, bOutLocked));
	}

	/** Adds a child, restoring the slot of an edited widget, otherwise calling Apply. */
	template <typename TApply>
	UPanelSlot* AddChild(FDocContext& C, UPanelWidget* Panel, UWidget* Child, TApply&& Apply)
	{
		UPanelSlot* Slot = Panel->AddChild(Child);
		const FSlotSnapshot* Snapshot = C.LockedSlots.Find(Child);
		if (!(Snapshot && RestoreSlot(Slot, *Snapshot)))
		{
			Apply(Slot);
		}
		return Slot;
	}

	// ----------------------------------------------------------------------------------------------------------------
	// Node builders

	/** Returns the outermost widget of the node. bOutSelfSized = it reports its design size on its own. */
	UWidget* BuildNode(FDocContext& C, const FJson& Json, EParentKind ParentKind)
	{
		const FNode N = Parse(Json);
		if (N.Role == EDwoCanvasRole::Ignore)
		{
			return nullptr;
		}

		if (N.Confidence >= 0.f && N.Confidence < Settings.LowConfidenceThreshold)
		{
			Warn(C, N, FString::Printf(TEXT("Inferred with low confidence (%d%%)%s: check it against the source"), FMath::RoundToInt(N.Confidence * 100.f),
				N.InferredFields.Num() ? *(TEXT(" - guessed: ") + FString::Join(N.InferredFields, TEXT(", "))) : TEXT("")));
		}

		bool bSelfSized = false;
		UWidget* Widget = nullptr;
		const FJson Bake = JObj(Json, TEXT("bake"));
		const FJson Component = JObj(Json, TEXT("component"));
		const bool bHasChildren = JArr(Json, TEXT("children")) && JArr(Json, TEXT("children"))->Num() > 0;

		// Project role handlers first (x- roles, or replacements for built-in roles)
		if (!N.RoleName.IsEmpty())
		{
			if (const TSharedPtr<IDwoCanvasRoleHandler> Handler = FDwoCanvasRoleHandlers::Find(N.RoleName))
			{
				Widget = BuildWithHandler(C, N, *Handler);
			}
			else if (N.RoleName.StartsWith(TEXT("x-")))
			{
				Warn(C, N, FString::Printf(TEXT("No role handler is registered for '%s': built as a plain layout"), *N.RoleName));
			}
		}

		if (Widget)
		{
		}
		else if (Bake.IsValid())
		{
			Widget = BuildBaked(C, N, Bake, bSelfSized);
		}
		else if (N.Role == EDwoCanvasRole::Button)
		{
			Widget = BuildButton(C, N);
		}
		else if (N.Role == EDwoCanvasRole::ListView || N.Role == EDwoCanvasRole::TileView)
		{
			Widget = BuildList(C, N);
		}
		else if (N.Role == EDwoCanvasRole::TextInput || N.Role == EDwoCanvasRole::CheckBox || N.Role == EDwoCanvasRole::Slider
			|| N.Role == EDwoCanvasRole::ProgressBar || N.Role == EDwoCanvasRole::Spacer || N.Role == EDwoCanvasRole::NamedSlot)
		{
			Widget = BuildControl(C, N, bSelfSized);
		}
		else if (N.Type == TEXT("text"))
		{
			Widget = BuildText(C, N, bSelfSized);
		}
		else if (Component.IsValid() && !ShouldInline(N, Component))
		{
			Widget = BuildInstance(C, N, Component, bSelfSized);
		}
		else if (bHasChildren || N.Role == EDwoCanvasRole::ScrollBox || N.Role == EDwoCanvasRole::Switcher)
		{
			Widget = BuildContainer(C, N, N.Id, false, bSelfSized);
		}
		else
		{
			Widget = BuildLeaf(C, N, bSelfSized);
		}

		if (!Widget)
		{
			return nullptr;
		}

		if (N.bMakeVariable)
		{
			C.VariableNames.Add(Widget->GetFName());
		}

		// Background blur sits behind the node
		const FVisual Visual = ReadVisual(C, N, N.J);
		if (Visual.BackgroundBlur > 0.f && !Bake.IsValid())
		{
			bool bLocked = false;
			UBackgroundBlur* Blur = Make<UBackgroundBlur>(C, N.Id + TEXT("#blur"), N.WidgetName + TEXT("_Blur"), bLocked);
			if (!bLocked)
			{
				Blur->SetBlurStrength(Visual.BackgroundBlur * 0.5f);
				Blur->SetCornerRadius(Visual.Style.CornerRadii);
				Blur->SetPadding(FMargin(0.f));
				Blur->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
			}
			AddChild(C, Blur, Widget, [](UPanelSlot* Slot) { SetContentSlot(Slot, HAlign_Fill, VAlign_Fill, FMargin(0.f)); });
			Widget = Blur;
			Info(C, N, TEXT("Background blur -> BackgroundBlur widget (costly: keep its use limited)"));
		}

		// Fixed design size inside a flow layout
		const bool bFlowParent = ParentKind != EParentKind::Canvas && ParentKind != EParentKind::Root;
		const bool bNeedsSize = bFlowParent && !bSelfSized
			&& ((N.IsFixedH() && N.W > 0.f) || (N.IsFixedV() && N.H > 0.f) || N.MinW > 0.f || N.MaxW > 0.f || N.MinH > 0.f || N.MaxH > 0.f);
		if (bNeedsSize)
		{
			bool bLocked = false;
			USizeBox* Size = Make<USizeBox>(C, N.Id + TEXT("#size"), N.WidgetName + TEXT("_Size"), bLocked);
			if (!bLocked)
			{
				if (N.IsFixedH() && N.W > 0.f) { Size->SetWidthOverride(N.W); }
				if (N.IsFixedV() && N.H > 0.f) { Size->SetHeightOverride(N.H); }
				if (N.MinW > 0.f) { Size->SetMinDesiredWidth(N.MinW); }
				if (N.MaxW > 0.f) { Size->SetMaxDesiredWidth(N.MaxW); }
				if (N.MinH > 0.f) { Size->SetMinDesiredHeight(N.MinH); }
				if (N.MaxH > 0.f) { Size->SetMaxDesiredHeight(N.MaxH); }
				Size->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
			}
			AddChild(C, Size, Widget, [](UPanelSlot* Slot) { SetContentSlot(Slot, HAlign_Fill, VAlign_Fill, FMargin(0.f)); });
			Widget = Size;
		}

		// Common properties on the outermost widget
		if (C.Created.Contains(Widget))
		{
			if (!N.bVisible)
			{
				Widget->SetVisibility(ESlateVisibility::Collapsed);
			}
			if (N.Opacity < 1.f)
			{
				Widget->SetRenderOpacity(N.Opacity);
			}
			if (!FMath::IsNearlyZero(N.Rotation))
			{
				// Design tools rotate counter-clockwise; UMG clockwise
				Widget->SetRenderTransformAngle(-N.Rotation);
				Widget->SetRenderTransformPivot(FVector2D(0.5f, 0.5f));
			}
		}
		return Widget;
	}

	bool ShouldInline(const FNode& N, const FJson& Component) const
	{
		if (Settings.FindComponentOverride(JStr(Component, TEXT("name"))))
		{
			return false;
		}
		const bool bHasChildren = JArr(N.J, TEXT("children")) && JArr(N.J, TEXT("children"))->Num() > 0;
		switch (Settings.InstancePolicy)
		{
		case EDwoCanvasInstancePolicy::AlwaysInline: return bHasChildren;
		case EDwoCanvasInstancePolicy::AlwaysReference: return false;
		default: return bHasChildren && JBool(Component, TEXT("overridden"));
		}
	}

	static EParentKind KindOfPanel(const UPanelWidget* Panel)
	{
		if (Cast<UCanvasPanel>(Panel)) { return EParentKind::Canvas; }
		if (Cast<UHorizontalBox>(Panel)) { return EParentKind::HBox; }
		if (Cast<UVerticalBox>(Panel)) { return EParentKind::VBox; }
		if (Cast<UWrapBox>(Panel)) { return EParentKind::Wrap; }
		if (Cast<UGridPanel>(Panel)) { return EParentKind::Grid; }
		if (Cast<UScrollBox>(Panel)) { return EParentKind::Scroll; }
		if (Cast<UDwoCanvasFrame>(Panel)) { return EParentKind::Frame; }
		if (Cast<UWidgetSwitcher>(Panel)) { return EParentKind::Switcher; }
		return EParentKind::Single;
	}

	UWidget* BuildWithHandler(FDocContext& C, const FNode& N, IDwoCanvasRoleHandler& Handler)
	{
		FDwoCanvasBuildContextData Data;
		Data.Node = N.J;
		Data.NodeId = N.Id;
		Data.Role = N.RoleName;
		Data.WidgetName = N.WidgetName;
		Data.DocumentName = C.DocName;
		Data.Size = FVector2D(N.W, N.H);
		Data.Make = [this, &C, &N](UClass* Class, const FString& Suffix, bool& bOutLocked) -> UWidget*
		{
			if (!Class)
			{
				return nullptr;
			}
			return Make(C, Suffix.IsEmpty() ? N.Id : N.Id + TEXT("#") + Suffix, Class,
				Suffix.IsEmpty() ? N.WidgetName : N.WidgetName + TEXT("_") + SanitizeName(Suffix), bOutLocked);
		};
		Data.BuildChildren = [this, &C, &N](UPanelWidget* Panel)
		{
			TArray<FJson> Children;
			if (const FJsonArray* Array = JArr(N.J, TEXT("children")))
			{
				for (const TSharedPtr<FJsonValue>& Value : *Array)
				{
					Children.Add(Value->AsObject());
				}
			}
			const EParentKind Kind = KindOfPanel(Panel);
			if (Kind == EParentKind::Single)
			{
				// Content widgets (Border, Button, SizeBox...) hold one child
				if (Children.Num() > 0)
				{
					if (UWidget* Child = BuildNode(C, Children[0], EParentKind::Single))
					{
						AddChild(C, Panel, Child, [&N](UPanelSlot* Slot) { SetContentSlot(Slot, HAlign_Fill, VAlign_Fill, N.Padding); });
					}
				}
				if (Children.Num() > 1)
				{
					Warn(C, N, FString::Printf(TEXT("Role handler put the children into a single-content %s: only the first of %d is built"), *Panel->GetClass()->GetName(), Children.Num()));
				}
				return;
			}
			AddFlowChildren(C, N, Panel, Kind, Children);
		};
		Data.Color = [this](const TSharedPtr<FJsonObject>& Paint) { return ResolveColor(Paint); };
		Data.Log = [this, &C, &N](bool bWarning, const FString& Message) { bWarning ? Warn(C, N, Message) : Info(C, N, Message); };

		FDwoCanvasBuildContext Context(Data);
		UWidget* Widget = Handler.Build(Context);
		if (Widget)
		{
			Info(C, N, FString::Printf(TEXT("Built by the '%s' role handler"), *N.RoleName));
		}
		return Widget;
	}

	UWidget* BuildBaked(FDocContext& C, const FNode& N, const FJson& Bake, bool& bOutSelfSized)
	{
		const FString ImageId = JStr(Bake, TEXT("image"));
		const FMargin Overflow = JMargin(Bake, TEXT("overflow"));

		bool bLocked = false;
		UImage* Image = Make<UImage>(C, N.Id, N.WidgetName, bLocked);
		if (!bLocked)
		{
			FSlateBrush Brush;
			Brush.SetResourceObject(GetTexture(ImageId));
			Brush.ImageSize = FVector2D(N.W + Overflow.Left + Overflow.Right, N.H + Overflow.Top + Overflow.Bottom);
			Brush.DrawAs = ESlateBrushDrawType::Image;
			Image->SetBrush(Brush);
			Image->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		bOutSelfSized = true;
		Info(C, N, TEXT("Baked to image"));

		if (Overflow.Left == 0.f && Overflow.Top == 0.f && Overflow.Right == 0.f && Overflow.Bottom == 0.f)
		{
			return Image;
		}

		// Keep the layout size at the design box; the shadow / effect overflow is drawn outside it
		USizeBox* Size = Make<USizeBox>(C, N.Id + TEXT("#bakebox"), N.WidgetName + TEXT("_Box"), bLocked);
		if (!bLocked)
		{
			Size->SetWidthOverride(N.W);
			Size->SetHeightOverride(N.H);
			Size->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		AddChild(C, Size, Image, [&Overflow](UPanelSlot* Slot)
		{
			SetContentSlot(Slot, HAlign_Fill, VAlign_Fill, FMargin(-Overflow.Left, -Overflow.Top, -Overflow.Right, -Overflow.Bottom));
		});
		return Size;
	}

	UWidget* BuildLeaf(FDocContext& C, const FNode& N, bool& bOutSelfSized)
	{
		const FVisual V = ReadVisual(C, N, N.J);
		const FVector2D Size(N.W, N.H);
		bOutSelfSized = true;
		bool bLocked = false;

		if (V.ImageFill.IsValid())
		{
			bool bScaleBox = false;
			UImage* Image = Make<UImage>(C, N.Id, N.WidgetName, bLocked);
			if (!bLocked)
			{
				Image->SetBrush(MakeImageBrush(C, N, V, Size, bScaleBox));
				Image->SetVisibility(ESlateVisibility::HitTestInvisible);
			}
			else
			{
				MakeImageBrush(C, N, V, Size, bScaleBox);
			}
			if (!bScaleBox)
			{
				return Image;
			}

			UScaleBox* Scale = Make<UScaleBox>(C, N.Id + TEXT("#fit"), N.WidgetName + TEXT("_Fit"), bLocked);
			if (!bLocked)
			{
				Scale->SetStretch(EStretch::ScaleToFit);
				Scale->SetVisibility(ESlateVisibility::HitTestInvisible);
			}
			AddChild(C, Scale, Image, [](UPanelSlot* Slot) { SetContentSlot(Slot, HAlign_Center, VAlign_Center, FMargin(0.f)); });
			bOutSelfSized = false;
			return Scale;
		}

		if (V.Style.NeedsMaterial())
		{
			UDwoCanvasShape* Shape = Make<UDwoCanvasShape>(C, N.Id, N.WidgetName, bLocked);
			if (!bLocked)
			{
				Shape->Style = V.Style;
				Shape->DesiredSize = Size;
				Shape->ShapeMaterial = DwoCanvas::GetDefaultShapeMaterial();
				Shape->SetVisibility(ESlateVisibility::HitTestInvisible);
			}
			return Shape;
		}

		UImage* Image = Make<UImage>(C, N.Id, N.WidgetName, bLocked);
		if (!bLocked)
		{
			FSlateBrush Brush = V.Style.MakeSimpleBrush();
			Brush.ImageSize = Size;
			Image->SetBrush(Brush);
			Image->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		return Image;
	}

	/** Puts Inner on top of the node's background (fills / stroke / shadow / image fill). */
	UWidget* WrapBackground(FDocContext& C, const FNode& N, const FVisual& V, UWidget* Inner, const FMargin& InnerPadding)
	{
		const bool bComplex = V.Style.NeedsMaterial() && (V.bHasFill || V.bHasStroke || V.bHasShadow);
		const bool bPadding = InnerPadding.Left != 0.f || InnerPadding.Top != 0.f || InnerPadding.Right != 0.f || InnerPadding.Bottom != 0.f;
		bool bLocked = false;

		if (bComplex || V.ImageFill.IsValid())
		{
			UOverlay* Overlay = Make<UOverlay>(C, N.Id + TEXT("#bg"), N.WidgetName + TEXT("_Bg"), bLocked);
			if (!bLocked)
			{
				Overlay->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
			}

			if (V.ImageFill.IsValid())
			{
				bool bScaleBox = false;
				UImage* Image = Make<UImage>(C, N.Id + TEXT("#bgimage"), N.WidgetName + TEXT("_BgImage"), bLocked);
				const FSlateBrush Brush = MakeImageBrush(C, N, V, FVector2D(N.W, N.H), bScaleBox);
				if (!bLocked)
				{
					Image->SetBrush(Brush);
					Image->SetVisibility(ESlateVisibility::HitTestInvisible);
				}
				AddChild(C, Overlay, Image, [](UPanelSlot* Slot) { SetContentSlot(Slot, HAlign_Fill, VAlign_Fill, FMargin(0.f)); });
			}
			if (V.bHasFill || V.bHasStroke || V.bHasShadow)
			{
				FDwoCanvasShapeStyle Style = V.Style;
				if (V.ImageFill.IsValid())
				{
					Style.FillType = EDwoCanvasFillType::None; // image is the fill; keep stroke / shadow
				}
				UDwoCanvasShape* Shape = Make<UDwoCanvasShape>(C, N.Id + TEXT("#bgshape"), N.WidgetName + TEXT("_BgShape"), bLocked);
				if (!bLocked)
				{
					Shape->Style = Style;
					Shape->DesiredSize = FVector2D::ZeroVector;
					Shape->ShapeMaterial = DwoCanvas::GetDefaultShapeMaterial();
					Shape->SetVisibility(ESlateVisibility::HitTestInvisible);
				}
				AddChild(C, Overlay, Shape, [](UPanelSlot* Slot) { SetContentSlot(Slot, HAlign_Fill, VAlign_Fill, FMargin(0.f)); });
			}
			AddChild(C, Overlay, Inner, [&InnerPadding](UPanelSlot* Slot) { SetContentSlot(Slot, HAlign_Fill, VAlign_Fill, InnerPadding); });
			return Overlay;
		}

		if (V.IsVisible() || bPadding)
		{
			UBorder* Border = Make<UBorder>(C, N.Id + TEXT("#bg"), N.WidgetName + TEXT("_Bg"), bLocked);
			if (!bLocked)
			{
				if (V.IsVisible())
				{
					Border->SetBrush(V.Style.MakeSimpleBrush());
				}
				else
				{
					FSlateBrush NoDraw;
					NoDraw.DrawAs = ESlateBrushDrawType::NoDrawType;
					Border->SetBrush(NoDraw);
				}
				Border->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
			}
			AddChild(C, Border, Inner, [&InnerPadding](UPanelSlot* Slot) { SetContentSlot(Slot, HAlign_Fill, VAlign_Fill, InnerPadding); });
			return Border;
		}
		return Inner;
	}

	/** Frame / group / inline instance / scroll / switcher with children. */
	UWidget* BuildContainer(FDocContext& C, const FNode& N, const FString& Key, bool bNoBackground, bool& bOutSelfSized)
	{
		bOutSelfSized = false;
		const FVisual V = bNoBackground ? FVisual() : ReadVisual(C, N, N.J);
		const FString WidgetName = Key == N.Id ? N.WidgetName : N.WidgetName + TEXT("_Content");

		TArray<FJson> Flow;
		TArray<FJson> Absolute;
		if (const FJsonArray* Children = JArr(N.J, TEXT("children")))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Children)
			{
				const FJson Child = Value->AsObject();
				const bool bAbs = N.IsFlowLayout() && JStr(Child, TEXT("positioning"), TEXT("auto")) == TEXT("absolute");
				(bAbs ? Absolute : Flow).Add(Child);
			}
		}

		const bool bScroll = N.Role == EDwoCanvasRole::ScrollBox;
		const bool bSwitcher = N.Role == EDwoCanvasRole::Switcher;
		const bool bUseFrame = Settings.LayoutMode == EDwoCanvasLayoutMode::DwoCanvasFrame && !bScroll && !bSwitcher
			&& (N.LayoutMode == TEXT("horizontal") || N.LayoutMode == TEXT("vertical") || N.LayoutMode == TEXT("wrap"));

		bool bLocked = false;
		UPanelWidget* Panel = nullptr;
		EParentKind Kind = EParentKind::Canvas;
		FMargin WrapperPadding(0.f);

		if (bUseFrame)
		{
			UDwoCanvasFrame* Frame = Make<UDwoCanvasFrame>(C, Key, WidgetName, bLocked);
			if (!bLocked)
			{
				Frame->Direction = N.LayoutMode == TEXT("horizontal") ? EDwoCanvasFrameDirection::Horizontal
					: (N.LayoutMode == TEXT("wrap") ? EDwoCanvasFrameDirection::Wrap : EDwoCanvasFrameDirection::Vertical);
				Frame->Gap = N.MainAlign == TEXT("space-between") ? 0.f : N.Gap;
				Frame->CrossGap = N.CrossGap;
				Frame->Padding = N.Padding;
				Frame->MainAlign = N.MainAlign == TEXT("center") ? EDwoCanvasMainAlign::Center
					: N.MainAlign == TEXT("end") ? EDwoCanvasMainAlign::End
					: N.MainAlign == TEXT("space-between") ? EDwoCanvasMainAlign::SpaceBetween : EDwoCanvasMainAlign::Start;
				Frame->WidthOverride = N.IsFixedH() ? N.W : 0.f;
				Frame->HeightOverride = N.IsFixedV() ? N.H : 0.f;
				Frame->MinWidth = N.MinW;
				Frame->MaxWidth = N.MaxW;
				Frame->MinHeight = N.MinH;
				Frame->MaxHeight = N.MaxH;
				if (V.bHasFill || V.bHasStroke || V.bHasShadow)
				{
					Frame->bDrawBackground = true;
					Frame->Background = V.Style;
					if (V.Style.NeedsMaterial())
					{
						Frame->ShapeMaterial = DwoCanvas::GetDefaultShapeMaterial();
					}
				}
				if (N.bClip)
				{
					Frame->SetClipping(EWidgetClipping::ClipToBounds);
				}
			}
			Panel = Frame;
			Kind = EParentKind::Frame;
			bOutSelfSized = true;
		}
		else
		{
			UClass* PanelClass = UCanvasPanel::StaticClass();
			if (bScroll) { PanelClass = UScrollBox::StaticClass(); Kind = EParentKind::Scroll; }
			else if (bSwitcher) { PanelClass = UWidgetSwitcher::StaticClass(); Kind = EParentKind::Switcher; }
			else if (N.LayoutMode == TEXT("horizontal")) { PanelClass = UHorizontalBox::StaticClass(); Kind = EParentKind::HBox; }
			else if (N.LayoutMode == TEXT("vertical")) { PanelClass = UVerticalBox::StaticClass(); Kind = EParentKind::VBox; }
			else if (N.LayoutMode == TEXT("wrap")) { PanelClass = UWrapBox::StaticClass(); Kind = EParentKind::Wrap; WrapperPadding = N.Padding; }
			else if (N.LayoutMode == TEXT("grid")) { PanelClass = UGridPanel::StaticClass(); Kind = EParentKind::Grid; WrapperPadding = N.Padding; }

			Panel = Cast<UPanelWidget>(Make(C, Key, PanelClass, WidgetName, bLocked));
			if (!bLocked)
			{
				Panel->SetVisibility(bScroll ? ESlateVisibility::Visible : ESlateVisibility::SelfHitTestInvisible);
				if (N.bClip || bScroll)
				{
					Panel->SetClipping(EWidgetClipping::ClipToBounds);
				}
				if (UScrollBox* Scroll = Cast<UScrollBox>(Panel))
				{
					Scroll->SetOrientation(N.LayoutMode == TEXT("horizontal") ? Orient_Horizontal : Orient_Vertical);
				}
				if (UWrapBox* Wrap = Cast<UWrapBox>(Panel))
				{
					Wrap->SetInnerSlotPadding(FVector2D(N.Gap, N.CrossGap));
					SetPropertyByName<bool>(Wrap, TEXT("bExplicitWrapSize"), false);
				}
			}
		}

		// Children
		AddFlowChildren(C, N, Panel, Kind, Flow);

		UWidget* Content = Panel;

		// Absolutely positioned children of an auto-layout frame float above the flow
		if (Absolute.Num() > 0)
		{
			UOverlay* AbsOverlay = Make<UOverlay>(C, Key + TEXT("#abs"), WidgetName + TEXT("_Abs"), bLocked);
			if (!bLocked) { AbsOverlay->SetVisibility(ESlateVisibility::SelfHitTestInvisible); }
			UCanvasPanel* AbsCanvas = Make<UCanvasPanel>(C, Key + TEXT("#abscanvas"), WidgetName + TEXT("_AbsCanvas"), bLocked);
			if (!bLocked) { AbsCanvas->SetVisibility(ESlateVisibility::SelfHitTestInvisible); }
			AddChild(C, AbsOverlay, Content, [](UPanelSlot* Slot) { SetContentSlot(Slot, HAlign_Fill, VAlign_Fill, FMargin(0.f)); });
			AddChild(C, AbsOverlay, AbsCanvas, [](UPanelSlot* Slot) { SetContentSlot(Slot, HAlign_Fill, VAlign_Fill, FMargin(0.f)); });
			AddFlowChildren(C, N, AbsCanvas, EParentKind::Canvas, Absolute);
			Content = AbsOverlay;
		}

		if (bUseFrame)
		{
			// The frame draws its own background; only an image fill needs a layer behind it
			if (V.ImageFill.IsValid())
			{
				FVisual ImageOnly;
				ImageOnly.ImageFill = V.ImageFill;
				ImageOnly.Style.CornerRadii = V.Style.CornerRadii;
				ImageOnly.Style.FillType = EDwoCanvasFillType::None;
				return WrapBackground(C, N, ImageOnly, Content, FMargin(0.f));
			}
			return Content;
		}

		return bNoBackground ? (WrapperPadding.GetTotalSpaceAlong<Orient_Horizontal>() > 0.f || WrapperPadding.GetTotalSpaceAlong<Orient_Vertical>() > 0.f
			? WrapBackground(C, N, FVisual(), Content, WrapperPadding) : Content)
			: WrapBackground(C, N, V, Content, WrapperPadding);
	}

	/** Adds children to a panel with slot rules of its kind. Groups inside a canvas are flattened. */
	void AddFlowChildren(FDocContext& C, const FNode& Parent, UPanelWidget* Panel, EParentKind Kind, const TArray<FJson>& Children, FVector2D Offset = FVector2D::ZeroVector)
	{
		// Native boxes: space-between / center / end become spacers
		const bool bBox = Kind == EParentKind::HBox || Kind == EParentKind::VBox;
		const bool bHorizontal = Kind == EParentKind::HBox || (Kind == EParentKind::Scroll && Parent.LayoutMode == TEXT("horizontal"));
		bool bAnyFill = false;
		for (const FJson& Child : Children)
		{
			const FNode ChildNode = Parse(Child);
			bAnyFill |= bHorizontal ? ChildNode.SizingH == TEXT("fill") : ChildNode.SizingV == TEXT("fill");
		}
		const bool bSpaceBetween = bBox && Parent.MainAlign == TEXT("space-between") && !bAnyFill && Children.Num() > 1;
		const bool bLeadSpacer = bBox && !bAnyFill && (Parent.MainAlign == TEXT("center") || Parent.MainAlign == TEXT("end"));
		const bool bTrailSpacer = bBox && !bAnyFill && Parent.MainAlign == TEXT("center");

		auto AddSpacer = [&](const FString& Suffix)
		{
			bool bLocked = false;
			USpacer* Spacer = Make<USpacer>(C, Parent.Id + Suffix, Parent.WidgetName + TEXT("_Spacer"), bLocked);
			AddChild(C, Panel, Spacer, [](UPanelSlot* Slot)
			{
				if (UHorizontalBoxSlot* H = Cast<UHorizontalBoxSlot>(Slot)) { H->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); }
				if (UVerticalBoxSlot* V = Cast<UVerticalBoxSlot>(Slot)) { V->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); }
			});
		};

		if (bLeadSpacer)
		{
			AddSpacer(TEXT("#lead"));
		}

		int32 FlowIndex = 0;
		const int32 Count = Children.Num();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FJson& ChildJson = Children[Index];
			const FNode Child = Parse(ChildJson);

			// Flatten groups in a canvas (no extra panel; positions are already relative to the frame)
			if (Kind == EParentKind::Canvas && Child.Type == TEXT("group") && Child.Role == EDwoCanvasRole::Auto && !JObj(ChildJson, TEXT("bake")).IsValid())
			{
				TArray<FJson> GroupChildren;
				if (const FJsonArray* Array = JArr(ChildJson, TEXT("children")))
				{
					for (const TSharedPtr<FJsonValue>& Value : *Array)
					{
						GroupChildren.Add(Value->AsObject());
					}
				}
				AddFlowChildren(C, Parent, Panel, Kind, GroupChildren, Offset + FVector2D(Child.X, Child.Y));
				continue;
			}

			if (bSpaceBetween && FlowIndex > 0)
			{
				AddSpacer(FString::Printf(TEXT("#sp%d"), FlowIndex));
			}

			UWidget* Widget = BuildNode(C, ChildJson, Kind);
			if (!Widget)
			{
				continue;
			}

			const bool bFirst = FlowIndex == 0;
			const bool bLast = Index == Count - 1;
			const int32 ThisIndex = FlowIndex++;

			AddChild(C, Panel, Widget, [&](UPanelSlot* Slot)
			{
				ApplySlot(C, Parent, Child, Slot, Kind, ThisIndex, bFirst, bLast, bSpaceBetween, Offset);
			});
		}

		if (bTrailSpacer)
		{
			AddSpacer(TEXT("#trail"));
		}
	}

	void ApplySlot(FDocContext& C, const FNode& P, const FNode& N, UPanelSlot* Slot, EParentKind Kind, int32 Index, bool bFirst, bool bLast, bool bSpaceBetween, FVector2D Offset)
	{
		const bool bFillH = N.SizingH == TEXT("fill");
		const bool bFillV = N.SizingV == TEXT("fill");
		const float Gap = bSpaceBetween ? 0.f : P.Gap;

		switch (Kind)
		{
		case EParentKind::Canvas:
		{
			UCanvasPanelSlot* S = Cast<UCanvasPanelSlot>(Slot);
			if (!S) { break; }
			const float PW = FMath::Max(P.W, 1.f);
			const float PH = FMath::Max(P.H, 1.f);
			const float X = N.X + Offset.X;
			const float Y = N.Y + Offset.Y;

			struct FAxis { float AMin, AMax, Align, A, B; };
			auto Axis = [](const FString& Constraint, float Pos, float Size, float ParentSize) -> FAxis
			{
				if (Constraint == TEXT("max")) { return { 1.f, 1.f, 1.f, Pos + Size - ParentSize, Size }; }
				if (Constraint == TEXT("center")) { return { 0.5f, 0.5f, 0.5f, Pos + Size * 0.5f - ParentSize * 0.5f, Size }; }
				if (Constraint == TEXT("stretch")) { return { 0.f, 1.f, 0.f, Pos, ParentSize - (Pos + Size) }; }
				if (Constraint == TEXT("scale")) { return { Pos / ParentSize, (Pos + Size) / ParentSize, 0.f, 0.f, 0.f }; }
				return { 0.f, 0.f, 0.f, Pos, Size };
			};
			const FAxis H = Axis(N.ConstraintH, X, N.W, PW);
			const FAxis V = Axis(N.ConstraintV, Y, N.H, PH);
			S->SetAnchors(FAnchors(H.AMin, V.AMin, H.AMax, V.AMax));
			S->SetAlignment(FVector2D(H.Align, V.Align));
			S->SetOffsets(FMargin(H.A, V.A, H.B, V.B));
			S->SetZOrder(Index);
			// Hug text measures itself (font metrics differ slightly from the design tool)
			const FString AutoResize = JStr(JObj(N.J, TEXT("text")), TEXT("autoResize"));
			S->SetAutoSize(N.Type == TEXT("text") && AutoResize == TEXT("width") && N.ConstraintH != TEXT("stretch") && N.ConstraintH != TEXT("scale"));
			break;
		}
		case EParentKind::HBox:
		case EParentKind::VBox:
		case EParentKind::Scroll:
		{
			const bool bHorizontal = Kind == EParentKind::HBox || (Kind == EParentKind::Scroll && P.LayoutMode == TEXT("horizontal"));
			FMargin Pad;
			if (bHorizontal)
			{
				Pad = FMargin(bFirst ? P.Padding.Left : Gap, P.Padding.Top, bLast ? P.Padding.Right : 0.f, P.Padding.Bottom);
			}
			else
			{
				Pad = FMargin(P.Padding.Left, bFirst ? P.Padding.Top : Gap, P.Padding.Right, bLast ? P.Padding.Bottom : 0.f);
			}
			const bool bMainFill = bHorizontal ? bFillH : bFillV;
			const bool bCrossFill = (bHorizontal ? bFillV : bFillH) || P.CrossAlign == TEXT("stretch");
			const EHorizontalAlignment HA = bHorizontal ? (bMainFill ? HAlign_Fill : HAlign_Left) : (bCrossFill ? HAlign_Fill : ToHAlign(P.CrossAlign));
			const EVerticalAlignment VA = bHorizontal ? (bCrossFill ? VAlign_Fill : ToVAlign(P.CrossAlign)) : (bMainFill ? VAlign_Fill : VAlign_Top);

			if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Slot))
			{
				S->SetPadding(Pad);
				S->SetSize(FSlateChildSize(bMainFill ? ESlateSizeRule::Fill : ESlateSizeRule::Automatic));
				S->SetHorizontalAlignment(HA);
				S->SetVerticalAlignment(VA);
			}
			else if (UVerticalBoxSlot* S2 = Cast<UVerticalBoxSlot>(Slot))
			{
				S2->SetPadding(Pad);
				S2->SetSize(FSlateChildSize(bMainFill ? ESlateSizeRule::Fill : ESlateSizeRule::Automatic));
				S2->SetHorizontalAlignment(HA);
				S2->SetVerticalAlignment(VA);
			}
			else if (UScrollBoxSlot* S3 = Cast<UScrollBoxSlot>(Slot))
			{
				S3->SetPadding(Pad);
				S3->SetHorizontalAlignment(HA);
				S3->SetVerticalAlignment(VA);
			}
			break;
		}
		case EParentKind::Wrap:
		{
			if (UWrapBoxSlot* S = Cast<UWrapBoxSlot>(Slot))
			{
				S->SetFillEmptySpace(bFillH);
				S->SetHorizontalAlignment(bFillH ? HAlign_Fill : HAlign_Left);
				S->SetVerticalAlignment(bFillV ? VAlign_Fill : ToVAlign(P.CrossAlign));
			}
			break;
		}
		case EParentKind::Grid:
		{
			if (UGridSlot* S = Cast<UGridSlot>(Slot))
			{
				const int32 Columns = FMath::Max(P.Columns, 1);
				const int32 Row = Index / Columns;
				const int32 Column = Index % Columns;
				S->SetRow(Row);
				S->SetColumn(Column);
				S->SetPadding(FMargin(Column > 0 ? P.Gap : 0.f, Row > 0 ? P.CrossGap : 0.f, 0.f, 0.f));
				S->SetHorizontalAlignment(bFillH ? HAlign_Fill : HAlign_Left);
				S->SetVerticalAlignment(bFillV ? VAlign_Fill : VAlign_Top);
			}
			if (UGridPanel* Grid = Cast<UGridPanel>(Slot->Parent); Grid && C.Created.Contains(Grid))
			{
				const int32 Columns = FMath::Max(P.Columns, 1);
				for (int32 Column = 0; Column < Columns; ++Column)
				{
					Grid->SetColumnFill(Column, bFillH ? 1.f : 0.f);
				}
			}
			break;
		}
		case EParentKind::Frame:
		{
			if (UDwoCanvasFrameSlot* S = Cast<UDwoCanvasFrameSlot>(Slot))
			{
				const UDwoCanvasFrame* Frame = Cast<UDwoCanvasFrame>(Slot->Parent);
				const bool bHorizontal = Frame && Frame->Direction != EDwoCanvasFrameDirection::Vertical;
				const bool bMainFill = bHorizontal ? bFillH : bFillV;
				const bool bCrossFill = (bHorizontal ? bFillV : bFillH) || P.CrossAlign == TEXT("stretch");
				S->Size = FSlateChildSize(bMainFill ? ESlateSizeRule::Fill : ESlateSizeRule::Automatic);
				S->HorizontalAlignment = bHorizontal ? (bMainFill ? HAlign_Fill : HAlign_Left) : (bCrossFill ? HAlign_Fill : ToHAlign(P.CrossAlign));
				S->VerticalAlignment = bHorizontal ? (bCrossFill ? VAlign_Fill : ToVAlign(P.CrossAlign)) : (bMainFill ? VAlign_Fill : VAlign_Top);
				S->SynchronizeProperties();
			}
			break;
		}
		case EParentKind::Switcher:
		case EParentKind::Single:
		default:
			SetContentSlot(Slot, HAlign_Fill, VAlign_Fill, FMargin(0.f));
			break;
		}
	}

	UWidget* BuildText(FDocContext& C, const FNode& N, bool& bOutSelfSized)
	{
		const FJson T = JObj(N.J, TEXT("text"));
		bool bLocked = false;
		UTextBlock* Text = Make<UTextBlock>(C, N.Id, N.WidgetName, bLocked);
		const FString AutoResize = JStr(T, TEXT("autoResize"), TEXT("width"));

		if (!bLocked)
		{
			ApplyTextStyle(C, N, T, Text);
			Text->SetText(MakeText(C, N, T));
			Text->SetVisibility(ESlateVisibility::HitTestInvisible);

			if (AutoResize == TEXT("height") || AutoResize == TEXT("none"))
			{
				Text->SetAutoWrapText(true);
			}
			if (AutoResize == TEXT("truncate") || JNum(T, TEXT("maxLines")) > 0)
			{
				Text->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
				Text->SetClipping(EWidgetClipping::ClipToBounds);
			}
		}

		bOutSelfSized = AutoResize == TEXT("width");
		const int32 MaxLines = (int32)JNum(T, TEXT("maxLines"));
		if (MaxLines > 0)
		{
			// Height cap = lines x line height
			const float FontPx = (float)JNum(JObj(T, TEXT("font")), TEXT("size"), 16.0);
			const float LinePx = LineHeightPx(T, FontPx);
			bool bSizeLocked = false;
			USizeBox* Box = Make<USizeBox>(C, N.Id + TEXT("#lines"), N.WidgetName + TEXT("_Lines"), bSizeLocked);
			if (!bSizeLocked)
			{
				Box->SetMaxDesiredHeight(LinePx * MaxLines);
				Box->SetVisibility(ESlateVisibility::HitTestInvisible);
			}
			AddChild(C, Box, Text, [](UPanelSlot* Slot) { SetContentSlot(Slot, HAlign_Fill, VAlign_Fill, FMargin(0.f)); });
			return Box;
		}
		return Text;
	}

	float LineHeightPx(const FJson& T, float FontPx) const
	{
		const FJson LineHeight = JObj(T, TEXT("lineHeight"));
		const FString Unit = JStr(LineHeight, TEXT("unit"), TEXT("auto"));
		if (Unit == TEXT("px")) { return (float)JNum(LineHeight, TEXT("value"), FontPx * 1.2); }
		if (Unit == TEXT("percent")) { return FontPx * (float)JNum(LineHeight, TEXT("value"), 120.0) / 100.f; }
		// Figma's auto line height is the font's natural height rounded to whole pixels
		const FDwoCanvasFontRule* Rule = Settings.FindFontRule(JStr(JObj(T, TEXT("font")), TEXT("family")));
		return (float)FMath::RoundToInt(FontPx * (Rule ? Rule->LineHeightRatio : 1.17f));
	}

	/** Height of one line as Slate lays it out (0 when no font renderer is available, e.g. in a commandlet). */
	static float MeasureLineHeight(const FSlateFontInfo& FontInfo)
	{
		if (!FSlateApplication::IsInitialized() || !FSlateApplication::Get().GetRenderer())
		{
			return 0.f;
		}
		const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
		return (float)Measure->GetMaxCharacterHeight(FontInfo);
	}

	FText MakeText(const FDocContext& C, const FNode& N, const FJson& T) const
	{
		const FString Content = JStr(T, TEXT("content"));
		const FJson Loc = JObj(T, TEXT("loc"));
		const FString Table = JStr(Loc, TEXT("table"));
		const FString Key = JStr(Loc, TEXT("key"));
		if (!Table.IsEmpty() && !Key.IsEmpty())
		{
			if (FStringTableRegistry::Get().FindStringTable(FName(*Table)).IsValid())
			{
				return FText::FromStringTable(FName(*Table), Key);
			}
			const_cast<FImpl*>(this)->Warn(C, N, FString::Printf(TEXT("String table '%s' not found: using a literal text"), *Table));
		}
		// Stable key per document + node so translations survive re-import
		return FText::AsLocalizable_Advanced(*Settings.LocalizationNamespace, *FString::Printf(TEXT("%s.%s"), *C.DocName, *SanitizeName(N.Id)), Content);
	}

	FSlateFontInfo ResolveFont(const FDocContext& C, const FNode& N, const FJson& Font)
	{
		const FString Family = JStr(Font, TEXT("family"), TEXT("Roboto"));
		// style is informative in dwocanvas/2; weight / italic decide when it is absent
		const FString Style = JStr(Font, TEXT("style"));
		const float SizePx = (float)JNum(Font, TEXT("size"), 16.0);

		const FDwoCanvasFontRule* Rule = Settings.FindFontRule(Family);
		UObject* FontObject = Rule ? Rule->Font.TryLoad() : nullptr;
		if (!FontObject)
		{
			FontObject = Settings.DefaultFont.TryLoad();
			if (!Rule && !WarnedFonts.Contains(Family))
			{
				WarnedFonts.Add(Family);
				Warn(C, N, FString::Printf(TEXT("Font family '%s' has no font rule: using the default font (add it in Project Settings > DwoCanvas)"), *Family));
			}
		}

		// Weight / italic (dwocanvas/2); older documents only had a style name
		bool bItalic = JBool(Font, TEXT("italic"), false);
		int32 Weight = (int32)JNum(Font, TEXT("weight"), 0.0);
		if (Weight <= 0)
		{
			Weight = FDwoCanvasSchema::WeightFromStyle(Style, bItalic);
		}

		// Typeface: explicit style mapping, explicit weight mapping, exact name, then weight buckets
		FName Typeface = NAME_None;
		if (Rule)
		{
			if (const FName* Mapped = Rule->TypefaceByStyle.Find(Style))
			{
				Typeface = *Mapped;
			}
			else if (const FName* ByWeight = Rule->TypefaceByWeight.Find(Weight))
			{
				Typeface = *ByWeight;
			}
		}
		if (const UFont* UFontObject = Cast<UFont>(FontObject); UFontObject && Typeface.IsNone())
		{
			TArray<FName> Available;
			for (const FTypefaceEntry& Entry : UFontObject->GetCompositeFont()->DefaultTypeface.Fonts)
			{
				Available.Add(Entry.Name);
			}
			auto Pick = [&Available](const TCHAR* Name) { return Available.Contains(FName(Name)) ? FName(Name) : NAME_None; };
			if (!Style.IsEmpty())
			{
				Typeface = Pick(*Style);
			}
			if (Typeface.IsNone())
			{
				if (Weight >= 600) { Typeface = Pick(bItalic ? TEXT("Bold Italic") : TEXT("Bold")); if (Typeface.IsNone()) { Typeface = Pick(TEXT("Bold")); } }
				else if (Weight <= 300) { Typeface = Pick(TEXT("Light")); }
				else if (bItalic) { Typeface = Pick(TEXT("Italic")); }
				if (Typeface.IsNone()) { Typeface = Pick(TEXT("Regular")); }
				const FString WarnKey = FString::Printf(TEXT("%s/%d/%d"), *Family, Weight, bItalic ? 1 : 0);
				const bool bNatural = Style.IsEmpty() && ((Weight == 400 && Typeface == FName(TEXT("Regular"))) || (Weight >= 600 && Typeface.ToString().StartsWith(TEXT("Bold")))
					|| (Weight <= 300 && Typeface == FName(TEXT("Light"))));
				if (!WarnedFonts.Contains(WarnKey) && Typeface.ToString() != Style && !bNatural)
				{
					WarnedFonts.Add(WarnKey);
					Info(C, N, FString::Printf(TEXT("Font '%s' weight %d%s mapped to typeface '%s'"), *Family, Weight, bItalic ? TEXT(" italic") : TEXT(""), *Typeface.ToString()));
				}
			}
		}

		// Fractional points: rounding to whole points made 9 / 11 / 13 px text 4-3% too large (7 / 8 / 10 pt)
		FSlateFontInfo Info(FontObject, FMath::Max(1.f, SizePx * Settings.FontSizeScale), Typeface);
		return Info;
	}

	void ApplyTextStyle(FDocContext& C, const FNode& N, const FJson& T, UTextBlock* Text)
	{
		const FJson Font = JObj(T, TEXT("font"));
		FSlateFontInfo FontInfo = ResolveFont(C, N, Font);
		const float SizePx = (float)JNum(Font, TEXT("size"), 16.0);

		const FJson Spacing = JObj(T, TEXT("letterSpacing"));
		if (Spacing.IsValid())
		{
			const double Value = JNum(Spacing, TEXT("value"));
			const double SpacingPx = JStr(Spacing, TEXT("unit"), TEXT("px")) == TEXT("percent") ? Value / 100.0 * SizePx : Value;
			// Slate adds LetterSpacing * Size(pt) / 1000 per glyph and truncates it to whole pixels
			// (SlateTextShaper). Aim at the nearest whole pixel and land on or just above it.
			const double PointSize = FMath::Max((double)FontInfo.Size, 0.01);
			const double TargetPx = FMath::RoundToDouble(SpacingPx);
			FontInfo.LetterSpacing = TargetPx == 0.0 ? 0 : (int32)(FMath::Sign(TargetPx) * FMath::CeilToDouble(FMath::Abs(TargetPx) * 1000.0 / PointSize));
		}
		Text->SetFont(FontInfo);

		// Line height. Slate's line = the font's ascender + descender rounded by FreeType (e.g. 20 px for Roboto 16 px),
		// Figma's auto line = round(size x ratio) (19 px). Scale Slate's measured line to the design's line so stacked
		// texts don't drift a pixel per line.
		const float SlateLinePx = MeasureLineHeight(FontInfo);
		const float DesignLinePx = LineHeightPx(T, SizePx);
		if (SlateLinePx > 0.f && FMath::Abs(DesignLinePx - SlateLinePx) > 0.01f)
		{
			Text->SetLineHeightPercentage(DesignLinePx / SlateLinePx);
		}
		else if (SlateLinePx <= 0.f && JStr(JObj(T, TEXT("lineHeight")), TEXT("unit"), TEXT("auto")) != TEXT("auto"))
		{
			// No font renderer (commandlet): estimate Slate's line from the ratio
			const FDwoCanvasFontRule* Rule = Settings.FindFontRule(JStr(Font, TEXT("family")));
			Text->SetLineHeightPercentage(DesignLinePx / FMath::Max(SizePx * (Rule ? Rule->LineHeightRatio : 1.17f), 1.f));
		}

		// Color: text.color, else top solid fill
		FLinearColor Color = JColor(T, TEXT("color"), FLinearColor::Transparent);
		if (Color.A <= 0.f)
		{
			const TArray<FJson> Fills = VisibleFills(N.J);
			if (Fills.Num() > 0)
			{
				if (JStr(Fills.Last(), TEXT("type"), TEXT("solid")) != TEXT("solid"))
				{
					Warn(C, N, TEXT("Gradient / image text fill is not supported live: using the first color (bake the text for an exact look)"));
				}
				Color = ResolveColor(Fills.Last());
			}
			else
			{
				Color = FLinearColor::White;
			}
		}
		Text->SetColorAndOpacity(FSlateColor(Color));

		const FString Align = JStr(T, TEXT("align"), TEXT("left"));
		Text->SetJustification(Align == TEXT("center") ? ETextJustify::Center : (Align == TEXT("right") ? ETextJustify::Right : ETextJustify::Left));
		if (Align == TEXT("justify"))
		{
			Info(C, N, TEXT("Justified text is shown left aligned"));
		}

		const FString Case = JStr(T, TEXT("case"), TEXT("none"));
		if (Case == TEXT("upper")) { Text->SetTextTransformPolicy(ETextTransformPolicy::ToUpper); }
		else if (Case == TEXT("lower")) { Text->SetTextTransformPolicy(ETextTransformPolicy::ToLower); }
		else if (Case != TEXT("none")) { Info(C, N, FString::Printf(TEXT("Text case '%s' is not supported"), *Case)); }

		const FString Decoration = JStr(T, TEXT("decoration"), TEXT("none"));
		if (Decoration != TEXT("none"))
		{
			Warn(C, N, FString::Printf(TEXT("Text decoration '%s' is not applied (needs a RichTextBlock style)"), *Decoration));
		}

		if (const FJsonArray* Runs = JArr(T, TEXT("runs")); Runs && Runs->Num() > 1)
		{
			Warn(C, N, FString::Printf(TEXT("Text has %d style runs: flattened to the first style (use a RichTextBlock for mixed styles)"), Runs->Num()));
		}

		// Drop shadow on text -> text shadow. Without one, zero the offset: UTextBlock defaults to (1, 1) and Slate adds the
		// offset to the desired size even when the shadow is transparent (every text came out 1 px wider and taller).
		Text->SetShadowOffset(FVector2D::ZeroVector);
		Text->SetShadowColorAndOpacity(FLinearColor::Transparent);
		if (const FJsonArray* Effects = JArr(N.J, TEXT("effects")))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Effects)
			{
				const FJson Effect = Value->AsObject();
				if (JStr(Effect, TEXT("type")) == TEXT("drop-shadow") && JBool(Effect, TEXT("visible"), true))
				{
					const TArray<double> Offset = JNums(Effect, TEXT("offset"));
					Text->SetShadowOffset(Offset.Num() >= 2 ? FVector2D(Offset[0], Offset[1]) : FVector2D(1.f, 1.f));
					Text->SetShadowColorAndOpacity(JColor(Effect, TEXT("color"), FLinearColor(0, 0, 0, 0.5f)));
					if (JNum(Effect, TEXT("blur")) > 0.0)
					{
						Info(C, N, TEXT("Text shadow blur is ignored (UMG text shadows are hard-edged)"));
					}
					break;
				}
			}
		}
	}

	UWidget* BuildInstance(FDocContext& C, const FNode& N, const FJson& Component, bool& bOutSelfSized)
	{
		const FString ComponentId = JStr(Component, TEXT("ref"));
		const FString ComponentName = JStr(Component, TEXT("name"));
		UClass* Class = Settings.FindComponentOverride(ComponentName);
		if (!Class)
		{
			Class = GetComponentClass(ComponentId);
		}
		if (!Class)
		{
			Error(C, N, FString::Printf(TEXT("Component '%s' (%s) could not be imported"), *ComponentName, *ComponentId));
			return nullptr;
		}

		if (JBool(Component, TEXT("overridden")))
		{
			Info(C, N, TEXT("Instance overrides are not applied when referencing the component (InstancePolicy = AlwaysReference)"));
		}

		bool bLocked = false;
		UWidget* Widget = Make(C, N.Id, Class, N.WidgetName, bLocked);

		// The component WBP already has its design size; only a resized instance needs a SizeBox
		const FJson* Doc = ComponentDocs.Find(ComponentId);
		const FNode Root = Doc ? Parse(JObj(*Doc, TEXT("root"))) : FNode();
		bOutSelfSized = Doc && FMath::IsNearlyEqual(Root.W, N.W, 0.5f) && FMath::IsNearlyEqual(Root.H, N.H, 0.5f);
		return Widget;
	}

	UWidget* BuildButton(FDocContext& C, const FNode& N)
	{
		bool bLocked = false;
		UButton* Button = Make<UButton>(C, N.Id, N.WidgetName, bLocked);
		const FVector2D Size(N.W, N.H);

		if (!bLocked)
		{
			FButtonStyle Style = Button->GetStyle();
			Style.Normal = MakeStateBrush(C, N, N.J, Size);
			const FJson States = JObj(N.J, TEXT("states"));
			const FJson Hovered = JObj(States, TEXT("hovered"));
			const FJson Pressed = JObj(States, TEXT("pressed"));
			const FJson Disabled = JObj(States, TEXT("disabled"));
			Style.Hovered = Hovered.IsValid() ? MakeStateBrush(C, N, Hovered, Size) : Style.Normal;
			Style.Pressed = Pressed.IsValid() ? MakeStateBrush(C, N, Pressed, Size) : Style.Hovered;
			if (Disabled.IsValid())
			{
				Style.Disabled = MakeStateBrush(C, N, Disabled, Size);
			}
			else
			{
				Style.Disabled = Style.Normal;
				Style.Disabled.TintColor = FSlateColor(Style.Normal.TintColor.GetSpecifiedColor() * FLinearColor(1.f, 1.f, 1.f, 0.4f));
			}
			if (!Hovered.IsValid())
			{
				Info(C, N, TEXT("No hovered state in the design: hover looks like normal"));
			}
			Style.NormalPadding = FMargin(0.f);
			Style.PressedPadding = FMargin(0.f);
			Button->SetStyle(Style);
			Button->SetBackgroundColor(FLinearColor::White);
			Button->SetVisibility(ESlateVisibility::Visible);
		}

		const FJsonArray* Children = JArr(N.J, TEXT("children"));
		if (!Children || Children->Num() == 0)
		{
			return Button;
		}

		// One child: straight into the button slot. Several: an inner layout without background.
		if (Children->Num() == 1)
		{
			const FJson ChildJson = (*Children)[0]->AsObject();
			if (UWidget* Child = BuildNode(C, ChildJson, EParentKind::Single))
			{
				const FNode ChildNode = Parse(ChildJson);
				AddChild(C, Button, Child, [&](UPanelSlot* Slot)
				{
					const bool bCanvas = !N.IsFlowLayout();
					EHorizontalAlignment HA = HAlign_Center;
					EVerticalAlignment VA = VAlign_Center;
					FMargin Pad = N.Padding;
					if (bCanvas)
					{
						// Position from the design box
						HA = HAlign_Left;
						VA = VAlign_Top;
						Pad = FMargin(ChildNode.X, ChildNode.Y, 0.f, 0.f);
					}
					else
					{
						const bool bHorizontal = N.LayoutMode == TEXT("horizontal");
						const FString& MainAlign = N.MainAlign;
						HA = ChildNode.SizingH == TEXT("fill") ? HAlign_Fill : ToHAlign(bHorizontal ? (MainAlign == TEXT("space-between") ? FString(TEXT("start")) : MainAlign) : N.CrossAlign);
						VA = ChildNode.SizingV == TEXT("fill") ? VAlign_Fill : ToVAlign(bHorizontal ? N.CrossAlign : MainAlign);
					}
					SetContentSlot(Slot, HA, VA, Pad);
				});
			}
			return Button;
		}

		bool bSelfSized = false;
		UWidget* Content = BuildContainer(C, N, N.Id + TEXT("#content"), true, bSelfSized);
		AddChild(C, Button, Content, [](UPanelSlot* Slot) { SetContentSlot(Slot, HAlign_Fill, VAlign_Fill, FMargin(0.f)); });
		return Button;
	}

	/** Text contents under a design node, keyed by the widget name each text gets. */
	void CollectTexts(const FJson& J, TMap<FName, FString>& Out) const
	{
		if (!J.IsValid())
		{
			return;
		}
		if (JStr(J, TEXT("type")) == TEXT("text"))
		{
			Out.Add(FName(*Parse(J).WidgetName), JStr(JObj(J, TEXT("text")), TEXT("content")));
		}
		if (const FJsonArray* Kids = JArr(J, TEXT("children")))
		{
			for (const TSharedPtr<FJsonValue>& Kid : *Kids)
			{
				CollectTexts(Kid->AsObject(), Out);
			}
		}
	}

	UWidget* BuildList(FDocContext& C, const FNode& N)
	{
		const bool bTile = N.Role == EDwoCanvasRole::TileView;
		bool bLocked = false;
		UListViewBase* List = Cast<UListViewBase>(Make(C, N.Id, bTile ? UTileView::StaticClass() : UListView::StaticClass(), N.WidgetName, bLocked));

		const FJsonArray* Children = JArr(N.J, TEXT("children"));
		const int32 Count = Children ? Children->Num() : 0;
		TArray<TMap<FName, FString>>& PreviewRows = C.ListPreview.FindOrAdd(List->GetFName());
		PreviewRows.Reset();
		for (int32 Index = 0; Children && Index < Count; ++Index)
		{
			CollectTexts((*Children)[Index]->AsObject(), PreviewRows.AddDefaulted_GetRef());
		}
		UClass* EntryClass = nullptr;
		FNode EntryNode;
		if (Count > 0)
		{
			const FJson First = (*Children)[0]->AsObject();
			EntryNode = Parse(First);
			const FJson Component = JObj(First, TEXT("component"));
			if (Component.IsValid())
			{
				EntryClass = Settings.FindComponentOverride(JStr(Component, TEXT("name")));
				if (!EntryClass)
				{
					EntryClass = GetComponentClass(JStr(Component, TEXT("ref")));
				}
			}
		}
		if (!EntryClass)
		{
			Warn(C, N, TEXT("List has no component instance as its first child: set the Entry Widget Class by hand"));
		}

		if (!bLocked)
		{
			if (EntryClass)
			{
				SetClassPropertyByName(List, TEXT("EntryWidgetClass"), EntryClass);
			}
			SetPropertyByName<int32>(List, TEXT("NumDesignerPreviewEntries"), FMath::Max(Count, 1));
			List->SetVisibility(ESlateVisibility::Visible);
			if (N.bClip)
			{
				List->SetClipping(EWidgetClipping::ClipToBounds);
			}

			if (UTileView* Tile = Cast<UTileView>(List))
			{
				// Figma wrap layout: fixed-size entries packed from the left with gap / cross gap between them.
				// TileView's default EvenlyDistributed spreads (and stretches) entries over the whole width instead.
				// Left aligned + spacing outside the entry size keeps each entry at its design size.
				const float RowGap = N.CrossGap > 0.f ? N.CrossGap : N.Gap;
				SetPropertyByName<EListItemAlignment>(Tile, TEXT("TileAlignment"), EListItemAlignment::LeftAligned);
				SetPropertyByName<bool>(Tile, TEXT("bEntrySizeIncludesEntrySpacing"), false);
				Tile->SetEntryWidth(EntryNode.W);
				Tile->SetEntryHeight(EntryNode.H);
				Tile->SetHorizontalEntrySpacing(N.Gap);
				Tile->SetVerticalEntrySpacing(RowGap);
			}
			else if (UListView* ListView = Cast<UListView>(List))
			{
				const bool bHorizontal = N.LayoutMode == TEXT("horizontal");
				SetPropertyByName<TEnumAsByte<EOrientation>>(ListView, TEXT("Orientation"), bHorizontal ? Orient_Horizontal : Orient_Vertical);
				if (bHorizontal) { ListView->SetHorizontalEntrySpacing(N.Gap); }
				else { ListView->SetVerticalEntrySpacing(N.Gap); }
			}
		}
		Info(C, N, FString::Printf(TEXT("%s with %d preview entries; items are set at runtime (SetListItems)"), bTile ? TEXT("TileView") : TEXT("ListView"), Count));

		// STileView puts half of the entry spacing on every side of each tile: take that out of the container padding
		// so the first tile sits at the design's padding (Figma has gaps only between items).
		FMargin Padding = N.Padding;
		if (bTile)
		{
			const float HalfGap = N.Gap * 0.5f;
			const float HalfRowGap = (N.CrossGap > 0.f ? N.CrossGap : N.Gap) * 0.5f;
			Padding.Left = FMath::Max(0.f, Padding.Left - HalfGap);
			Padding.Right = FMath::Max(0.f, Padding.Right - HalfGap);
			Padding.Top = FMath::Max(0.f, Padding.Top - HalfRowGap);
			Padding.Bottom = FMath::Max(0.f, Padding.Bottom - HalfRowGap);
		}
		const FVisual V = ReadVisual(C, N, N.J);
		return WrapBackground(C, N, V, List, Padding);
	}

	UWidget* BuildControl(FDocContext& C, const FNode& N, bool& bOutSelfSized)
	{
		bool bLocked = false;
		bOutSelfSized = false;
		switch (N.Role)
		{
		case EDwoCanvasRole::Spacer:
		{
			USpacer* Spacer = Make<USpacer>(C, N.Id, N.WidgetName, bLocked);
			if (!bLocked) { Spacer->SetSize(FVector2D(N.W, N.H)); }
			bOutSelfSized = true;
			return Spacer;
		}
		case EDwoCanvasRole::NamedSlot:
		{
			UNamedSlot* Slot = Make<UNamedSlot>(C, N.Id, N.WidgetName, bLocked);
			if (JArr(N.J, TEXT("children")) && JArr(N.J, TEXT("children"))->Num() > 0)
			{
				Info(C, N, TEXT("Named slot: design content is placeholder only and not imported"));
			}
			return Slot;
		}
		case EDwoCanvasRole::TextInput:
		{
			UEditableTextBox* Input = Make<UEditableTextBox>(C, N.Id, N.WidgetName, bLocked);
			if (!bLocked)
			{
				// Hint = first text inside the design
				if (const FJson TextNode = FindFirstText(N.J))
				{
					const FJson T = JObj(TextNode, TEXT("text"));
					const FNode TextInfo = Parse(TextNode);
					Input->SetHintText(MakeText(C, TextInfo, T));
					FEditableTextBoxStyle Style = Input->GetWidgetStyle();
					Style.SetFont(ResolveFont(C, TextInfo, JObj(T, TEXT("font"))));
					const FVisual V = ReadVisual(C, N, N.J);
					if (V.bHasFill || V.bHasStroke)
					{
						const FSlateBrush Brush = V.Style.MakeSimpleBrush();
						Style.SetBackgroundImageNormal(Brush);
						Style.SetBackgroundImageHovered(Brush);
						Style.SetBackgroundImageFocused(Brush);
					}
					Input->SetWidgetStyle(Style);
				}
			}
			return Input;
		}
		case EDwoCanvasRole::CheckBox:
		{
			UCheckBox* Check = Make<UCheckBox>(C, N.Id, N.WidgetName, bLocked);
			Warn(C, N, TEXT("CheckBox uses the default style: map it to a component override (e.g. a CommonUI button) for the design look"));
			return Check;
		}
		case EDwoCanvasRole::Slider:
		{
			USlider* Slider = Make<USlider>(C, N.Id, N.WidgetName, bLocked);
			Warn(C, N, TEXT("Slider uses the default style"));
			return Slider;
		}
		case EDwoCanvasRole::ProgressBar:
		default:
		{
			UProgressBar* Bar = Make<UProgressBar>(C, N.Id, N.WidgetName, bLocked);
			if (!bLocked)
			{
				const FVisual V = ReadVisual(C, N, N.J);
				FProgressBarStyle Style = Bar->GetWidgetStyle();
				if (V.bHasFill)
				{
					Style.BackgroundImage = V.Style.MakeSimpleBrush();
				}
				Bar->SetWidgetStyle(Style);
				Bar->SetPercent(0.5f);
			}
			Info(C, N, TEXT("ProgressBar: background from the design, fill uses the default brush"));
			return Bar;
		}
		}
	}

	static FJson FindFirstText(const FJson& Node)
	{
		if (const FJsonArray* Children = JArr(Node, TEXT("children")))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Children)
			{
				const FJson Child = Value->AsObject();
				if (JStr(Child, TEXT("type")) == TEXT("text"))
				{
					return Child;
				}
				if (FJson Found = FindFirstText(Child))
				{
					return Found;
				}
			}
		}
		return FJson();
	}

	// ----------------------------------------------------------------------------------------------------------------
	// Documents

	UClass* GetComponentClass(const FString& ComponentId)
	{
		if (UClass** Cached = ComponentClasses.Find(ComponentId))
		{
			return *Cached;
		}
		const FJson* Doc = ComponentDocs.Find(ComponentId);
		if (!Doc)
		{
			return nullptr;
		}
		if (InProgress.Contains(ComponentId))
		{
			Report->Add(EDwoCanvasSeverity::Error, SourceName, ComponentId, TEXT(""), TEXT("Component references itself"));
			return nullptr;
		}

		UWidgetBlueprint* Blueprint = ImportDocument(*Doc, true);
		UClass* Class = Blueprint ? Blueprint->GeneratedClass.Get() : nullptr;
		ComponentClasses.Add(ComponentId, Class);
		return Class;
	}

	UWidgetBlueprint* ImportDocument(const FJson& Doc, bool bComponent)
	{
		FDocContext C;
		C.DocId = JStr(Doc, TEXT("id"));
		C.DocName = SanitizeName(JStr(Doc, TEXT("name"), TEXT("Widget")));
		C.bComponent = bComponent;

		const FJson RootJson = JObj(Doc, TEXT("root"));
		if (!RootJson.IsValid())
		{
			Report->Add(EDwoCanvasSeverity::Error, C.DocName, C.DocId, TEXT(""), TEXT("Document has no root node"));
			return nullptr;
		}

		InProgress.Add(C.DocId);
		ON_SCOPE_EXIT { InProgress.Remove(C.DocId); };

		// Asset
		FString Target = JStr(Doc, TEXT("target"));
		if (bDraft)
		{
			// Inferred drafts never overwrite: each import gets a new blueprint in the draft folder
			const FString Base = Settings.DraftFolder / (TEXT("WBP_") + C.DocName);
			Target = Base;
			for (int32 Suffix = 2; FPackageName::DoesPackageExist(Target) || FindObject<UPackage>(nullptr, *Target); ++Suffix)
			{
				Target = FString::Printf(TEXT("%s_%d"), *Base, Suffix);
			}
		}
		else if (Target.IsEmpty())
		{
			Target = (bComponent ? Settings.ComponentFolder : Settings.WidgetFolder) / (TEXT("WBP_") + C.DocName);
		}
		C.AssetPath = Target;

		UClass* ParentClass = nullptr;
		const FString ParentPath = JStr(Doc, TEXT("parentClass"));
		if (!ParentPath.IsEmpty())
		{
			ParentClass = TSoftClassPtr<UUserWidget>(FSoftObjectPath(ParentPath)).LoadSynchronous();
			if (!ParentClass)
			{
				Report->Add(EDwoCanvasSeverity::Error, C.DocName, C.DocId, TEXT(""), FString::Printf(TEXT("Parent class '%s' not found"), *ParentPath));
				return nullptr;
			}
		}
		if (!ParentClass)
		{
			ParentClass = Settings.DefaultParentClass.LoadSynchronous();
		}
		if (!ParentClass)
		{
			ParentClass = UUserWidget::StaticClass();
		}

		bool bCreated = false;
		FString AssetError;
		C.Blueprint = FDwoCanvasWidgetWriter::LoadOrCreate(Target, ParentClass, bCreated, AssetError);
		if (!C.Blueprint)
		{
			Report->Add(EDwoCanvasSeverity::Error, C.DocName, C.DocId, TEXT(""), AssetError);
			return nullptr;
		}
		C.Tree = C.Blueprint->WidgetTree;
		KeepAlive.Add(C.Blueprint);
		if (!bCreated && C.Blueprint->ParentClass != ParentClass && !ParentPath.IsEmpty())
		{
			Report->Add(EDwoCanvasSeverity::Warning, C.DocName, C.DocId, TEXT(""),
				FString::Printf(TEXT("Existing blueprint has parent %s, design asks for %s: not reparented"), *GetNameSafe(C.Blueprint->ParentClass), *ParentClass->GetName()));
		}

		// List entries must implement IUserObjectListEntry
		if (bComponent && ListEntryComponents.Contains(C.DocId)
			&& !FBlueprintEditorUtils::ImplementsInterface(C.Blueprint, true, UUserObjectListEntry::StaticClass()))
		{
			FBlueprintEditorUtils::ImplementNewInterface(C.Blueprint, UUserObjectListEntry::StaticClass()->GetClassPathName());
			Report->Add(EDwoCanvasSeverity::Info, C.DocName, C.DocId, TEXT(""), TEXT("Added the UserObjectListEntry interface (used as a list entry)"));
		}

		const UClass* NativeParent = FDwoCanvasWidgetWriter::GetNativeParent(C.Blueprint);
		C.BindAll = FDwoCanvasWidgetWriter::GetBindWidgetProperties(NativeParent, true);
		C.BindRequired = FDwoCanvasWidgetWriter::GetBindWidgetProperties(NativeParent, false);

		// Record
		const bool bHadRecord = !bDraft && C.OldRecord.Load(Target);
		C.NewRecord.DocumentId = C.DocId;
		C.NewRecord.AssetPath = Target;
		if (!bHadRecord && FDwoCanvasWidgetWriter::HasDesignerContent(C.Blueprint) && !Options.bResetOverrides)
		{
			Report->Add(EDwoCanvasSeverity::Error, C.DocName, C.DocId, TEXT(""),
				FString::Printf(TEXT("%s already has a designer layout that DwoCanvas did not create: not touched (import with Reset Overrides to replace it)"), *Target));
			return C.Blueprint;
		}
		if (!bHadRecord && Options.bResetOverrides)
		{
			FDwoCanvasWidgetWriter::ClearTree(C.Blueprint);
		}

		FDwoCanvasWidgetWriter::CloseEditors(C.Blueprint);
		C.Blueprint->Modify();
		C.Tree->Modify();

		CollectNodeIds(RootJson, C.NewNodeIds);
		PrepareTree(C);

		// Build
		const EParentKind RootKind = bComponent ? EParentKind::RootComponent : EParentKind::Root;
		UWidget* Root = BuildNode(C, RootJson, RootKind);
		if (Root && bComponent)
		{
			// Components keep their design size
			const FNode RootNode = Parse(RootJson);
			if (!Cast<USizeBox>(Root) && !Cast<UDwoCanvasFrame>(Root) && (RootNode.W > 0.f || RootNode.H > 0.f))
			{
				bool bLocked = false;
				USizeBox* Size = Make<USizeBox>(C, RootNode.Id + TEXT("#rootsize"), RootNode.WidgetName + TEXT("_Size"), bLocked);
				if (!bLocked)
				{
					if (RootNode.IsFixedH()) { Size->SetWidthOverride(RootNode.W); }
					if (RootNode.IsFixedV()) { Size->SetHeightOverride(RootNode.H); }
					Size->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
				}
				AddChild(C, Size, Root, [](UPanelSlot* Slot) { SetContentSlot(Slot, HAlign_Fill, VAlign_Fill, FMargin(0.f)); });
				Root = Size;
			}
		}
		C.Tree->RootWidget = Root;

		FinishTree(C);

		// Variables: bound names + role / $ rules; widgets edited or added in UE keep their setting
		for (UWidget* Widget : C.Created)
		{
			Widget->bIsVariable = C.VariableNames.Contains(Widget->GetFName()) || C.BindAll.Contains(Widget->GetFName());
		}

		ValidateBindings(C);
		SyncVariableGuids(C);

		FString Error;
		if (!FDwoCanvasWidgetWriter::Compile(C.Blueprint, Error))
		{
			Report->Add(EDwoCanvasSeverity::Error, C.DocName, C.DocId, TEXT(""), Error);
		}

		// Designer preview size
		if (UUserWidget* Defaults = C.Blueprint->GeneratedClass ? C.Blueprint->GeneratedClass->GetDefaultObject<UUserWidget>() : nullptr)
		{
			const FNode RootNode = Parse(RootJson);
			Defaults->DesignTimeSize = FVector2D(RootNode.W, RootNode.H);
			Defaults->DesignSizeMode = bComponent ? EDesignPreviewSizeMode::Desired : EDesignPreviewSizeMode::Custom;
			Defaults->MarkPackageDirty();
		}

		if (!FDwoCanvasWidgetWriter::Save(C.Blueprint, Error))
		{
			Report->Add(EDwoCanvasSeverity::Error, C.DocName, C.DocId, TEXT(""), Error);
		}

		// Hashes after compile + save: this is the state the next import compares against
		for (auto& Pair : C.NewRecord.Entries)
		{
			UWidget* Widget = FindObjectFast<UWidget>(C.Tree, Pair.Value.WidgetName);
			Pair.Value.Hash = FDwoCanvasImportRecord::ComputeWidgetHash(Widget);
		}
		if (!bDraft)
		{
			C.NewRecord.Save(Target);
		}
		Report->WrittenAssets.Add(Target);
		{
			FDwoCanvasReport::FDocumentResult& Result = Report->FindOrAddDocument(C.DocName);
			Result.AssetPath = Target;
			Result.bComponent = bComponent;
			Result.bDraft = bDraft;
		}

		// Visual check against the exported preview
		const FString PreviewId = JStr(Doc, TEXT("preview"));
		if (Options.bVisualDiff && !PreviewId.IsEmpty())
		{
			TArray<uint8> Bytes;
			FJson Info;
			if (LoadImageBytes(PreviewId, Bytes, Info))
			{
				const FNode RootNode = Parse(RootJson);
				FDwoCanvasVisualDiff::Compare(C.Blueprint, Bytes, FIntPoint(FMath::RoundToInt(RootNode.W), FMath::RoundToInt(RootNode.H)),
					JNum(Info, TEXT("scale"), 1.0), C.DocName, *Report, &C.ListPreview);
			}
		}

		return C.Blueprint;
	}

	/** Splits the existing tree into: regenerate, keep (edited / referenced), user-added. Detaches everything. */
	void PrepareTree(FDocContext& C)
	{
		for (const auto& Pair : C.OldRecord.Entries)
		{
			C.ReservedNames.Add(Pair.Value.WidgetName);
		}

		TArray<UWidget*> All;
		C.Tree->GetAllWidgets(All);

		TArray<UWidget*> Generated;
		for (UWidget* Widget : All)
		{
			KeepAlive.Add(Widget);
			const FString* Key = C.OldRecord.FindKeyByWidget(Widget->GetFName());
			if (!Key)
			{
				// Added in UE. Re-attach later only if its parent is a generated widget (nested user content stays put).
				UPanelWidget* Parent = Widget->GetParent();
				const bool bParentGenerated = Parent && C.OldRecord.FindKeyByWidget(Parent->GetFName());
				if (bParentGenerated || Widget == C.Tree->RootWidget)
				{
					FDocContext::FUserWidget Info;
					Info.Widget = Widget;
					Info.ParentName = Parent ? Parent->GetFName() : NAME_None;
					Info.Index = Parent ? Parent->GetChildIndex(Widget) : 0;
					Info.Slot = SnapshotSlot(Widget->Slot);
					C.UserAdded.Add(Info);
				}
				continue;
			}

			Generated.Add(Widget);
			const FDwoCanvasRecordEntry& Entry = C.OldRecord.Entries[*Key];
			const bool bEdited = Entry.bLocked || FDwoCanvasImportRecord::IsEdited(Widget, Entry);

			FString NodeId = *Key;
			int32 HashIndex = INDEX_NONE;
			if (NodeId.FindChar(TEXT('#'), HashIndex))
			{
				NodeId.LeftInline(HashIndex);
			}
			const bool bNodeGone = !C.NewNodeIds.Contains(NodeId);
			const bool bReferenced = bNodeGone && IsReferenced(C, Widget->GetFName());

			if ((bEdited && !Options.bResetOverrides) || bReferenced || (bNodeGone && !Settings.bRemoveUntouchedOrphans))
			{
				C.Locked.Add(Widget->GetFName(), Widget);
				C.LockedSlots.Add(Widget, SnapshotSlot(Widget->Slot));
			}
		}

		// Detach: empty every generated panel, then drop the regenerated widgets
		for (UWidget* Widget : Generated)
		{
			if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
			{
				Panel->ClearChildren();
			}
		}
		for (const FDocContext::FUserWidget& Info : C.UserAdded)
		{
			if (Info.Widget->GetParent())
			{
				Info.Widget->RemoveFromParent();
			}
		}
		C.Tree->RootWidget = nullptr;

		for (UWidget* Widget : Generated)
		{
			if (!C.Locked.Contains(Widget->GetFName()))
			{
				FDwoCanvasWidgetWriter::Discard(Widget);
			}
		}
	}

	bool IsReferenced(const FDocContext& C, FName WidgetName) const
	{
		if (FBlueprintEditorUtils::IsVariableUsed(C.Blueprint, WidgetName))
		{
			return true;
		}
		for (const UWidgetAnimation* Animation : C.Blueprint->Animations)
		{
			if (!Animation)
			{
				continue;
			}
			for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
			{
				if (Binding.WidgetName == WidgetName)
				{
					return true;
				}
			}
		}
		return false;
	}

	/** Re-attaches widgets added in UE and parks widgets whose node is gone. */
	void FinishTree(FDocContext& C)
	{
		UPanelWidget* RootPanel = Cast<UPanelWidget>(C.Tree->RootWidget);
		UCanvasPanel* Parking = nullptr;
		auto GetParking = [&]() -> UPanelWidget*
		{
			if (!Parking && RootPanel)
			{
				bool bLocked = false;
				Parking = Make<UCanvasPanel>(C, TEXT("#parking"), TEXT("DwoCanvas_Parked"), bLocked);
				Parking->SetVisibility(ESlateVisibility::Collapsed);
				RootPanel->AddChild(Parking);
			}
			return Parking;
		};

		for (const FDocContext::FUserWidget& Info : C.UserAdded)
		{
			UPanelWidget* Parent = Info.ParentName.IsNone() ? nullptr : FindObjectFast<UPanelWidget>(C.Tree, Info.ParentName);
			const bool bParentInTree = Parent && (Parent == C.Tree->RootWidget || Parent->GetParent() || Parent->Slot);
			if (!Info.ParentName.IsNone() && bParentInTree && (Parent->CanHaveMultipleChildren() || Parent->GetChildrenCount() == 0))
			{
				UPanelSlot* Slot = Parent->InsertChildAt(FMath::Min(Info.Index, Parent->GetChildrenCount()), Info.Widget);
				RestoreSlot(Slot, Info.Slot);
				++Report->KeptUserAdded;
				Report->Add(EDwoCanvasSeverity::Info, C.DocName, TEXT(""), Info.Widget->GetName(), FString::Printf(TEXT("Kept widget added in UE (in %s)"), *Info.ParentName.ToString()));
			}
			else if (UPanelWidget* Park = GetParking())
			{
				Park->AddChild(Info.Widget);
				++Report->Orphaned;
				Report->Add(EDwoCanvasSeverity::Warning, C.DocName, TEXT(""), Info.Widget->GetName(),
					FString::Printf(TEXT("Widget added in UE lost its parent '%s': parked in DwoCanvas_Parked (collapsed)"), *Info.ParentName.ToString()));
			}
		}

		// Edited / referenced widgets that the design no longer produces
		for (const auto& Pair : C.Locked)
		{
			UWidget* Widget = Pair.Value;
			const FString* Key = C.OldRecord.FindKeyByWidget(Pair.Key);
			if (UPanelWidget* Park = GetParking())
			{
				Park->AddChild(Widget);
				++Report->Orphaned;
				if (Key)
				{
					FDwoCanvasRecordEntry Entry = C.OldRecord.Entries[*Key];
					Entry.bLocked = true;
					C.NewRecord.Entries.Add(*Key, Entry);
				}
				Report->Add(EDwoCanvasSeverity::Warning, C.DocName, Key ? *Key : FString(), Pair.Key.ToString(),
					TEXT("Design node removed but the widget was edited or is referenced (graph / animation): parked in DwoCanvas_Parked"));
			}
		}
		C.Locked.Reset();

		// Removed untouched widgets were already discarded
		for (const auto& Pair : C.OldRecord.Entries)
		{
			if (!C.NewRecord.Entries.Contains(Pair.Key))
			{
				++Report->Removed;
			}
		}
	}

	void ValidateBindings(FDocContext& C)
	{
		TMap<FName, UWidget*> ByName;
		C.Tree->ForEachWidget([&ByName](UWidget* Widget) { ByName.Add(Widget->GetFName(), Widget); });

		// Summary of what matched, so a clean report still shows the bindings were checked
		TArray<FString> Matched;
		for (const auto& Pair : C.BindAll)
		{
			UWidget** Found = ByName.Find(Pair.Key);
			if (Found && (!Pair.Value || (*Found)->IsA(Pair.Value)))
			{
				Matched.Add(FString::Printf(TEXT("%s (%s)"), *Pair.Key.ToString(), *(*Found)->GetClass()->GetName()));
			}
		}
		if (C.BindAll.Num() > 0)
		{
			FDwoCanvasReport::FDocumentResult& Result = Report->FindOrAddDocument(C.DocName);
			Result.BindingsMatched = Matched.Num();
			Result.BindingsTotal = C.BindAll.Num();
			Report->Add(EDwoCanvasSeverity::Info, C.DocName, TEXT(""), TEXT(""),
				FString::Printf(TEXT("Bindings: %d of %d matched: %s"), Matched.Num(), C.BindAll.Num(), *FString::Join(Matched, TEXT(", "))));
		}

		for (const auto& Pair : C.BindAll)
		{
			UWidget** Found = ByName.Find(Pair.Key);
			const bool bRequired = C.BindRequired.Contains(Pair.Key);
			if (!Found)
			{
				Report->Add(bRequired ? EDwoCanvasSeverity::Error : EDwoCanvasSeverity::Info, C.DocName, TEXT(""), Pair.Key.ToString(),
					FString::Printf(TEXT("Parent class expects a %s named '%s' (%s): name a layer '%s' in the design"),
						*GetNameSafe(Pair.Value), *Pair.Key.ToString(), bRequired ? TEXT("BindWidget") : TEXT("BindWidgetOptional"), *Pair.Key.ToString()));
			}
			else if (Pair.Value && !(*Found)->IsA(Pair.Value))
			{
				Report->Add(EDwoCanvasSeverity::Error, C.DocName, TEXT(""), Pair.Key.ToString(),
					FString::Printf(TEXT("'%s' is a %s but the parent class binds a %s: change the layer's role"), *Pair.Key.ToString(), *(*Found)->GetClass()->GetName(), *Pair.Value->GetName()));
			}
		}
	}

	/** Keeps WidgetVariableNameToGuidMap in step with the tree (stable names keep their GUID). */
	void SyncVariableGuids(FDocContext& C)
	{
		TSet<FName> Names;
		C.Tree->ForEachWidget([&Names](UWidget* Widget) { Names.Add(Widget->GetFName()); });
		for (const UWidgetAnimation* Animation : C.Blueprint->Animations)
		{
			if (Animation)
			{
				Names.Add(Animation->GetFName());
			}
		}

		if (C.Blueprint->WidgetVariableNameToGuidMap.IsEmpty())
		{
			return; // compiler fills it
		}
		TArray<FName> Stale;
		for (const auto& Pair : C.Blueprint->WidgetVariableNameToGuidMap)
		{
			if (!Names.Contains(Pair.Key))
			{
				Stale.Add(Pair.Key);
			}
		}
		for (const FName& Name : Stale)
		{
			C.Blueprint->OnVariableRemoved(Name);
		}
		for (const FName& Name : Names)
		{
			if (!C.Blueprint->WidgetVariableNameToGuidMap.Contains(Name))
			{
				C.Blueprint->OnVariableAdded(Name);
			}
		}
	}

	// ----------------------------------------------------------------------------------------------------------------
	// Entry

	// ----------------------------------------------------------------------------------------------------------------
	// Design tokens (W3C Design Tokens format): resolved into literal values before building

	static void FlattenTokens(const FJson& Group, const FString& Prefix, TMap<FString, TSharedPtr<FJsonValue>>& Out)
	{
		if (!Group.IsValid())
		{
			return;
		}
		for (const auto& Pair : Group->Values)
		{
			if (Pair.Key.StartsWith(TEXT("$")) || Pair.Value->Type != EJson::Object)
			{
				continue;
			}
			const FString Path = Prefix.IsEmpty() ? Pair.Key : Prefix + TEXT(".") + Pair.Key;
			const FJson Child = Pair.Value->AsObject();
			if (const TSharedPtr<FJsonValue> Value = Child->TryGetField(TEXT("$value")))
			{
				Out.Add(Path, Value);
			}
			else
			{
				FlattenTokens(Child, Path, Out);
			}
		}
	}

	/** Follows {alias} references. */
	TSharedPtr<FJsonValue> TokenValue(const FString& Path, int32 Depth = 0) const
	{
		const TSharedPtr<FJsonValue>* Found = TokenValues.Find(Path);
		if (!Found || Depth > 8)
		{
			return nullptr;
		}
		FString Text;
		if ((*Found)->TryGetString(Text) && Text.StartsWith(TEXT("{")) && Text.EndsWith(TEXT("}")))
		{
			return TokenValue(Text.Mid(1, Text.Len() - 2), Depth + 1);
		}
		return *Found;
	}

	/** sRGB 0..1 RGBA from "#rrggbb[aa]", [r,g,b,a] or {components, alpha}. */
	static bool TokenColor(const TSharedPtr<FJsonValue>& Value, TArray<double>& Out)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		FString Text;
		if (Value->TryGetString(Text) && Text.StartsWith(TEXT("#")))
		{
			const FColor Color = FColor::FromHex(Text);
			Out = { Color.R / 255.0, Color.G / 255.0, Color.B / 255.0, Color.A / 255.0 };
			return true;
		}
		if (Value->Type == EJson::Array)
		{
			Out.Reset();
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				Out.Add(Item->AsNumber());
			}
			if (Out.Num() == 3) { Out.Add(1.0); }
			return Out.Num() == 4;
		}
		if (Value->Type == EJson::Object)
		{
			const FJson Object = Value->AsObject();
			const TArray<double> Components = JNums(Object, TEXT("components"));
			if (Components.Num() >= 3)
			{
				Out = { Components[0], Components[1], Components[2], JNum(Object, TEXT("alpha"), 1.0) };
				return true;
			}
		}
		return false;
	}

	/** Number from 12, "12px", "1.5" or {value, unit}. */
	static bool TokenNumber(const TSharedPtr<FJsonValue>& Value, double& Out)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::Number)
		{
			Out = Value->AsNumber();
			return true;
		}
		FString Text;
		if (Value->TryGetString(Text))
		{
			Text.RemoveFromEnd(TEXT("px"));
			if (Text.IsNumeric())
			{
				Out = FCString::Atod(*Text);
				return true;
			}
		}
		if (Value->Type == EJson::Object)
		{
			return Value->AsObject()->TryGetNumberField(TEXT("value"), Out);
		}
		return false;
	}

	bool ResolveColorToken(const FString& Path, TArray<double>& Out) const
	{
		if (const FLinearColor* Override = Settings.ColorTokenOverrides.Find(Path))
		{
			const FColor Srgb = Override->ToFColor(true);
			Out = { Srgb.R / 255.0, Srgb.G / 255.0, Srgb.B / 255.0, Override->A };
			return true;
		}
		return TokenColor(TokenValue(Path), Out);
	}

	bool ResolveNumberToken(const FString& Path, double& Out) const
	{
		if (const float* Override = Settings.NumberTokenOverrides.Find(Path))
		{
			Out = *Override;
			return true;
		}
		return TokenNumber(TokenValue(Path), Out);
	}

	void ApplyColorToken(const FJson& Owner, const FString& Where)
	{
		const FString Path = JStr(Owner, TEXT("colorToken"));
		if (Path.IsEmpty())
		{
			return;
		}
		TArray<double> Color;
		if (!ResolveColorToken(Path, Color))
		{
			Report->Add(EDwoCanvasSeverity::Warning, SourceName, TEXT(""), Where, FString::Printf(TEXT("Color token '%s' is not defined: using the literal color"), *Path));
			return;
		}
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const double Channel : Color) { Values.Add(MakeShared<FJsonValueNumber>(Channel)); }
		Owner->SetArrayField(TEXT("color"), Values);
		++TokensApplied;
	}

	void ApplyTokensToNode(const FJson& Node)
	{
		if (!Node.IsValid())
		{
			return;
		}
		const FString Where = JStr(Node, TEXT("name"));
		if (const FJsonArray* Fills = JArr(Node, TEXT("fills")))
		{
			for (const TSharedPtr<FJsonValue>& Fill : *Fills) { ApplyColorToken(Fill->AsObject(), Where); }
		}
		ApplyColorToken(JObj(Node, TEXT("stroke")), Where);
		ApplyColorToken(JObj(Node, TEXT("text")), Where);

		if (const FJson Refs = JObj(Node, TEXT("tokens")))
		{
			for (const auto& Pair : Refs->Values)
			{
				double Number = 0.0;
				const FString Path = Pair.Value->AsString();
				if (!ResolveNumberToken(Path, Number))
				{
					Report->Add(EDwoCanvasSeverity::Warning, SourceName, JStr(Node, TEXT("id")), Where, FString::Printf(TEXT("Token '%s' (%s) is not defined"), *Path, *Pair.Key));
					continue;
				}
				auto Ensure = [&Node](const TCHAR* Field)
				{
					if (!Node->HasTypedField<EJson::Object>(Field)) { Node->SetObjectField(Field, MakeShared<FJsonObject>()); }
					return Node->GetObjectField(Field);
				};
				const TSharedPtr<FJsonValue> NumberValue = MakeShared<FJsonValueNumber>(Number);
				if (Pair.Key == TEXT("gap") || Pair.Key == TEXT("crossGap")) { Ensure(TEXT("layout"))->SetNumberField(Pair.Key, Number); }
				else if (Pair.Key == TEXT("padding")) { Ensure(TEXT("layout"))->SetArrayField(TEXT("padding"), { NumberValue, NumberValue, NumberValue, NumberValue }); }
				else if (Pair.Key == TEXT("radius")) { Node->SetArrayField(TEXT("radius"), { NumberValue }); }
				else if (Pair.Key == TEXT("strokeWidth")) { Ensure(TEXT("stroke"))->SetNumberField(TEXT("width"), Number); }
				else if (Pair.Key == TEXT("opacity")) { Node->SetNumberField(TEXT("opacity"), Number); }
				else if (Pair.Key == TEXT("fontSize"))
				{
					const FJson Text = Ensure(TEXT("text"));
					if (!Text->HasTypedField<EJson::Object>(TEXT("font"))) { Text->SetObjectField(TEXT("font"), MakeShared<FJsonObject>()); }
					Text->GetObjectField(TEXT("font"))->SetNumberField(TEXT("size"), Number);
				}
				else
				{
					Report->Add(EDwoCanvasSeverity::Info, SourceName, JStr(Node, TEXT("id")), Where, FString::Printf(TEXT("Token property '%s' is not supported"), *Pair.Key));
					continue;
				}
				++TokensApplied;
			}
		}

		if (const FJsonArray* Children = JArr(Node, TEXT("children")))
		{
			for (const TSharedPtr<FJsonValue>& Child : *Children) { ApplyTokensToNode(Child->AsObject()); }
		}
	}

	void ApplyTokens(const FJson& Root)
	{
		FlattenTokens(JObj(Root, TEXT("tokens")), FString(), TokenValues);
		for (const TCHAR* Field : { TEXT("documents"), TEXT("components") })
		{
			if (const FJsonArray* Docs = JArr(Root, Field))
			{
				for (const TSharedPtr<FJsonValue>& Doc : *Docs) { ApplyTokensToNode(JObj(Doc->AsObject(), TEXT("root"))); }
			}
		}
		if (TokensApplied > 0)
		{
			Report->Add(EDwoCanvasSeverity::Info, SourceName, TEXT(""), TEXT(""), FString::Printf(TEXT("%d design token reference(s) applied (%d token(s) defined)"), TokensApplied, TokenValues.Num()));
		}
	}

	bool Run(const FJson& Root)
	{
		const FString Schema = JStr(Root, TEXT("schema"));
		bool bMigrated = false;
		if (!FDwoCanvasSchema::MigrateToCurrent(Root, bMigrated))
		{
			// Common mistake: picking the Figma plugin's own manifest.json instead of the export
			const bool bFigmaManifest = Root->HasField(TEXT("api")) && Root->HasField(TEXT("main"));
			Report->Add(EDwoCanvasSeverity::Error, SourceName, TEXT(""), TEXT(""), bFigmaManifest
				? FString(TEXT("This is the Figma plugin's manifest.json, not a design document. Pick the *.dwocanvas.json file that DwoCanvas Export saved."))
				: FString::Printf(TEXT("Not a DwoCanvas design document (schema '%s', expected %s or %s)"), *Schema, FDwoCanvasSchema::CurrentSchemaId, FDwoCanvasSchema::LegacySchemaId));
			return false;
		}
		if (bMigrated)
		{
			Report->Add(EDwoCanvasSeverity::Info, SourceName, TEXT(""), TEXT(""), FString::Printf(TEXT("%s document upgraded to %s on load"), *Schema, FDwoCanvasSchema::CurrentSchemaId));
		}

		// The schema is the contract between adapters and this importer
		TArray<FString> SchemaErrors;
		if (!FDwoCanvasSchema::Validate(Root, SchemaErrors))
		{
			const EDwoCanvasSeverity Severity = Settings.bStrictSchema ? EDwoCanvasSeverity::Error : EDwoCanvasSeverity::Warning;
			for (const FString& Problem : SchemaErrors)
			{
				Report->Add(Severity, SourceName, TEXT(""), TEXT(""), TEXT("Schema: ") + Problem);
			}
			if (Settings.bStrictSchema)
			{
				return false;
			}
		}

		// Source and what it could provide
		const FJson Source = JObj(Root, TEXT("source"));
		Report->SourceTool = JStr(Source, TEXT("tool"));
		Report->Adapter = JStr(Source, TEXT("adapter"));
		if (const FJsonArray* CapabilityArray = JArr(Root, TEXT("capabilities")))
		{
			for (const TSharedPtr<FJsonValue>& Value : *CapabilityArray)
			{
				Capabilities.Add(Value->AsString());
			}
		}
		bDraft = JStr(Source, TEXT("mode")) == TEXT("draft") || Capabilities.Contains(TEXT("inferred"));
		Report->bDraft = bDraft;
		if (bDraft)
		{
			Report->Add(EDwoCanvasSeverity::Warning, SourceName, TEXT(""), TEXT(""), FString::Printf(
				TEXT("Inferred draft (%s): written to %s as new blueprints, not re-importable. Review it, then continue in the design tool or in UE"),
				*JStr(Source, TEXT("model"), Report->Adapter), *Settings.DraftFolder));
		}
		else if (!Capabilities.Contains(TEXT("stable-ids")))
		{
			Report->Add(EDwoCanvasSeverity::Info, SourceName, TEXT(""), TEXT(""), TEXT("The source does not promise stable node ids: work done in UE may not be kept on re-import"));
		}

		int32 Extensions = 0;
		for (const auto& Pair : Root->Values)
		{
			Extensions += Pair.Key.StartsWith(TEXT("x-")) ? 1 : 0;
		}
		if (Extensions > 0)
		{
			Report->Add(EDwoCanvasSeverity::Info, SourceName, TEXT(""), TEXT(""), FString::Printf(TEXT("%d tool-specific extension field(s) (x-...) ignored"), Extensions));
		}

		ApplyTokens(Root);

		if (const FJson ImagesJson = JObj(Root, TEXT("assets")))
		{
			for (const auto& Pair : ImagesJson->Values)
			{
				Images.Add(Pair.Key, Pair.Value->AsObject());
			}
		}

		TArray<FJson> Documents;
		if (const FJsonArray* Array = JArr(Root, TEXT("documents")))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array) { Documents.Add(Value->AsObject()); }
		}
		if (const FJsonArray* Array = JArr(Root, TEXT("components")))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				const FJson Component = Value->AsObject();
				ComponentDocs.Add(JStr(Component, TEXT("id")), Component);
			}
		}

		// Exporter lint results go into the same report
		if (const FJsonArray* Lint = JArr(Root, TEXT("lint")))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Lint)
			{
				const FJson Item = Value->AsObject();
				const FString Level = JStr(Item, TEXT("level"), TEXT("info"));
				Report->Add(Level == TEXT("error") ? EDwoCanvasSeverity::Error : (Level == TEXT("warning") ? EDwoCanvasSeverity::Warning : EDwoCanvasSeverity::Info),
					TEXT("lint"), JStr(Item, TEXT("node")), JStr(Item, TEXT("name")), JStr(Item, TEXT("message")));
			}
		}

		for (const FJson& Doc : Documents) { CollectListEntries(JObj(Doc, TEXT("root"))); }
		for (const auto& Pair : ComponentDocs) { CollectListEntries(JObj(Pair.Value, TEXT("root"))); }

		// Components first (documents then only reference finished classes)
		if (Options.OnlyDocument.IsEmpty())
		{
			for (const auto& Pair : ComponentDocs)
			{
				GetComponentClass(Pair.Key);
			}
		}

		int32 Imported = 0;
		for (const FJson& Doc : Documents)
		{
			if (!Options.OnlyDocument.IsEmpty() && JStr(Doc, TEXT("name")) != Options.OnlyDocument)
			{
				continue;
			}
			if (ImportDocument(Doc, false))
			{
				++Imported;
			}
		}

		return Imported > 0 && !Report->HasErrors();
	}
};

FDwoCanvasImporter::FDwoCanvasImporter(const FDwoCanvasImportOptions& InOptions)
	: Impl(MakeUnique<FImpl>())
{
	Impl->Options = InOptions;
}

FDwoCanvasImporter::~FDwoCanvasImporter() = default;

bool FDwoCanvasImporter::ImportFile(const FString& JsonFile, FDwoCanvasReport& OutReport)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *JsonFile))
	{
		OutReport.Add(EDwoCanvasSeverity::Error, FPaths::GetCleanFilename(JsonFile), TEXT(""), TEXT(""), TEXT("File not found"));
		return false;
	}
	Impl->SourceName = FPaths::GetCleanFilename(JsonFile);
	return ImportJson(Text, FPaths::GetPath(JsonFile), OutReport);
}

bool FDwoCanvasImporter::ImportJson(const FString& JsonText, const FString& SourceDirectory, FDwoCanvasReport& OutReport)
{
	Impl->Report = &OutReport;
	Impl->SourceDirectory = SourceDirectory;
	if (Impl->SourceName.IsEmpty())
	{
		Impl->SourceName = TEXT("design");
	}

	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonText), Root) || !Root.IsValid())
	{
		OutReport.Add(EDwoCanvasSeverity::Error, Impl->SourceName, TEXT(""), TEXT(""), TEXT("Invalid JSON"));
		return false;
	}
	return Impl->Run(Root);
}
