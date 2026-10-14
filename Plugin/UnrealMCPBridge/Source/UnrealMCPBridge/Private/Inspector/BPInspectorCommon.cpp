#include "BPInspectorCommon.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EditorAssetLibrary.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "UObject/UnrealType.h"

namespace BPICommon
{

UObject* LoadAssetObject(const FString& AssetPath)
{
	if (AssetPath.IsEmpty())
	{
		return nullptr;
	}

	if (UObject* Loaded = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath))
	{
		return Loaded;
	}

	// cold-load fallback: 일부 cooked/registry 케이스에서 StaticLoadObject 가 실패
	// (MCPInspectorLibrary.cpp 의 LoadBlueprintFromPath 와 동일 사유)
	return UEditorAssetLibrary::LoadAsset(AssetPath);
}

UBlueprint* LoadBlueprintFromPath(const FString& AssetPath)
{
	if (UBlueprint* BP = Cast<UBlueprint>(LoadAssetObject(AssetPath)))
	{
		return BP;
	}

	// Level Blueprint fallback: AssetPath 가 .umap(World) 인 경우
	if (UObject* WorldObj = StaticLoadObject(UWorld::StaticClass(), nullptr, *AssetPath))
	{
		if (UWorld* World = Cast<UWorld>(WorldObj))
		{
			if (ULevel* Level = World->PersistentLevel)
			{
				if (ULevelScriptBlueprint* LSBP = Level->GetLevelScriptBlueprint(/*bDontCreate*/ true))
				{
					return LSBP;
				}
			}
		}
	}

	return nullptr;
}

UEdGraph* GetGraphByIndex(UBlueprint* BP, int32 GraphIndex)
{
	if (!BP)
	{
		return nullptr;
	}
	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	return AllGraphs.IsValidIndex(GraphIndex) ? AllGraphs[GraphIndex] : nullptr;
}

UEdGraphNode* GetNodeByIndex(UEdGraph* Graph, int32 NodeIndex)
{
	if (!Graph || !Graph->Nodes.IsValidIndex(NodeIndex))
	{
		return nullptr;
	}
	return Graph->Nodes[NodeIndex];
}

// 한 세그먼트 파싱: "Name" 또는 "Name[3]"
static bool ParsePathSegment(const FString& Segment, FString& OutName, int32& OutArrayIndex)
{
	OutArrayIndex = INDEX_NONE;
	OutName = Segment;

	int32 BracketPos;
	if (Segment.FindChar(TEXT('['), BracketPos))
	{
		if (!Segment.EndsWith(TEXT("]")))
		{
			return false;
		}
		OutName = Segment.Left(BracketPos);
		const FString IdxStr = Segment.Mid(BracketPos + 1, Segment.Len() - BracketPos - 2);
		OutArrayIndex = FCString::Atoi(*IdxStr);
		if (OutArrayIndex < 0 || IdxStr.IsEmpty())
		{
			return false;
		}
	}
	return !OutName.IsEmpty();
}

bool ResolvePropertyPath(UObject* RootObject, const FString& PropertyPath, FProperty*& OutProp, void*& OutValuePtr, FString& OutError)
{
	OutProp = nullptr;
	OutValuePtr = nullptr;

	if (!RootObject)
	{
		OutError = TEXT("null root object");
		return false;
	}

	TArray<FString> Segments;
	PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
	if (Segments.Num() == 0)
	{
		OutError = TEXT("empty property path");
		return false;
	}

	UStruct* CurStruct = RootObject->GetClass();
	void* CurContainer = RootObject;

	for (int32 SegIdx = 0; SegIdx < Segments.Num(); ++SegIdx)
	{
		FString PropName;
		int32 ArrayIndex;
		if (!ParsePathSegment(Segments[SegIdx], PropName, ArrayIndex))
		{
			OutError = FString::Printf(TEXT("bad segment '%s'"), *Segments[SegIdx]);
			return false;
		}

		FProperty* Prop = FindFProperty<FProperty>(CurStruct, *PropName);
		if (!Prop)
		{
			OutError = FString::Printf(TEXT("property '%s' not found on %s"), *PropName, *CurStruct->GetName());
			return false;
		}

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CurContainer);

		if (ArrayIndex != INDEX_NONE)
		{
			if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
			{
				FScriptArrayHelper Helper(ArrayProp, ValuePtr);
				if (!Helper.IsValidIndex(ArrayIndex))
				{
					OutError = FString::Printf(TEXT("index %d out of range for '%s' (num=%d)"), ArrayIndex, *PropName, Helper.Num());
					return false;
				}
				Prop = ArrayProp->Inner;
				ValuePtr = Helper.GetRawPtr(ArrayIndex);
			}
			else if (Prop->ArrayDim > 1)
			{
				if (ArrayIndex >= Prop->ArrayDim)
				{
					OutError = FString::Printf(TEXT("index %d out of range for static array '%s' (dim=%d)"), ArrayIndex, *PropName, Prop->ArrayDim);
					return false;
				}
				ValuePtr = Prop->ContainerPtrToValuePtr<void>(CurContainer, ArrayIndex);
			}
			else
			{
				OutError = FString::Printf(TEXT("'%s' is not an array"), *PropName);
				return false;
			}
		}

		const bool bLast = (SegIdx == Segments.Num() - 1);
		if (bLast)
		{
			OutProp = Prop;
			OutValuePtr = ValuePtr;
			return true;
		}

		// 중간 세그먼트: struct / object 로 진입
		if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			CurStruct = StructProp->Struct;
			CurContainer = ValuePtr;
		}
		else if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			UObject* Inner = ObjProp->GetObjectPropertyValue(ValuePtr);
			if (!Inner)
			{
				OutError = FString::Printf(TEXT("'%s' is null - cannot descend"), *PropName);
				return false;
			}
			CurStruct = Inner->GetClass();
			CurContainer = Inner;
		}
		else
		{
			OutError = FString::Printf(TEXT("'%s' is not struct/object - cannot descend"), *PropName);
			return false;
		}
	}

	OutError = TEXT("unreachable");
	return false;
}

FString ExportPropertyByPath(UObject* RootObject, const FString& PropertyPath)
{
	FProperty* Prop;
	void* ValuePtr;
	FString Err;
	if (!ResolvePropertyPath(RootObject, PropertyPath, Prop, ValuePtr, Err))
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: ExportPropertyByPath 실패 (%s): %s"), *PropertyPath, *Err);
		return FString();
	}

	FString Out;
	Prop->ExportText_Direct(Out, ValuePtr, ValuePtr, nullptr, PPF_None);
	return Out;
}

bool ImportPropertyByPath(UObject* RootObject, const FString& PropertyPath, const FString& ValueText)
{
	FProperty* Prop;
	void* ValuePtr;
	FString Err;
	if (!ResolvePropertyPath(RootObject, PropertyPath, Prop, ValuePtr, Err))
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: ImportPropertyByPath 실패 (%s): %s"), *PropertyPath, *Err);
		return false;
	}

	// ObjectProperty 에 asset path 문자열을 넣는 경우 선로드 (ImportText 의 FindObject 실패 대비)
	if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
	{
		if (ValueText.StartsWith(TEXT("/")))
		{
			StaticLoadObject(ObjProp->PropertyClass, nullptr, *ValueText);
		}
	}

	RootObject->Modify();
	if (!Prop->ImportText_Direct(*ValueText, ValuePtr, RootObject, PPF_None))
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: ImportText 실패 (%s = %s)"), *PropertyPath, *ValueText);
		return false;
	}
	return true;
}

void DumpObjectEditProperties(const UObject* Obj, TArray<FString>& OutKVPairs)
{
	if (!Obj)
	{
		return;
	}

	for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit))
		{
			continue;
		}

		FString Value;
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);
		Prop->ExportText_Direct(Value, ValuePtr, ValuePtr, nullptr, PPF_None);
		if (Value.Len() > 512)
		{
			Value = Value.Left(512) + TEXT("...");
		}
		OutKVPairs.Add(Prop->GetName() + TEXT("=") + Value);
	}
}

void MarkAssetDirty(UObject* Asset)
{
	if (!Asset)
	{
		return;
	}
	Asset->Modify();
	Asset->MarkPackageDirty();
}

} // namespace BPICommon
