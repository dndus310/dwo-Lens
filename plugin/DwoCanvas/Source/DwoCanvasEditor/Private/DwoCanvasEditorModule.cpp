// DwoCanvas - editor module: Tools menu entries

#include "DwoCanvasImporter.h"
#include "DwoCanvasResultWindow.h"

#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "IDesktopPlatform.h"
#include "Misc/MessageDialog.h"
#include "Modules/ModuleManager.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "DwoCanvasEditor"

class FDwoCanvasEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FDwoCanvasEditorModule::RegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}

private:
	FString LastFile;

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		FToolMenuSection& Section = Menu->AddSection("DwoCanvas", LOCTEXT("Section", "DwoCanvas"));

		Section.AddMenuEntry("DwoCanvasImport",
			LOCTEXT("Import", "Import Design File..."),
			LOCTEXT("ImportTip", "Imports a DwoCanvas export (Figma plugin JSON) into Widget Blueprints. Widgets edited in UE are kept."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FDwoCanvasEditorModule::ImportWithDialog, false)));

		Section.AddMenuEntry("DwoCanvasReimport",
			LOCTEXT("Reimport", "Reimport Last Design File"),
			LOCTEXT("ReimportTip", "Imports the last file again."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateRaw(this, &FDwoCanvasEditorModule::Import, false),
				FCanExecuteAction::CreateLambda([this]() { return !LastFile.IsEmpty(); })));

		Section.AddMenuEntry("DwoCanvasImportReset",
			LOCTEXT("ImportReset", "Import Design File (Reset Overrides)..."),
			LOCTEXT("ImportResetTip", "Imports and overwrites widgets that were edited in UE."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FDwoCanvasEditorModule::ImportWithDialog, true)));
	}

	void ImportWithDialog(bool bResetOverrides)
	{
		IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
		if (!DesktopPlatform)
		{
			return;
		}

		TArray<FString> Files;
		const void* ParentWindow = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
		if (!DesktopPlatform->OpenFileDialog(ParentWindow, TEXT("DwoCanvas export"), FPaths::GetPath(LastFile), TEXT(""),
			TEXT("DwoCanvas export (*.dwocanvas.json)|*.dwocanvas.json|All JSON files (*.json)|*.json"), EFileDialogFlags::None, Files) || Files.Num() == 0)
		{
			return;
		}

		if (bResetOverrides && FMessageDialog::Open(EAppMsgType::YesNo,
			LOCTEXT("ConfirmReset", "Overwrite widgets that were edited in UE with the design?")) != EAppReturnType::Yes)
		{
			return;
		}

		LastFile = Files[0];
		Import(bResetOverrides);
	}

	void Import(bool bResetOverrides)
	{
		FDwoCanvasImportOptions Options;
		Options.bResetOverrides = bResetOverrides;

		FDwoCanvasReport Report;
		FDwoCanvasImporter Importer(Options);
		Importer.ImportFile(LastFile, Report);
		const FString ReportFile = Report.SaveToFile(LastFile);

		FDwoCanvasResultWindow::Show(Report, LastFile, ReportFile,
			FSimpleDelegate::CreateLambda([this]() { Import(false); }));
	}
};

IMPLEMENT_MODULE(FDwoCanvasEditorModule, DwoCanvasEditor)

#undef LOCTEXT_NAMESPACE
