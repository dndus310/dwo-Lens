// DwoCanvas - JSON Schema check of design documents and designbridge/1 -> dwocanvas/2 migration

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

class DWOCANVASEDITOR_API FDwoCanvasSchema
{
public:
	static const TCHAR* CurrentSchemaId;  // dwocanvas/2
	static const TCHAR* LegacySchemaId;   // designbridge/1

	/** Plugin/Schema/dwocanvas.schema.json */
	static FString GetSchemaFile();

	/**
	 * Validates a document against the plugin's schema (JSON Schema 2020-12 subset: $ref, type, enum, const,
	 * required, properties, additionalProperties, patternProperties, items, min/maxItems, uniqueItems,
	 * minimum, maximum, minLength, pattern, anyOf). Errors are "path: message".
	 */
	static bool Validate(const TSharedPtr<FJsonObject>& Document, TArray<FString>& OutErrors, int32 MaxErrors = 50);

	/** Upgrades a designbridge/1 document in place. Returns false if the document is neither version. */
	static bool MigrateToCurrent(const TSharedPtr<FJsonObject>& Document, bool& bOutMigrated);

	/** CSS weight from a style name ("SemiBold Italic" -> 600, italic). */
	static int32 WeightFromStyle(const FString& Style, bool& bOutItalic);
};
