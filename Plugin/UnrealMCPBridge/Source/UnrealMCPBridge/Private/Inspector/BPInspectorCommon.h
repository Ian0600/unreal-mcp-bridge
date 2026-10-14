#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;

// BlueprintInspector 내부 공용 헬퍼 (Private 전용).
// LoadBlueprintFromPath 는 MCPInspectorLibrary.cpp 의 file-static 구현과 동일 로직의 사본 —
// "기존 파일 무변경" 원칙 때문에 의도적 중복 (통합은 다음 리팩터링 때).
namespace BPICommon
{
	/** "/Game/.../Foo.Foo" 또는 "/Game/.../Foo" → UObject. StaticLoadObject + EditorAssetLibrary cold-load fallback. */
	UObject* LoadAssetObject(const FString& AssetPath);

	template <typename T>
	T* LoadTypedAsset(const FString& AssetPath)
	{
		return Cast<T>(LoadAssetObject(AssetPath));
	}

	/** 일반 BP + Level BP fallback. (MCPInspectorLibrary.cpp 의 규약과 동일) */
	UBlueprint* LoadBlueprintFromPath(const FString& AssetPath);

	/** GetBlueprintGraphs / GetGraphNodesByIndex 와 동일 인덱스 규약: BP->GetAllGraphs 순서 / Graph->Nodes 순서 */
	UEdGraph* GetGraphByIndex(UBlueprint* BP, int32 GraphIndex);
	UEdGraphNode* GetNodeByIndex(UEdGraph* Graph, int32 NodeIndex);

	/**
	 * "A.B[2].C" 형태 프로퍼티 경로를 리플렉션으로 해석.
	 * 중간 세그먼트는 struct / object 로 진입, TArray 와 C-array 인덱싱 지원.
	 * 성공 시 최종 FProperty 와 값 포인터 반환.
	 */
	bool ResolvePropertyPath(UObject* RootObject, const FString& PropertyPath, FProperty*& OutProp, void*& OutValuePtr, FString& OutError);

	/** 경로의 값을 ExportText 문자열로. 실패 시 빈 문자열 + Warning 로그. */
	FString ExportPropertyByPath(UObject* RootObject, const FString& PropertyPath);

	/** 경로에 ImportText 로 값 기록. ObjectProperty 대상이고 ValueText 가 "/..." 면 선로드 시도. 호출 전 Modify 는 내부 수행. */
	bool ImportPropertyByPath(UObject* RootObject, const FString& PropertyPath, const FString& ValueText);

	/** 오브젝트의 CPF_Edit 프로퍼티를 "Name=Value" 줄로 덤프 (긴 값은 512자 절단). */
	void DumpObjectEditProperties(const UObject* Obj, TArray<FString>& OutKVPairs);

	/** Modify + MarkPackageDirty (저장은 호출자 몫 — 기존 규약). */
	void MarkAssetDirty(UObject* Asset);
}
