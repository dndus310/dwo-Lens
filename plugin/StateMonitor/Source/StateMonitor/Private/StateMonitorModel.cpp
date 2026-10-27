#include "StateMonitorModel.h"

#include "HAL/PlatformTime.h"
#include "Misc/SecureHash.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"

namespace StateMonitor
{
struct FCapturedValue
{
	TSharedPtr<FStateMonitorNode> Node;
	FString Json;
};

struct FOrderedElement
{
	FString Key;
	int32 Index = INDEX_NONE;
};

FString QuoteJson(const FString& Value)
{
	FString Result(TEXT("\""));
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
		case TEXT('\b'):
			Result += TEXT("\\b");
			break;
		case TEXT('\f'):
			Result += TEXT("\\f");
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
	Result += TEXT('"');
	return Result;
}

FString ChildPath(const FString& ParentPath, FString Name)
{
	Name.ReplaceInline(TEXT("~"), TEXT("~0"));
	Name.ReplaceInline(TEXT("/"), TEXT("~1"));
	return ParentPath + TEXT("/") + Name;
}

FString KeyIdentity(const FString& Key)
{
	uint8 Hash[FSHA1::DigestSize];
	FSHA1::HashBuffer(*Key, static_cast<uint64>(Key.Len()) * sizeof(TCHAR), Hash);
	return BytesToHex(Hash, UE_ARRAY_COUNT(Hash));
}

FString JoinJson(const TArray<FString>& Values, bool bObject, int32 Depth)
{
	const TCHAR* Open = bObject ? TEXT("{") : TEXT("[");
	const TCHAR* Close = bObject ? TEXT("}") : TEXT("]");
	if (Values.IsEmpty())
	{
		return FString(Open) + Close;
	}

	const FString Indent = FString::ChrN((Depth + 1) * 2, TEXT(' '));
	const FString Separator = TEXT(",\n") + Indent;
	return FString(Open) + TEXT("\n") + Indent + FString::Join(Values, *Separator) + TEXT("\n") +
	       FString::ChrN(Depth * 2, TEXT(' ')) + Close;
}

class FReflectionCapture
{
public:
	explicit FReflectionCapture(const FStateMonitorSettings& InSettings);

	FCapturedValue CaptureStruct(const UScriptStruct* StructType, const void* Data, const FString& Name,
	                             const FString& Path, int32 Depth);

private:
	FReflectionCapture(const FStateMonitorSettings& InSettings, int32& InRemainingKeyNodes,
	                   int32& InRemainingContainerSlots);

	FCapturedValue CreateValue(const FString& Name, const FString& Path, const FString& TypeName);

	FCapturedValue CaptureProperty(const FProperty* Property, const void* Data, const FString& Name,
	                               const FString& Path, int32 Depth);

	FCapturedValue CaptureStaticArray(const FProperty* Property, const void* Container, const FString& Path,
	                                  int32 Depth);

	void CaptureArray(FCapturedValue& Result, const FArrayProperty* Property, const void* Data, int32 Depth);

	void CaptureMap(FCapturedValue& Result, const FMapProperty* Property, const void* Data, int32 Depth);

	void CaptureSet(FCapturedValue& Result, const FSetProperty* Property, const void* Data, int32 Depth);

	void CaptureLeaf(FCapturedValue& Result, const FProperty* Property, const void* Data);

	void SetText(FCapturedValue& Result, const FString& Value);

	void AddChild(FCapturedValue& Parent, FCapturedValue& Child, TArray<FString>& JsonValues, bool bNamed);

	bool HasCapacity() const;

	bool GetOrderingKey(const FProperty* Property, const void* Data, int32 Depth, FString& OutKey);

	void SortKeys(TArray<FOrderedElement>& Order, FCapturedValue& Result);

	const FStateMonitorSettings& Settings;
	int32 NodeCount;
	int32 RemainingKeyNodes;
	int32 RemainingContainerSlots;
	int32* SharedKeyNodes;
	int32* SharedContainerSlots;
	bool bCapturingKey;
};

FReflectionCapture::FReflectionCapture(const FStateMonitorSettings& InSettings)
    : Settings(InSettings), NodeCount(0), RemainingKeyNodes(InSettings.MaxNodesPerSnapshot),
      RemainingContainerSlots(InSettings.MaxNodesPerSnapshot * 4), SharedKeyNodes(&RemainingKeyNodes),
      SharedContainerSlots(&RemainingContainerSlots), bCapturingKey(false)
{
}

FReflectionCapture::FReflectionCapture(const FStateMonitorSettings& InSettings, int32& InRemainingKeyNodes,
                                       int32& InRemainingContainerSlots)
    : Settings(InSettings), NodeCount(0), RemainingKeyNodes(0), RemainingContainerSlots(0),
      SharedKeyNodes(&InRemainingKeyNodes), SharedContainerSlots(&InRemainingContainerSlots), bCapturingKey(true)
{
}

FCapturedValue FReflectionCapture::CreateValue(const FString& Name, const FString& Path, const FString& TypeName)
{
	FCapturedValue Result;
	Result.Node = MakeShared<FStateMonitorNode>();
	Result.Node->Name = Name;
	Result.Node->Path = Path;
	Result.Node->TypeName = TypeName;
	++NodeCount;
	if (bCapturingKey)
	{
		--(*SharedKeyNodes);
	}
	return Result;
}

FCapturedValue FReflectionCapture::CaptureStruct(const UScriptStruct* StructType, const void* Data, const FString& Name,
                                                 const FString& Path, int32 Depth)
{
	FCapturedValue Result = CreateValue(Name, Path, StructType->GetName());
	if (Depth >= Settings.MaxDepth)
	{
		Result.Node->bTruncated = true;
		SetText(Result, TEXT("<depth limit>"));
		return Result;
	}

	TArray<FString> Fields;
	for (TFieldIterator<FProperty> Field(StructType); Field; ++Field)
	{
		if (!HasCapacity())
		{
			Result.Node->bTruncated = true;
			break;
		}

		const FProperty* Property = *Field;
		const FString PropertyPath = ChildPath(Path, Property->GetName());
		FCapturedValue Child;
		if (Property->ArrayDim > 1)
		{
			Child = CaptureStaticArray(Property, Data, PropertyPath, Depth + 1);
		}
		else
		{
			const void* Value = Property->ContainerPtrToValuePtr<void>(Data);
			Child = CaptureProperty(Property, Value, Property->GetName(), PropertyPath, Depth + 1);
		}
		Child.Node->DisplayName = Property->GetAuthoredName();
		AddChild(Result, Child, Fields, true);
	}

	if (Fields.IsEmpty() && !Result.Node->bTruncated)
	{
		FString Exported;
		StructType->ExportText(Exported, Data, nullptr, nullptr, PPF_None, nullptr);
		SetText(Result, Exported);
		return Result;
	}

	Result.Node->Value = FString::Printf(TEXT("%d fields"), Result.Node->Children.Num());
	Result.Json = JoinJson(Fields, true, Depth);
	return Result;
}

FCapturedValue FReflectionCapture::CaptureProperty(const FProperty* Property, const void* Data, const FString& Name,
                                                   const FString& Path, int32 Depth)
{
	if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		return CaptureStruct(StructProperty->Struct, Data, Name, Path, Depth);
	}

	FCapturedValue Result = CreateValue(Name, Path, Property->GetCPPType());
	if (Depth >= Settings.MaxDepth)
	{
		Result.Node->bTruncated = true;
		SetText(Result, TEXT("<depth limit>"));
		return Result;
	}

	if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		CaptureArray(Result, ArrayProperty, Data, Depth);
	}
	else if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
	{
		CaptureMap(Result, MapProperty, Data, Depth);
	}
	else if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
	{
		CaptureSet(Result, SetProperty, Data, Depth);
	}
	else
	{
		CaptureLeaf(Result, Property, Data);
	}
	return Result;
}

FCapturedValue FReflectionCapture::CaptureStaticArray(const FProperty* Property, const void* Container,
                                                      const FString& Path, int32 Depth)
{
	FCapturedValue Result = CreateValue(Property->GetName(), Path, Property->GetCPPType() + TEXT("[]"));
	TArray<FString> Values;
	for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
	{
		if (!HasCapacity())
		{
			Result.Node->bTruncated = true;
			break;
		}
		if (Depth >= Settings.MaxDepth)
		{
			Result.Node->bTruncated = true;
			break;
		}

		const FString IndexText = LexToString(Index);
		const void* Data = Property->ContainerPtrToValuePtr<void>(Container, Index);
		FCapturedValue Child = CaptureProperty(Property, Data, IndexText, ChildPath(Path, IndexText), Depth + 1);
		AddChild(Result, Child, Values, false);
	}
	Result.Node->Value = FString::Printf(TEXT("%d items"), Property->ArrayDim);
	Result.Json = JoinJson(Values, false, Depth);
	return Result;
}

void FReflectionCapture::CaptureArray(FCapturedValue& Result, const FArrayProperty* Property, const void* Data,
                                      int32 Depth)
{
	FScriptArrayHelper Array(Property, Data);
	TArray<FString> Values;
	for (int32 Index = 0; Index < Array.Num(); ++Index)
	{
		if (!HasCapacity())
		{
			Result.Node->bTruncated = true;
			break;
		}

		const FString IndexText = LexToString(Index);
		FCapturedValue Child = CaptureProperty(Property->Inner, Array.GetRawPtr(Index), IndexText,
		                                       ChildPath(Result.Node->Path, IndexText), Depth + 1);
		AddChild(Result, Child, Values, false);
	}
	Result.Node->Value = FString::Printf(TEXT("%d items"), Array.Num());
	Result.Json = JoinJson(Values, false, Depth);
}

bool FReflectionCapture::GetOrderingKey(const FProperty* Property, const void* Data, int32 Depth, FString& OutKey)
{
	if (*SharedKeyNodes <= 0)
	{
		return false;
	}

	// Keys share a separate snapshot-wide budget. An incomplete key has no reliable identity.
	FReflectionCapture KeyCapture(Settings, *SharedKeyNodes, *SharedContainerSlots);
	FCapturedValue Key = KeyCapture.CaptureProperty(Property, Data, TEXT("Key"), TEXT("$"), Depth);
	if (Key.Node->bTruncated)
	{
		return false;
	}
	OutKey = MoveTemp(Key.Json);
	return true;
}

void FReflectionCapture::SortKeys(TArray<FOrderedElement>& Order, FCapturedValue& Result)
{
	Order.Sort(
	    [](const FOrderedElement& Left, const FOrderedElement& Right)
	    {
		    return Left.Key < Right.Key;
	    });
	for (int32 Index = 1; Index < Order.Num(); ++Index)
	{
		if (Order[Index].Key == Order[Index - 1].Key)
		{
			// Distinct keys can export the same value (for example expired object references).
			Order[Index].Index = INDEX_NONE;
			Order[Index - 1].Index = INDEX_NONE;
			Result.Node->bTruncated = true;
		}
	}
}

void FReflectionCapture::CaptureMap(FCapturedValue& Result, const FMapProperty* Property, const void* Data, int32 Depth)
{
	FScriptMapHelper Map(Property, Data);
	TArray<FOrderedElement> Order;
	for (int32 Index = 0; Index < Map.GetMaxIndex(); ++Index)
	{
		if (*SharedContainerSlots <= 0 || *SharedKeyNodes <= 0)
		{
			Result.Node->bTruncated = true;
			break;
		}
		--(*SharedContainerSlots);
		if (!Map.IsValidIndex(Index))
		{
			continue;
		}
		if (Order.Num() >= Settings.MaxNodesPerSnapshot)
		{
			Result.Node->bTruncated = true;
			break;
		}
		FString Key;
		if (!GetOrderingKey(Property->KeyProp, Map.GetKeyPtr(Index), Depth + 2, Key))
		{
			Result.Node->bTruncated = true;
			continue;
		}
		FOrderedElement& Element = Order.AddDefaulted_GetRef();
		Element.Key = MoveTemp(Key);
		Element.Index = Index;
	}
	SortKeys(Order, Result);

	TArray<FString> Entries;
	for (const FOrderedElement& Element : Order)
	{
		if (Element.Index == INDEX_NONE)
		{
			continue;
		}
		if (NodeCount + 3 > Settings.MaxNodesPerSnapshot)
		{
			Result.Node->bTruncated = true;
			break;
		}
		if (bCapturingKey && *SharedKeyNodes < 3)
		{
			Result.Node->bTruncated = true;
			break;
		}

		const FString EntryPath = ChildPath(Result.Node->Path, TEXT("key:") + KeyIdentity(Element.Key));
		FCapturedValue Entry = CreateValue(Element.Key, EntryPath, TEXT("Map entry"));
		TArray<FString> Fields;
		FCapturedValue Key = CaptureProperty(Property->KeyProp, Map.GetKeyPtr(Element.Index), TEXT("Key"),
		                                     ChildPath(EntryPath, TEXT("Key")), Depth + 2);
		AddChild(Entry, Key, Fields, true);
		if (HasCapacity())
		{
			FCapturedValue Value = CaptureProperty(Property->ValueProp, Map.GetValuePtr(Element.Index), TEXT("Value"),
			                                       ChildPath(EntryPath, TEXT("Value")), Depth + 2);
			AddChild(Entry, Value, Fields, true);
		}
		else
		{
			Entry.Node->bTruncated = true;
		}
		Entry.Json = JoinJson(Fields, true, Depth + 1);
		Entry.Node->Value = TEXT("Key / Value");
		AddChild(Result, Entry, Entries, false);
	}
	Result.Node->Value = FString::Printf(TEXT("%d entries"), Map.Num());
	Result.Json = JoinJson(Entries, false, Depth);
}

void FReflectionCapture::CaptureSet(FCapturedValue& Result, const FSetProperty* Property, const void* Data, int32 Depth)
{
	const FScriptSetHelper Set(Property, Data);
	TArray<FOrderedElement> Order;
	for (int32 Index = 0; Index < Set.GetMaxIndex(); ++Index)
	{
		if (*SharedContainerSlots <= 0 || *SharedKeyNodes <= 0)
		{
			Result.Node->bTruncated = true;
			break;
		}
		--(*SharedContainerSlots);
		if (!Set.IsValidIndex(Index))
		{
			continue;
		}
		if (Order.Num() >= Settings.MaxNodesPerSnapshot)
		{
			Result.Node->bTruncated = true;
			break;
		}
		FString Key;
		if (!GetOrderingKey(Property->ElementProp, Set.GetElementPtr(Index), Depth + 1, Key))
		{
			Result.Node->bTruncated = true;
			continue;
		}
		FOrderedElement& Element = Order.AddDefaulted_GetRef();
		Element.Key = MoveTemp(Key);
		Element.Index = Index;
	}
	SortKeys(Order, Result);

	TArray<FString> Values;
	for (const FOrderedElement& Element : Order)
	{
		if (Element.Index == INDEX_NONE)
		{
			continue;
		}
		if (!HasCapacity())
		{
			Result.Node->bTruncated = true;
			break;
		}
		FCapturedValue Child =
		    CaptureProperty(Property->ElementProp, Set.GetElementPtr(Element.Index), Element.Key,
		                    ChildPath(Result.Node->Path, TEXT("item:") + KeyIdentity(Element.Key)), Depth + 1);
		AddChild(Result, Child, Values, false);
	}
	Result.Node->Value = FString::Printf(TEXT("%d items"), Set.Num());
	Result.Json = JoinJson(Values, false, Depth);
}

void FReflectionCapture::SetText(FCapturedValue& Result, const FString& Value)
{
	if (Value.Len() > Settings.MaxTextCharacters)
	{
		Result.Node->Value = Value.Left(Settings.MaxTextCharacters) + TEXT("…");
		Result.Node->bTruncated = true;
	}
	else
	{
		Result.Node->Value = Value;
	}
	Result.Json = QuoteJson(Result.Node->Value);
}

void FReflectionCapture::CaptureLeaf(FCapturedValue& Result, const FProperty* Property, const void* Data)
{
	if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
	{
		Result.Node->Value = BoolProperty->GetPropertyValue(Data) ? TEXT("true") : TEXT("false");
		Result.Json = Result.Node->Value;
		return;
	}
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		const int64 EnumValue = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Data);
		const FString EnumName = EnumProperty->GetEnum()->GetNameStringByValue(EnumValue);
		SetText(Result, EnumName.IsEmpty() ? LexToString(EnumValue) : EnumName);
		return;
	}
	if (const FNumericProperty* NumberProperty = CastField<FNumericProperty>(Property))
	{
		if (const UEnum* Enum = NumberProperty->GetIntPropertyEnum())
		{
			const int64 EnumValue = NumberProperty->GetSignedIntPropertyValue(Data);
			const FString EnumName = Enum->GetNameStringByValue(EnumValue);
			SetText(Result, EnumName.IsEmpty() ? LexToString(EnumValue) : EnumName);
			return;
		}
		if (NumberProperty->IsFloatingPoint())
		{
			const double Value = NumberProperty->GetFloatingPointPropertyValue(Data);
			if (!FMath::IsFinite(Value))
			{
				SetText(Result, LexToString(Value));
				return;
			}
			Result.Node->Value = FString::Printf(TEXT("%.17g"), Value);
		}
		else
		{
			Result.Node->Value = NumberProperty->GetNumericPropertyValueToString(Data);
		}
		Result.Json = Result.Node->Value;
		return;
	}
	if (const FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Property))
	{
		SetText(Result, SoftProperty->GetPropertyValue(Data).ToSoftObjectPath().ToString());
		return;
	}
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
	{
		const UObject* Object = ObjectProperty->GetObjectPropertyValue(Data);
		if (IsValid(Object))
		{
			SetText(Result, Object->GetPathName());
		}
		else
		{
			Result.Node->Value = TEXT("null");
			Result.Json = TEXT("null");
		}
		return;
	}
	if (const FStrProperty* StringProperty = CastField<FStrProperty>(Property))
	{
		SetText(Result, StringProperty->GetPropertyValue(Data));
		return;
	}
	if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
	{
		SetText(Result, NameProperty->GetPropertyValue(Data).ToString());
		return;
	}
	if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
	{
		SetText(Result, TextProperty->GetPropertyValue(Data).ToString());
		return;
	}

	FString Exported;
	Property->ExportTextItem_Direct(Exported, Data, nullptr, nullptr, PPF_None);
	SetText(Result, MoveTemp(Exported));
}

void FReflectionCapture::AddChild(FCapturedValue& Parent, FCapturedValue& Child, TArray<FString>& JsonValues,
                                  bool bNamed)
{
	Parent.Node->Children.Add(Child.Node);
	Parent.Node->bTruncated |= Child.Node->bTruncated;
	JsonValues.Add(bNamed ? QuoteJson(Child.Node->Name) + TEXT(": ") + Child.Json : Child.Json);
}

bool FReflectionCapture::HasCapacity() const
{
	if (bCapturingKey)
	{
		if (*SharedKeyNodes <= 0)
		{
			return false;
		}
	}
	return NodeCount < Settings.MaxNodesPerSnapshot;
}

void Flatten(const TSharedPtr<FStateMonitorNode>& Node, TMap<FString, TSharedPtr<FStateMonitorNode>>& Fields)
{
	Fields.Add(Node->Path, Node);
	for (const TSharedPtr<FStateMonitorNode>& Child : Node->Children)
	{
		Flatten(Child, Fields);
	}
}

void RecordChange(FStateMonitorSource& Source, const FStateMonitorNode& Node, EStateMonitorChange Kind,
                  const FString& PreviousValue, const FString& Value, int32 Limit)
{
	if (Source.Changes.Num() >= Limit)
	{
		++Source.OmittedChanges;
		return;
	}

	FStateMonitorChange& Change = Source.Changes.AddDefaulted_GetRef();
	Change.Path = Node.Path;
	Change.TypeName = Node.TypeName;
	Change.PreviousValue = PreviousValue;
	Change.Value = Value;
	Change.Change = Kind;
}

bool MarkParentChanges(const TSharedPtr<FStateMonitorNode>& Node)
{
	bool bChanged = Node->Change != EStateMonitorChange::Unchanged;
	for (const TSharedPtr<FStateMonitorNode>& Child : Node->Children)
	{
		bChanged |= MarkParentChanges(Child);
	}
	if (bChanged)
	{
		if (Node->Change == EStateMonitorChange::Unchanged)
		{
			Node->Change = EStateMonitorChange::Modified;
		}
	}
	return bChanged;
}

void CompareSources(FStateMonitorSource& Current, const FStateMonitorSource* Previous, int32 Limit)
{
	TMap<FString, TSharedPtr<FStateMonitorNode>> OldFields;
	TMap<FString, TSharedPtr<FStateMonitorNode>> CurrentFields;
	if (Previous)
	{
		Flatten(Previous->Root, OldFields);
	}
	Flatten(Current.Root, CurrentFields);
	for (const auto& Field : CurrentFields)
	{
		FStateMonitorNode& Node = *Field.Value;
		const TSharedPtr<FStateMonitorNode>* OldNode = OldFields.Find(Field.Key);
		if (!OldNode)
		{
			if (Previous)
			{
				if (Previous->bTruncated)
				{
					continue;
				}
			}
			Node.Change = EStateMonitorChange::Added;
			RecordChange(Current, Node, Node.Change, FString(), Node.Value, Limit);
			continue;
		}
		const bool bValueChanged = Node.Value != (*OldNode)->Value;
		const bool bTypeChanged = Node.TypeName != (*OldNode)->TypeName;
		const bool bBoundsChanged = Node.bTruncated != (*OldNode)->bTruncated;
		if (bValueChanged || bTypeChanged || bBoundsChanged)
		{
			Node.Change = EStateMonitorChange::Modified;
			Node.PreviousValue = (*OldNode)->Value;
			RecordChange(Current, Node, Node.Change, Node.PreviousValue, Node.Value, Limit);
		}
	}
	for (const auto& Field : OldFields)
	{
		if (Current.bTruncated)
		{
			break;
		}
		if (!CurrentFields.Contains(Field.Key))
		{
			RecordChange(Current, *Field.Value, EStateMonitorChange::Removed, Field.Value->Value, FString(), Limit);
		}
	}
	MarkParentChanges(Current.Root);
}

TSharedPtr<FStateMonitorNode> CopyBaseline(const TSharedPtr<FStateMonitorNode>& Node)
{
	TSharedPtr<FStateMonitorNode> Copy = MakeShared<FStateMonitorNode>(*Node);
	Copy->Change = EStateMonitorChange::Unchanged;
	Copy->PreviousValue.Reset();
	Copy->Children.Reset();
	for (const TSharedPtr<FStateMonitorNode>& Child : Node->Children)
	{
		Copy->Children.Add(CopyBaseline(Child));
	}
	return Copy;
}

int64 CountHistoryCharacters(const FStateMonitorHistoryEntry& Entry)
{
	int64 Count = 0;
	for (const FStateMonitorChange& Change : Entry.Changes)
	{
		Count += Change.Path.Len();
		Count += Change.TypeName.Len();
		Count += Change.PreviousValue.Len();
		Count += Change.Value.Len();
	}
	return Count;
}

void AddHistory(FStateMonitorSource& Source, const FStateMonitorSettings& Settings)
{
	TSharedPtr<FStateMonitorHistoryEntry> Entry = MakeShared<FStateMonitorHistoryEntry>();
	Entry->Sequence = Source.Sequence;
	Entry->TimeSeconds = Source.TimeSeconds;
	Entry->OmittedChanges = Source.OmittedChanges;
	int64 EntryCharacters = 0;
	for (const FStateMonitorChange& Change : Source.Changes)
	{
		const int64 ChangeCharacters = static_cast<int64>(Change.Path.Len()) + Change.TypeName.Len() +
		                               Change.PreviousValue.Len() + Change.Value.Len();
		if (EntryCharacters + ChangeCharacters > Settings.MaxHistoryCharacters)
		{
			++Entry->OmittedChanges;
			continue;
		}
		Entry->Changes.Add(Change);
		EntryCharacters += ChangeCharacters;
	}

	int64 HistoryCharacters = EntryCharacters;
	int32 FirstRetained = Source.History.Num();
	for (int32 Index = Source.History.Num() - 1; Index >= 0; --Index)
	{
		if (Source.History.Num() - Index >= Settings.MaxHistoryEntries)
		{
			break;
		}
		const int64 PreviousCharacters = CountHistoryCharacters(*Source.History[Index]);
		if (HistoryCharacters + PreviousCharacters > Settings.MaxHistoryCharacters)
		{
			break;
		}
		HistoryCharacters += PreviousCharacters;
		FirstRetained = Index;
	}
	if (FirstRetained > 0)
	{
		Source.History.RemoveAt(0, FirstRetained);
	}
	Source.History.Add(Entry);
}
} // namespace StateMonitor

FStateMonitorModel::FStateMonitorModel(const FStateMonitorSettings& InSettings)
    : Settings(InSettings), Revision(0), NextSequence(1)
{
	check(IsInGameThread());
	Settings.MaxSources = FMath::Clamp(Settings.MaxSources, 1, 1024);
	Settings.MaxNodesPerSnapshot = FMath::Clamp(Settings.MaxNodesPerSnapshot, 1, 100000);
	Settings.MaxDepth = FMath::Clamp(Settings.MaxDepth, 1, 64);
	Settings.MaxHistoryEntries = FMath::Clamp(Settings.MaxHistoryEntries, 0, 10000);
	Settings.MaxChangesPerEntry = FMath::Clamp(Settings.MaxChangesPerEntry, 1, 100000);
	Settings.MaxTextCharacters = FMath::Clamp(Settings.MaxTextCharacters, 16, 65536);
	Settings.MaxHistoryCharacters = FMath::Clamp(Settings.MaxHistoryCharacters, 64, 64 * 1024 * 1024);
}

bool FStateMonitorModel::SubmitSnapshot(FName SourceId, const FText& Label, const UScriptStruct* StructType,
                                           const void* StructData, bool bRecordHistory)
{
	check(IsInGameThread());
	if (SourceId.IsNone())
	{
		return false;
	}
	if (!StructType)
	{
		return false;
	}
	if (!StructData)
	{
		return false;
	}

	TSharedPtr<FStateMonitorSource> Previous = FindSource(SourceId);
	if (!Previous.IsValid())
	{
		if (Sources.Num() >= Settings.MaxSources)
		{
			return false;
		}
	}

	StateMonitor::FReflectionCapture Capture(Settings);
	StateMonitor::FCapturedValue Value =
	    Capture.CaptureStruct(StructType, StructData, StructType->GetName(), TEXT("$"), 0);
	TSharedPtr<FStateMonitorSource> Current = MakeShared<FStateMonitorSource>();
	Current->Root = Value.Node;
	Current->CurrentJson = MoveTemp(Value.Json);
	Current->SourceId = SourceId;
	Current->Label = Label;
	Current->StructType = StructType->GetPathName();
	Current->bTruncated = Value.Node->bTruncated;
	if (Previous.IsValid())
	{
		Current->History = Previous->History;
	}
	StateMonitor::CompareSources(*Current, Previous.Get(), Settings.MaxChangesPerEntry);
	if (Previous.IsValid())
	{
		const bool bSameValues = Current->Changes.IsEmpty() && Current->CurrentJson == Previous->CurrentJson;
		const bool bSameType = Current->StructType == Previous->StructType;
		const bool bSameLabel = Current->Label.EqualTo(Previous->Label);
		if (bSameValues && bSameType && bSameLabel)
		{
			return true;
		}
	}
	Current->Sequence = NextSequence++;
	Current->TimeSeconds = FPlatformTime::Seconds();
	if (!Current->Changes.IsEmpty())
	{
		if (bRecordHistory && Settings.MaxHistoryEntries > 0)
		{
			StateMonitor::AddHistory(*Current, Settings);
		}
	}
	if (Previous.IsValid())
	{
		Sources[Sources.IndexOfByKey(Previous)] = Current;
	}
	else
	{
		Sources.Add(Current);
	}
	++Revision;
	return true;
}

bool FStateMonitorModel::RemoveSource(FName SourceId)
{
	check(IsInGameThread());
	const int32 Removed = Sources.RemoveAll(
	    [SourceId](const TSharedPtr<FStateMonitorSource>& Source)
	    {
		    return Source->SourceId == SourceId;
	    });
	if (Removed > 0)
	{
		++Revision;
	}
	return Removed > 0;
}

void FStateMonitorModel::ClearHistory(FName SourceId)
{
	check(IsInGameThread());
	const TSharedPtr<FStateMonitorSource> Previous = FindSource(SourceId);
	if (!Previous.IsValid())
	{
		return;
	}

	TSharedPtr<FStateMonitorSource> Current = MakeShared<FStateMonitorSource>(*Previous);
	Current->History.Reset();
	Sources[Sources.IndexOfByKey(Previous)] = Current;
	++Revision;
}

void FStateMonitorModel::ResetBaseline(FName SourceId)
{
	check(IsInGameThread());
	const TSharedPtr<FStateMonitorSource> Previous = FindSource(SourceId);
	if (!Previous.IsValid())
	{
		return;
	}

	TSharedPtr<FStateMonitorSource> Current = MakeShared<FStateMonitorSource>(*Previous);
	Current->Root = StateMonitor::CopyBaseline(Previous->Root);
	Current->Changes.Reset();
	Current->OmittedChanges = 0;
	Sources[Sources.IndexOfByKey(Previous)] = Current;
	++Revision;
}

void FStateMonitorModel::Reset()
{
	check(IsInGameThread());
	Sources.Reset();
	++Revision;
}

const TArray<TSharedPtr<FStateMonitorSource>>& FStateMonitorModel::GetSources() const
{
	check(IsInGameThread());
	return Sources;
}

TSharedPtr<FStateMonitorSource> FStateMonitorModel::FindSource(FName SourceId) const
{
	check(IsInGameThread());
	for (const TSharedPtr<FStateMonitorSource>& Source : Sources)
	{
		if (Source->SourceId == SourceId)
		{
			return Source;
		}
	}
	return nullptr;
}

uint64 FStateMonitorModel::GetRevision() const
{
	check(IsInGameThread());
	return Revision;
}

const FStateMonitorSettings& FStateMonitorModel::GetSettings() const
{
	check(IsInGameThread());
	return Settings;
}
