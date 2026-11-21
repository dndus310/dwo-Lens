// DwoCanvas - Blueprint / Python entry points and the import commandlet

#include "DwoCanvasLibrary.h"

#include "DwoCanvasImporter.h"

FString UDwoCanvasLibrary::ImportDesignFile(const FString& JsonFile, bool bResetOverrides, bool& bSucceeded)
{
	FDwoCanvasImportOptions Options;
	Options.bResetOverrides = bResetOverrides;

	FDwoCanvasReport Report;
	FDwoCanvasImporter Importer(Options);
	bSucceeded = Importer.ImportFile(JsonFile, Report);
	Report.SaveToFile(JsonFile);
	return Report.ToText();
}

UDwoCanvasImportCommandlet::UDwoCanvasImportCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 UDwoCanvasImportCommandlet::Main(const FString& Params)
{
	FString File;
	if (!FParse::Value(*Params, TEXT("File="), File))
	{
		UE_LOG(LogTemp, Error, TEXT("DwoCanvasImport: -File=<export.json> is required"));
		return 2;
	}

	FDwoCanvasImportOptions Options;
	Options.bResetOverrides = FParse::Param(*Params, TEXT("ResetOverrides"));
	FParse::Value(*Params, TEXT("Doc="), Options.OnlyDocument);

	FDwoCanvasReport Report;
	FDwoCanvasImporter Importer(Options);
	Importer.ImportFile(File, Report);
	const FString ReportFile = Report.SaveToFile(File);

	UE_LOG(LogTemp, Display, TEXT("\n%s\nReport: %s"), *Report.ToText(), *ReportFile);
	return Report.HasErrors() ? 1 : 0;
}
