// DwoCanvas - renders a Widget Blueprint and compares it with the design preview

#include "DwoCanvasVisualDiff.h"

#include "DwoCanvasImportRecord.h"
#include "DwoCanvasSettings.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/IUserObjectListEntry.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Components/ListView.h"
#include "ShaderCompiler.h"
#include "Editor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "Slate/WidgetRenderer.h"
#include "TextureResource.h"
#include "WidgetBlueprint.h"

bool FDwoCanvasVisualDiff::CanRender()
{
	return GEditor && !IsRunningCommandlet() && FApp::CanEverRender() && FSlateApplication::IsInitialized();
}

bool FDwoCanvasVisualDiff::Render(UWidgetBlueprint* Blueprint, FIntPoint Size, TArray<FColor>& OutPixels, const FDwoCanvasListPreview* ListPreview)
{
	if (!CanRender() || !Blueprint || !Blueprint->GeneratedClass || Size.X <= 0 || Size.Y <= 0)
	{
		return false;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	UUserWidget* Widget = World ? CreateWidget<UUserWidget>(World, Blueprint->GeneratedClass.Get()) : nullptr;
	if (!Widget)
	{
		return false;
	}

	// Lists are empty at runtime: show as many placeholder items as the design had entries, carrying the design's texts
	TArray<UListView*> Lists;
	Widget->WidgetTree->ForEachWidget([ListPreview, &Lists](UWidget* Child)
	{
		if (UListView* List = Cast<UListView>(Child))
		{
			const FIntProperty* Count = FindFProperty<FIntProperty>(UListViewBase::StaticClass(), TEXT("NumDesignerPreviewEntries"));
			const int32 Num = Count ? Count->GetPropertyValue_InContainer(List) : 0;
			const TArray<TMap<FName, FString>>* Entries = ListPreview ? ListPreview->Find(List->GetFName()) : nullptr;
			TArray<UObject*> Items;
			for (int32 Index = 0; Index < Num; ++Index)
			{
				UDwoCanvasPreviewItem* Item = NewObject<UDwoCanvasPreviewItem>(List);
				if (Entries && Entries->IsValidIndex(Index))
				{
					Item->Texts = (*Entries)[Index];
				}
				Items.Add(Item);
			}
			List->SetListItems(Items);
			Lists.Add(List);
		}
	});

	// Entry widgets exist after the first draw: write each item's design texts into its entry
	auto ApplyPreviewTexts = [&Lists]()
	{
		for (UListView* List : Lists)
		{
			for (UUserWidget* Entry : List->GetDisplayedEntryWidgets())
			{
				const UDwoCanvasPreviewItem* Item = Entry && Entry->Implements<UUserObjectListEntry>()
					? Cast<UDwoCanvasPreviewItem>(UUserObjectListEntryLibrary::GetListItemObject(TScriptInterface<IUserObjectListEntry>(Entry))) : nullptr;
				if (!Item || !Entry->WidgetTree)
				{
					continue;
				}
				for (const TPair<FName, FString>& Text : Item->Texts)
				{
					if (UTextBlock* Block = Cast<UTextBlock>(Entry->WidgetTree->FindWidget(Text.Key)))
					{
						if (!Block->GetText().ToString().Equals(Text.Value))
						{
							Block->SetText(FText::FromString(Text.Value));
						}
					}
				}
			}
		}
	};

	// Gamma space, like the game viewport: Slate's shaders gamma-correct their output and translucent widgets, shadows and
	// edge anti-aliasing blend on the encoded values (also how Figma composites). A linear float target blends in linear
	// space instead, which makes every translucent pixel lighter than it will look in game.
	const bool bGamma = true;
	// 8-bit target without sRGB write conversion: stores the shader's gamma-corrected values as they are
	UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>();
	Target->ClearColor = FLinearColor::Transparent;
	Target->Filter = TF_Nearest;
	Target->InitCustomFormat(Size.X, Size.Y, PF_B8G8R8A8, /*bInForceLinearGamma*/ true);
	Target->UpdateResourceImmediate(true);

	const TSharedRef<SWidget> SlateWidget = Widget->TakeWidget();
	FWidgetRenderer Renderer(bGamma, true);

	// The target already holds display (sRGB-encoded) values: read them as they are
	FReadSurfaceDataFlags ReadFlags(RCM_UNorm, CubeFace_MAX);
	ReadFlags.SetLinearToGamma(false);

	// Materials compile on first use: draw until two consecutive frames match (layout settled, shaders ready)
	bool bRead = false;
	TArray<FColor> Previous;
	for (int32 Pass = 0; Pass < 8; ++Pass)
	{
		Renderer.DrawWidget(Target, SlateWidget, FVector2D(Size), 0.f);
		FlushRenderingCommands();
		ApplyPreviewTexts(); // takes effect from the next pass (frames differ until it has)
		if (GShaderCompilingManager && GShaderCompilingManager->IsCompiling())
		{
			GShaderCompilingManager->FinishAllCompilation();
			continue;
		}

		FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
		bRead = Resource && Resource->ReadPixels(OutPixels, ReadFlags);
		if (bRead && Pass > 0 && OutPixels == Previous)
		{
			break;
		}
		Previous = OutPixels;
	}
	Widget->RemoveFromParent();
	return bRead && OutPixels.Num() == Size.X * Size.Y;
}

namespace
{
	/** Composite over a neutral grey so transparent areas compare equal on both sides. */
	FColor OverGrey(const FColor& Color)
	{
		const float A = Color.A / 255.f;
		auto Mix = [A](uint8 Channel) { return (uint8)FMath::RoundToInt(Channel * A + 128.f * (1.f - A)); };
		return FColor(Mix(Color.R), Mix(Color.G), Mix(Color.B), 255);
	}

	void SavePng(const FString& File, FIntPoint Size, const TArray<FColor>& Pixels)
	{
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
		FFileHelper::SaveArrayToFile(Png, *File);
	}
}

void FDwoCanvasVisualDiff::Compare(UWidgetBlueprint* Blueprint, const TArray<uint8>& PreviewPng, FIntPoint Size, double PreviewScale,
	const FString& DocumentName, FDwoCanvasReport& Report, const FDwoCanvasListPreview* ListPreview)
{
	if (!CanRender())
	{
		Report.Add(EDwoCanvasSeverity::Info, DocumentName, TEXT(""), TEXT(""), TEXT("Visual diff skipped (no renderer: run the import from the editor)"));
		return;
	}

	FImage Preview;
	if (!FImageUtils::DecompressImage(PreviewPng.GetData(), PreviewPng.Num(), Preview))
	{
		Report.Add(EDwoCanvasSeverity::Warning, DocumentName, TEXT(""), TEXT(""), TEXT("Visual diff: preview image could not be decoded"));
		return;
	}
	Preview.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);

	TArray<FColor> Actual;
	if (!Render(Blueprint, Size, Actual, ListPreview))
	{
		Report.Add(EDwoCanvasSeverity::Warning, DocumentName, TEXT(""), TEXT(""), TEXT("Visual diff: the widget could not be rendered"));
		return;
	}

	const UDwoCanvasSettings& Settings = UDwoCanvasSettings::Get();
	const TArrayView64<FColor> PreviewPixels = Preview.AsBGRA8();
	TArray<FColor> Expected;
	TArray<FColor> Diff;
	Expected.SetNumUninitialized(Size.X * Size.Y);
	Diff.SetNumUninitialized(Size.X * Size.Y);

	int64 Different = 0;
	for (int32 Y = 0; Y < Size.Y; ++Y)
	{
		for (int32 X = 0; X < Size.X; ++X)
		{
			// Nearest sample of the (possibly 2x) preview
			const int32 PX = FMath::Clamp(FMath::FloorToInt((X + 0.5) * PreviewScale), 0, (int32)Preview.SizeX - 1);
			const int32 PY = FMath::Clamp(FMath::FloorToInt((Y + 0.5) * PreviewScale), 0, (int32)Preview.SizeY - 1);
			const FColor E = OverGrey(PreviewPixels[(int64)PY * Preview.SizeX + PX]);
			const FColor A = OverGrey(Actual[Y * Size.X + X]);
			const int32 Delta = FMath::Max3(FMath::Abs(E.R - A.R), FMath::Abs(E.G - A.G), FMath::Abs(E.B - A.B));
			const bool bDifferent = Delta > Settings.VisualDiffPixelThreshold;
			Different += bDifferent ? 1 : 0;
			Expected[Y * Size.X + X] = E;
			Diff[Y * Size.X + X] = bDifferent ? FColor(255, 0, 255, 255) : FColor(A.R / 3, A.G / 3, A.B / 3, 255);
		}
	}

	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DwoCanvas"), TEXT("Reports"));
	IFileManager::Get().MakeDirectory(*Dir, true);
	TArray<FColor> ActualOpaque = Actual;
	for (FColor& Pixel : ActualOpaque) { Pixel = OverGrey(Pixel); }
	SavePng(FPaths::Combine(Dir, DocumentName + TEXT("_actual.png")), Size, ActualOpaque);
	SavePng(FPaths::Combine(Dir, DocumentName + TEXT("_expected.png")), Size, Expected);
	SavePng(FPaths::Combine(Dir, DocumentName + TEXT("_diff.png")), Size, Diff);

	const double Ratio = (double)Different / FMath::Max<int64>(1, (int64)Size.X * Size.Y);
	{
		FDwoCanvasReport::FDocumentResult& Result = Report.FindOrAddDocument(DocumentName);
		Result.DiffRatio = Ratio;
		Result.DiffImage = FPaths::ConvertRelativePathToFull(FPaths::Combine(Dir, DocumentName + TEXT("_diff.png")));
		Result.ActualImage = FPaths::ConvertRelativePathToFull(FPaths::Combine(Dir, DocumentName + TEXT("_actual.png")));
		Result.ExpectedImage = FPaths::ConvertRelativePathToFull(FPaths::Combine(Dir, DocumentName + TEXT("_expected.png")));
	}
	const FString Message = FString::Printf(TEXT("Visual diff: %.1f%% of pixels differ (images in Saved/DwoCanvas/Reports/%s_*.png)"), Ratio * 100.0, *DocumentName);
	Report.Add(Ratio > Settings.VisualDiffWarnRatio ? EDwoCanvasSeverity::Warning : EDwoCanvasSeverity::Info, DocumentName, TEXT(""), TEXT(""), Message);
}
