// DwoCanvas - JSON Schema check of design documents and designbridge/1 -> dwocanvas/2 migration

#include "DwoCanvasSchema.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

const TCHAR* FDwoCanvasSchema::CurrentSchemaId = TEXT("dwocanvas/2");
const TCHAR* FDwoCanvasSchema::LegacySchemaId = TEXT("designbridge/1");

FString FDwoCanvasSchema::GetSchemaFile()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DwoCanvas"));
	return Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Schema"), TEXT("dwocanvas.schema.json")) : FString();
}

namespace DwoCanvasSchemaImpl
{
	using FJsonPtr = TSharedPtr<FJsonObject>;
	using FValuePtr = TSharedPtr<FJsonValue>;

	struct FValidator
	{
		FJsonPtr Root;
		TArray<FString>* Errors = nullptr;
		int32 MaxErrors = 50;

		bool Full() const { return Errors->Num() >= MaxErrors; }

		void Fail(const FString& Path, const FString& Message)
		{
			if (!Full())
			{
				Errors->Add(FString::Printf(TEXT("%s: %s"), Path.IsEmpty() ? TEXT("/") : *Path, *Message));
			}
		}

		FJsonPtr Resolve(const FJsonPtr& Schema) const
		{
			FString Ref;
			if (!Schema->TryGetStringField(TEXT("$ref"), Ref) || !Ref.StartsWith(TEXT("#/")))
			{
				return Schema;
			}
			TArray<FString> Parts;
			Ref.RightChop(2).ParseIntoArray(Parts, TEXT("/"));
			FJsonPtr Node = Root;
			for (const FString& Part : Parts)
			{
				const FJsonPtr* Next = nullptr;
				if (!Node->TryGetObjectField(Part, Next))
				{
					return Schema;
				}
				Node = *Next;
			}
			return Node;
		}

		static FString TypeOf(const FValuePtr& Value)
		{
			switch (Value->Type)
			{
			case EJson::Null: return TEXT("null");
			case EJson::String: return TEXT("string");
			case EJson::Number: return TEXT("number");
			case EJson::Boolean: return TEXT("boolean");
			case EJson::Array: return TEXT("array");
			case EJson::Object: return TEXT("object");
			default: return TEXT("unknown");
			}
		}

		static bool MatchesType(const FValuePtr& Value, const FString& Type)
		{
			if (Type == TEXT("integer"))
			{
				return Value->Type == EJson::Number && FMath::IsNearlyEqual(Value->AsNumber(), FMath::RoundToDouble(Value->AsNumber()));
			}
			return TypeOf(Value) == Type;
		}

		static bool Equal(const FValuePtr& A, const FValuePtr& B)
		{
			if (A->Type != B->Type)
			{
				return false;
			}
			switch (A->Type)
			{
			case EJson::String: return A->AsString() == B->AsString();
			case EJson::Number: return A->AsNumber() == B->AsNumber();
			case EJson::Boolean: return A->AsBool() == B->AsBool();
			case EJson::Null: return true;
			default:
			{
				FString SA, SB;
				const TSharedRef<TJsonWriter<>> WA = TJsonWriterFactory<>::Create(&SA);
				const TSharedRef<TJsonWriter<>> WB = TJsonWriterFactory<>::Create(&SB);
				FJsonSerializer::Serialize(A, TEXT(""), WA);
				FJsonSerializer::Serialize(B, TEXT(""), WB);
				return SA == SB;
			}
			}
		}

		static FString Describe(const FValuePtr& Value)
		{
			if (Value->Type == EJson::String) { return TEXT("\"") + Value->AsString() + TEXT("\""); }
			if (Value->Type == EJson::Number) { return FString::SanitizeFloat(Value->AsNumber()); }
			if (Value->Type == EJson::Boolean) { return Value->AsBool() ? TEXT("true") : TEXT("false"); }
			return TypeOf(Value);
		}

		/** Returns true when valid. Errors are only collected when bCollect. */
		bool Check(const FValuePtr& Value, FJsonPtr Schema, const FString& Path, bool bCollect)
		{
			if (Full() || !Schema.IsValid())
			{
				return true;
			}
			Schema = Resolve(Schema);
			const int32 Before = Errors->Num();
			bool bOk = true;
			auto Error = [&](const FString& Message)
			{
				bOk = false;
				if (bCollect)
				{
					Fail(Path, Message);
				}
			};

			// anyOf: valid if one branch is valid (errors from branches are not reported)
			const TArray<FValuePtr>* AnyOf = nullptr;
			if (Schema->TryGetArrayField(TEXT("anyOf"), AnyOf))
			{
				bool bAny = false;
				for (const FValuePtr& Branch : *AnyOf)
				{
					TArray<FString> Scratch;
					TArray<FString>* Saved = Errors;
					Errors = &Scratch;
					const bool bBranch = Check(Value, Branch->AsObject(), Path, false);
					Errors = Saved;
					if (bBranch)
					{
						bAny = true;
						break;
					}
				}
				if (!bAny)
				{
					Error(FString::Printf(TEXT("%s does not match any allowed form"), *Describe(Value)));
					return false;
				}
			}

			// type
			if (const FValuePtr TypeValue = Schema->TryGetField(TEXT("type")))
			{
				bool bTypeOk = false;
				TArray<FString> Types;
				if (TypeValue->Type == EJson::Array)
				{
					for (const FValuePtr& T : TypeValue->AsArray()) { Types.Add(T->AsString()); }
				}
				else
				{
					Types.Add(TypeValue->AsString());
				}
				for (const FString& T : Types)
				{
					bTypeOk |= MatchesType(Value, T);
				}
				if (!bTypeOk)
				{
					Error(FString::Printf(TEXT("expected %s, got %s"), *FString::Join(Types, TEXT(" or ")), *TypeOf(Value)));
					return false;
				}
			}

			// const / enum
			if (const FValuePtr Const = Schema->TryGetField(TEXT("const")))
			{
				if (!Equal(Value, Const))
				{
					Error(FString::Printf(TEXT("must be %s (got %s)"), *Describe(Const), *Describe(Value)));
				}
			}
			const TArray<FValuePtr>* Enum = nullptr;
			if (Schema->TryGetArrayField(TEXT("enum"), Enum))
			{
				bool bFound = false;
				TArray<FString> Allowed;
				for (const FValuePtr& Option : *Enum)
				{
					bFound |= Equal(Value, Option);
					Allowed.Add(Describe(Option));
				}
				if (!bFound)
				{
					Error(FString::Printf(TEXT("%s is not one of %s"), *Describe(Value), *FString::Join(Allowed, TEXT(", "))));
				}
			}

			// numbers
			double Limit = 0.0;
			if (Value->Type == EJson::Number)
			{
				if (Schema->TryGetNumberField(TEXT("minimum"), Limit) && Value->AsNumber() < Limit)
				{
					Error(FString::Printf(TEXT("%s is below the minimum %s"), *Describe(Value), *FString::SanitizeFloat(Limit)));
				}
				if (Schema->TryGetNumberField(TEXT("maximum"), Limit) && Value->AsNumber() > Limit)
				{
					Error(FString::Printf(TEXT("%s is above the maximum %s"), *Describe(Value), *FString::SanitizeFloat(Limit)));
				}
			}

			// strings
			if (Value->Type == EJson::String)
			{
				const FString Text = Value->AsString();
				if (Schema->TryGetNumberField(TEXT("minLength"), Limit) && Text.Len() < (int32)Limit)
				{
					Error(TEXT("must not be empty"));
				}
				FString Pattern;
				if (Schema->TryGetStringField(TEXT("pattern"), Pattern))
				{
					FRegexMatcher Matcher(FRegexPattern(Pattern), Text);
					if (!Matcher.FindNext())
					{
						Error(FString::Printf(TEXT("\"%s\" does not match %s"), *Text, *Pattern));
					}
				}
			}

			// arrays
			if (Value->Type == EJson::Array)
			{
				const TArray<FValuePtr>& Items = Value->AsArray();
				if (Schema->TryGetNumberField(TEXT("minItems"), Limit) && Items.Num() < (int32)Limit)
				{
					Error(FString::Printf(TEXT("needs at least %d item(s), has %d"), (int32)Limit, Items.Num()));
				}
				if (Schema->TryGetNumberField(TEXT("maxItems"), Limit) && Items.Num() > (int32)Limit)
				{
					Error(FString::Printf(TEXT("allows at most %d item(s), has %d"), (int32)Limit, Items.Num()));
				}
				bool bUnique = false;
				if (Schema->TryGetBoolField(TEXT("uniqueItems"), bUnique) && bUnique)
				{
					for (int32 I = 0; I < Items.Num(); ++I)
					{
						for (int32 J = I + 1; J < Items.Num(); ++J)
						{
							if (Equal(Items[I], Items[J]))
							{
								Error(FString::Printf(TEXT("duplicate item %s"), *Describe(Items[I])));
							}
						}
					}
				}
				const FJsonPtr* ItemSchema = nullptr;
				if (Schema->TryGetObjectField(TEXT("items"), ItemSchema))
				{
					for (int32 Index = 0; Index < Items.Num(); ++Index)
					{
						bOk &= Check(Items[Index], *ItemSchema, FString::Printf(TEXT("%s/%d"), *Path, Index), bCollect);
					}
				}
			}

			// objects
			if (Value->Type == EJson::Object)
			{
				const FJsonPtr Object = Value->AsObject();
				const TArray<FValuePtr>* Required = nullptr;
				if (Schema->TryGetArrayField(TEXT("required"), Required))
				{
					for (const FValuePtr& Name : *Required)
					{
						if (!Object->HasField(Name->AsString()))
						{
							Error(FString::Printf(TEXT("missing required field \"%s\""), *Name->AsString()));
						}
					}
				}

				const FJsonPtr* Properties = nullptr;
				Schema->TryGetObjectField(TEXT("properties"), Properties);
				const FJsonPtr* Patterns = nullptr;
				Schema->TryGetObjectField(TEXT("patternProperties"), Patterns);
				const FValuePtr Additional = Schema->TryGetField(TEXT("additionalProperties"));

				for (const auto& Pair : Object->Values)
				{
					const FString ChildPath = Path + TEXT("/") + Pair.Key;
					bool bMatched = false;
					if (Properties)
					{
						const FJsonPtr* PropertySchema = nullptr;
						if ((*Properties)->TryGetObjectField(Pair.Key, PropertySchema))
						{
							bMatched = true;
							bOk &= Check(Pair.Value, *PropertySchema, ChildPath, bCollect);
						}
					}
					if (Patterns)
					{
						for (const auto& PatternPair : (*Patterns)->Values)
						{
							FRegexMatcher Matcher(FRegexPattern(PatternPair.Key), Pair.Key);
							if (Matcher.FindNext())
							{
								bMatched = true;
								bOk &= Check(Pair.Value, PatternPair.Value->AsObject(), ChildPath, bCollect);
							}
						}
					}
					if (!bMatched && Additional.IsValid())
					{
						if (Additional->Type == EJson::Boolean && !Additional->AsBool())
						{
							Error(FString::Printf(TEXT("unknown field \"%s\""), *Pair.Key));
						}
						else if (Additional->Type == EJson::Object)
						{
							bOk &= Check(Pair.Value, Additional->AsObject(), ChildPath, bCollect);
						}
					}
				}
			}

			return bOk && Errors->Num() == Before;
		}
	};

	const TMap<FString, FString>& LegacyRoles()
	{
		static const TMap<FString, FString> Map = {
			{ TEXT("Auto"), TEXT("auto") }, { TEXT("Button"), TEXT("button") }, { TEXT("ListView"), TEXT("list") },
			{ TEXT("TileView"), TEXT("tile") }, { TEXT("ScrollBox"), TEXT("scroll") }, { TEXT("TextInput"), TEXT("textInput") },
			{ TEXT("CheckBox"), TEXT("checkBox") }, { TEXT("Slider"), TEXT("slider") }, { TEXT("ProgressBar"), TEXT("progressBar") },
			{ TEXT("Switcher"), TEXT("switcher") }, { TEXT("NamedSlot"), TEXT("namedSlot") }, { TEXT("Spacer"), TEXT("spacer") },
			{ TEXT("Ignore"), TEXT("ignore") },
		};
		return Map;
	}

	void MigratePaints(const FJsonPtr& Owner, const TCHAR* Field)
	{
		const TArray<FValuePtr>* Paints = nullptr;
		if (Owner.IsValid() && Owner->TryGetArrayField(Field, Paints))
		{
			for (const FValuePtr& Value : *Paints)
			{
				const FJsonPtr Paint = Value->AsObject();
				FString Token;
				if (Paint.IsValid() && Paint->TryGetStringField(TEXT("token"), Token))
				{
					Paint->SetStringField(TEXT("colorToken"), Token);
					Paint->RemoveField(TEXT("token"));
				}
			}
		}
	}

	void MigrateNode(const FJsonPtr& Node)
	{
		if (!Node.IsValid())
		{
			return;
		}
		const FJsonPtr* Meta = nullptr;
		if (Node->TryGetObjectField(TEXT("meta"), Meta))
		{
			FString Role;
			if ((*Meta)->TryGetStringField(TEXT("role"), Role) && !Node->HasField(TEXT("role")))
			{
				const FString* Mapped = LegacyRoles().Find(Role);
				Node->SetStringField(TEXT("role"), Mapped ? *Mapped : Role.ToLower());
			}
			bool bVariable = false;
			FString VariableText;
			if ((*Meta)->TryGetBoolField(TEXT("variable"), bVariable))
			{
				Node->SetBoolField(TEXT("variable"), bVariable);
			}
			else if ((*Meta)->TryGetStringField(TEXT("variable"), VariableText))
			{
				Node->SetBoolField(TEXT("variable"), VariableText.Equals(TEXT("true"), ESearchCase::IgnoreCase));
			}
		}

		MigratePaints(Node, TEXT("fills"));
		const FJsonPtr* Text = nullptr;
		if (Node->TryGetObjectField(TEXT("text"), Text))
		{
			const FJsonPtr* Font = nullptr;
			FString Style;
			if ((*Text)->TryGetObjectField(TEXT("font"), Font) && (*Font)->TryGetStringField(TEXT("style"), Style) && !(*Font)->HasField(TEXT("weight")))
			{
				bool bItalic = false;
				(*Font)->SetNumberField(TEXT("weight"), FDwoCanvasSchema::WeightFromStyle(Style, bItalic));
				(*Font)->SetBoolField(TEXT("italic"), bItalic);
			}
		}

		const FJsonPtr* States = nullptr;
		if (Node->TryGetObjectField(TEXT("states"), States))
		{
			for (const auto& Pair : (*States)->Values)
			{
				MigratePaints(Pair.Value->AsObject(), TEXT("fills"));
			}
		}

		const TArray<FValuePtr>* Children = nullptr;
		if (Node->TryGetArrayField(TEXT("children"), Children))
		{
			for (const FValuePtr& Child : *Children)
			{
				MigrateNode(Child->AsObject());
			}
		}
	}
}

int32 FDwoCanvasSchema::WeightFromStyle(const FString& Style, bool& bOutItalic)
{
	const FString S = Style.Replace(TEXT(" "), TEXT("")).Replace(TEXT("-"), TEXT("")).ToLower();
	bOutItalic = S.Contains(TEXT("italic")) || S.Contains(TEXT("oblique"));
	if (S.Contains(TEXT("extralight")) || S.Contains(TEXT("ultralight"))) { return 200; }
	if (S.Contains(TEXT("semibold")) || S.Contains(TEXT("demibold"))) { return 600; }
	if (S.Contains(TEXT("extrabold")) || S.Contains(TEXT("ultrabold")) || S.Contains(TEXT("heavy"))) { return 800; }
	if (S.Contains(TEXT("thin")) || S.Contains(TEXT("hairline"))) { return 100; }
	if (S.Contains(TEXT("light"))) { return 300; }
	if (S.Contains(TEXT("medium"))) { return 500; }
	if (S.Contains(TEXT("black"))) { return 900; }
	if (S.Contains(TEXT("bold"))) { return 700; }
	return 400;
}

bool FDwoCanvasSchema::MigrateToCurrent(const TSharedPtr<FJsonObject>& Document, bool& bOutMigrated)
{
	using namespace DwoCanvasSchemaImpl;
	bOutMigrated = false;
	FString Schema;
	Document->TryGetStringField(TEXT("schema"), Schema);
	if (Schema == CurrentSchemaId)
	{
		return true;
	}
	if (!Schema.StartsWith(LegacySchemaId))
	{
		return false;
	}

	Document->SetStringField(TEXT("schema"), CurrentSchemaId);

	// images -> assets (file -> uri)
	const FJsonPtr* Images = nullptr;
	if (Document->TryGetObjectField(TEXT("images"), Images))
	{
		const FJsonPtr Assets = *Images;
		for (const auto& Pair : Assets->Values)
		{
			const FJsonPtr Asset = Pair.Value->AsObject();
			FString File;
			if (Asset.IsValid() && Asset->TryGetStringField(TEXT("file"), File))
			{
				Asset->SetStringField(TEXT("uri"), File);
				Asset->RemoveField(TEXT("file"));
			}
		}
		Document->SetObjectField(TEXT("assets"), Assets);
		Document->RemoveField(TEXT("images"));
	}

	// Extract-mode defaults (designbridge/1 only came from the Figma exporter or hand-written samples)
	if (!Document->HasField(TEXT("capabilities")))
	{
		TArray<FValuePtr> Capabilities;
		for (const TCHAR* Capability : { TEXT("layout.flow"), TEXT("layout.constraints"), TEXT("paint.gradient"), TEXT("paint.image"),
			TEXT("effects"), TEXT("components"), TEXT("variants"), TEXT("preview"), TEXT("stable-ids") })
		{
			Capabilities.Add(MakeShared<FJsonValueString>(Capability));
		}
		Document->SetArrayField(TEXT("capabilities"), Capabilities);
	}
	FJsonPtr Source = Document->HasTypedField<EJson::Object>(TEXT("source")) ? Document->GetObjectField(TEXT("source")) : MakeShared<FJsonObject>();
	if (!Source->HasField(TEXT("mode")))
	{
		Source->SetStringField(TEXT("mode"), TEXT("extract"));
	}
	Document->SetObjectField(TEXT("source"), Source);

	for (const TCHAR* Field : { TEXT("documents"), TEXT("components") })
	{
		const TArray<FValuePtr>* Docs = nullptr;
		if (Document->TryGetArrayField(Field, Docs))
		{
			for (const FValuePtr& Doc : *Docs)
			{
				const FJsonPtr* Root = nullptr;
				if (Doc->AsObject().IsValid() && Doc->AsObject()->TryGetObjectField(TEXT("root"), Root))
				{
					MigrateNode(*Root);
				}
			}
		}
	}

	// Fields that never existed in the v2 top level
	Document->RemoveField(TEXT("images"));
	bOutMigrated = true;
	return true;
}

bool FDwoCanvasSchema::Validate(const TSharedPtr<FJsonObject>& Document, TArray<FString>& OutErrors, int32 MaxErrors)
{
	using namespace DwoCanvasSchemaImpl;
	static FJsonPtr CachedSchema;
	if (!CachedSchema.IsValid())
	{
		FString Text;
		const FString File = GetSchemaFile();
		if (File.IsEmpty() || !FFileHelper::LoadFileToString(Text, *File)
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), CachedSchema) || !CachedSchema.IsValid())
		{
			OutErrors.Add(FString::Printf(TEXT("Schema file could not be read (%s)"), *File));
			CachedSchema.Reset();
			return false;
		}
	}

	FValidator Validator;
	Validator.Root = CachedSchema;
	Validator.Errors = &OutErrors;
	Validator.MaxErrors = MaxErrors;
	const int32 Before = OutErrors.Num();
	Validator.Check(MakeShared<FJsonValueObject>(Document), CachedSchema, TEXT(""), true);
	return OutErrors.Num() == Before;
}
