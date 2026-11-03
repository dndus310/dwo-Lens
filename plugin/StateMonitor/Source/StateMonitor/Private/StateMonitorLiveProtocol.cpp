#include "StateMonitorLiveProtocol.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StateMonitorModel.h"

namespace StateMonitorLive
{
namespace Private
{
FString Quote(const FString& Value)
{
	FString Result;
	Result.Reserve(Value.Len() + 2);
	Result.AppendChar(TEXT('"'));
	for (const TCHAR Character : Value)
	{
		switch (Character)
		{
		case TEXT('"'):
			Result += TEXT("\\\"");
			break;
		case TEXT('\\'):
			Result += TEXT("\\\\");
			break;
		case TEXT('\n'):
			Result += TEXT("\\n");
			break;
		case TEXT('\r'):
			Result += TEXT("\\r");
			break;
		case TEXT('\t'):
			Result += TEXT("\\t");
			break;
		default:
			if (Character < 0x20)
			{
				Result += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(Character));
			}
			else
			{
				Result.AppendChar(Character);
			}
			break;
		}
	}
	Result.AppendChar(TEXT('"'));
	return Result;
}

FString Number(double Value)
{
	return FMath::IsFinite(Value) ? FString::Printf(TEXT("%.3f"), Value) : FString(TEXT("0"));
}

const TCHAR* ChangeName(EStateMonitorChange Change)
{
	switch (Change)
	{
	case EStateMonitorChange::Added:
		return TEXT("added");
	case EStateMonitorChange::Modified:
		return TEXT("modified");
	case EStateMonitorChange::Removed:
		return TEXT("removed");
	default:
		return TEXT("unchanged");
	}
}

/** @brief 모델 경로 "$/A/B"를 뷰어용 "A/B"로 바꾼다. 루트는 빈 문자열이다. */
FString ViewerPath(const FString& Path)
{
	if (Path.StartsWith(TEXT("$/")))
	{
		return Path.RightChop(2);
	}
	return Path == TEXT("$") ? FString() : Path;
}

/** @brief "/Script/Engine.GameModeBase" 같은 경로에서 타입 이름만 남긴다. */
FString ShortTypeName(const FString& TypePath)
{
	int32 Dot = INDEX_NONE;
	if (TypePath.FindLastChar(TEXT('.'), Dot))
	{
		return TypePath.RightChop(Dot + 1);
	}
	return TypePath;
}
} // namespace Private

FString CompactJson(const FString& Json)
{
	// 모델 JSON의 문자열은 제어 문자를 모두 이스케이프하므로 날것의 '\n'은 항상 서식이다.
	FString Result;
	Result.Reserve(Json.Len());
	bool bSkippingIndent = false;
	for (const TCHAR Character : Json)
	{
		if (Character == TEXT('\n') || Character == TEXT('\r'))
		{
			bSkippingIndent = true;
			continue;
		}
		if (bSkippingIndent && Character == TEXT(' '))
		{
			continue;
		}
		bSkippingIndent = false;
		Result.AppendChar(Character);
	}
	return Result;
}

FString MakeHelloLine(const FStateMonitorLiveHello& Hello)
{
	return FString::Printf(TEXT("{\"t\":\"hello\",\"v\":%d,\"project\":%s,\"world\":%s,\"netMode\":%s,\"pie\":%d,")
	                       TEXT("\"pid\":%u,\"engine\":%s,\"build\":%s,\"time\":%s}"),
	                       ProtocolVersion, *Private::Quote(Hello.Project), *Private::Quote(Hello.World),
	                       *Private::Quote(Hello.NetMode), Hello.PieInstance, Hello.ProcessId,
	                       *Private::Quote(Hello.EngineVersion), *Private::Quote(Hello.BuildConfiguration),
	                       *Private::Number(Hello.TimeSeconds));
}

FString MakeWorldLine(const FString& World)
{
	return FString::Printf(TEXT("{\"t\":\"world\",\"world\":%s}"), *Private::Quote(World));
}

FString MakeRemoveLine(FName SourceId)
{
	return FString::Printf(TEXT("{\"t\":\"remove\",\"id\":%s}"), *Private::Quote(SourceId.ToString()));
}

FString MakeSnapLine(const FStateMonitorSource& Source, bool bAuto)
{
	FString Value = CompactJson(Source.CurrentJson);
	if (!Value.StartsWith(TEXT("{")))
	{
		// 필드가 없는 구조체는 텍스트 표현이 값이 되므로 객체로 감싼다.
		Value = TEXT("{\"value\":") + (Value.IsEmpty() ? FString(TEXT("null")) : Value) + TEXT("}");
	}

	TArray<FString> Changes;
	Changes.Reserve(Source.Changes.Num());
	for (const FStateMonitorChange& Change : Source.Changes)
	{
		Changes.Add(FString::Printf(TEXT("{\"path\":%s,\"type\":%s,\"old\":%s,\"new\":%s,\"change\":\"%s\"}"),
		                            *Private::Quote(Private::ViewerPath(Change.Path)), *Private::Quote(Change.TypeName),
		                            *Private::Quote(Change.PreviousValue), *Private::Quote(Change.Value),
		                            Private::ChangeName(Change.Change)));
	}

	FString Line;
	Line.Reserve(Value.Len() + 256);
	Line += TEXT("{\"t\":\"snap\",\"id\":");
	Line += Private::Quote(Source.SourceId.ToString());
	Line += TEXT(",\"label\":");
	Line += Private::Quote(Source.Label.ToString());
	Line += TEXT(",\"type\":");
	Line += Private::Quote(Private::ShortTypeName(Source.StructType));
	Line += Source.bObject ? TEXT(",\"kind\":\"object\"") : TEXT(",\"kind\":\"struct\"");
	Line += TEXT(",\"object\":");
	Line += Private::Quote(Source.ObjectPath);
	Line += TEXT(",\"owner\":");
	Line += Private::Quote(Source.OwnerPath);
	Line += FString::Printf(TEXT(",\"seq\":%llu,\"time\":%s"), static_cast<unsigned long long>(Source.Sequence),
	                        *Private::Number(Source.TimeSeconds));
	Line += bAuto ? TEXT(",\"auto\":true") : TEXT(",\"auto\":false");
	Line += TEXT(",\"value\":");
	Line += Value;
	Line += TEXT(",\"changes\":[");
	Line += FString::Join(Changes, TEXT(","));
	Line += TEXT("]");
	if (Source.OmittedChanges > 0)
	{
		Line += FString::Printf(TEXT(",\"omittedChanges\":%d"), Source.OmittedChanges);
	}
	Line += Source.bTruncated ? TEXT(",\"truncated\":true}") : TEXT(",\"truncated\":false}");
	return Line;
}

bool ParseCommand(const FString& Line, FStateMonitorLiveCommand& OutCommand)
{
	OutCommand = FStateMonitorLiveCommand();
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return false;
	}

	FString Type;
	if (!Object->TryGetStringField(TEXT("t"), Type))
	{
		return false;
	}

	bool bOn = false;
	if (Object->TryGetBoolField(TEXT("on"), bOn))
	{
		OutCommand.bOn = bOn;
	}

	if (Type == TEXT("auto"))
	{
		OutCommand.Type = EStateMonitorLiveCommand::Auto;
		double Interval = 0.0;
		if (Object->TryGetNumberField(TEXT("interval"), Interval) && FMath::IsFinite(Interval))
		{
			OutCommand.Interval = FMath::Clamp(Interval, 0.05, 10.0);
			OutCommand.bHasInterval = true;
		}
		bool bEngine = false;
		if (Object->TryGetBoolField(TEXT("engine"), bEngine))
		{
			OutCommand.bIncludeEngineProperties = bEngine;
			OutCommand.bHasEngine = true;
		}
		return true;
	}
	if (Type == TEXT("watch") || Type == TEXT("unwatch"))
	{
		OutCommand.Type = Type == TEXT("watch") ? EStateMonitorLiveCommand::Watch : EStateMonitorLiveCommand::Unwatch;
		if (!Object->TryGetStringField(TEXT("class"), OutCommand.ClassName) || OutCommand.ClassName.IsEmpty())
		{
			return false;
		}
		double Max = 0.0;
		if (Object->TryGetNumberField(TEXT("max"), Max) && FMath::IsFinite(Max))
		{
			OutCommand.Max = FMath::Clamp(FMath::RoundToInt32(Max), 1, 256);
		}
		return true;
	}
	if (Type == TEXT("pause"))
	{
		OutCommand.Type = EStateMonitorLiveCommand::Pause;
		return true;
	}
	if (Type == TEXT("resync"))
	{
		OutCommand.Type = EStateMonitorLiveCommand::Resync;
		return true;
	}
	return false;
}
} // namespace StateMonitorLive
