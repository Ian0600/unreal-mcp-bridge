#include "MCPInspectorLibrary.h"
#include "Runtime/Launch/Resources/Version.h"

// Cross-version compatibility (UE4 / UE5 < 5.6 vs UE5.6+)
//   - UE 5.6 부터 UBlendSpaceBase 제거 → UBlendSpace 로 통합
//   - UE 5.6 부터 FAnimNode_SequencePlayer / BlendSpacePlayer 의 멤버가 protected/private 화
//     → GetSequence() / GetBlendSpace() / GetLoop() / GetLoopAnimation() / GetPlayRate() / GetStartPosition() accessor 필수
#define BPI_UE_5_6_OR_LATER ((ENGINE_MAJOR_VERSION > 5) || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6))

#include "AnimGraphNode_BlendListByEnum.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Editor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallMaterialParameterCollectionFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Variable.h"
#include "EdGraphSchema_K2.h"
#include "Animation/AnimMontage.h"
#if !BPI_UE_5_6_OR_LATER
#include "Animation/BlendSpaceBase.h"
#endif
#include "Engine/SkeletalMesh.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EditorAssetLibrary.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/Skeleton.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimationStateMachineGraph.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateConduitNode.h"
#include "AnimationStateGraph.h"
#include "AnimationStateGraphSchema.h"
#include "AnimGraphNode_Slot.h"
#include "Animation/AnimNodeBase.h"
#include "AnimNodes/AnimNode_Slot.h"
#include "AnimationGraph.h"
#include "Animation/AnimNode_SequencePlayer.h"
#include "AnimNodes/AnimNode_BlendSpacePlayer.h"
#include "AlphaBlend.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "UObject/UnrealType.h"
#include "UObject/SoftObjectPtr.h"
// ─── Material inspector ───
#include "SceneTypes.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialFunctionInterface.h"
#include "BlueprintEditorLibrary.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
// ─── Niagara inspector ───
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraLightRendererProperties.h"
#include "NiagaraParameterCollection.h"
#include "NiagaraParameterCollectionFactoryNew.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraStackFunctionInputBinder.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "EdGraphSchema_Niagara.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"

// ─── 공통 헬퍼 ──────────────────────────────────────────────────────────────

static UBlueprint* LoadBlueprintFromPath(const FString& AssetPath)
{
	// 1) 일반 UBlueprint (Anim/Widget/Class BP 모두 UBlueprint 상속)
	if (UObject* LoadedObj = StaticLoadObject(UBlueprint::StaticClass(), nullptr, *AssetPath))
	{
		if (UBlueprint* BP = Cast<UBlueprint>(LoadedObj))
		{
			return BP;
		}
	}

	// 1b) cold-load fallback: StaticLoadObject 가 일부 cooked/registry 케이스에서
	//     실패하는 경우가 있어 EditorAssetLibrary 로 한번 더 시도.
	//     "/Game/.../Foo.Foo" 와 "/Game/.../Foo" 양쪽 모두 처리.
	{
		FString TryPath = AssetPath;
		if (UObject* LoadedObj = UEditorAssetLibrary::LoadAsset(TryPath))
		{
			if (UBlueprint* BP = Cast<UBlueprint>(LoadedObj))
			{
				return BP;
			}
		}
	}

	// 2) Level Blueprint fallback: AssetPath가 .umap(World) 인 경우.
	//    World 로드 → PersistentLevel → LevelScriptBlueprint 반환.
	//    bDontCreate=true 로 빈 LevelScript 생성을 회피 (read-only audit 용도).
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

static FString PinDirectionToString(EEdGraphPinDirection Dir)
{
	switch (Dir)
	{
	case EGPD_Input: return TEXT("Input");
	case EGPD_Output: return TEXT("Output");
	default: return TEXT("Unknown");
	}
}

static FString GraphTypeToString(EGraphType Type)
{
	switch (Type)
	{
	case GT_Function: return TEXT("Function");
	case GT_Ubergraph: return TEXT("Ubergraph");
	case GT_Macro: return TEXT("Macro");
	case GT_Animation: return TEXT("Animation");
	case GT_StateMachine: return TEXT("StateMachine");
	default: return TEXT("Unknown");
	}
}

static FString ContainerTypeToString(EPinContainerType Type)
{
	switch (Type)
	{
	case EPinContainerType::None:  return TEXT("None");
	case EPinContainerType::Array: return TEXT("Array");
	case EPinContainerType::Set:   return TEXT("Set");
	case EPinContainerType::Map:   return TEXT("Map");
	default: return TEXT("Unknown");
	}
}

static FMCPInspectorPinTypeInfo BuildPinTypeInfo(const FEdGraphPinType& T)
{
	FMCPInspectorPinTypeInfo Info;
	Info.Category    = T.PinCategory.ToString();
	Info.SubCategory = T.PinSubCategory.ToString();
	Info.SubCategoryObject = T.PinSubCategoryObject.IsValid() ? T.PinSubCategoryObject->GetPathName() : TEXT("");
	Info.ContainerType = ContainerTypeToString(T.ContainerType);
	Info.ValueCategory = T.PinValueType.TerminalCategory.ToString();
	Info.ValueSubCategoryObject = T.PinValueType.TerminalSubCategoryObject.IsValid()
		? T.PinValueType.TerminalSubCategoryObject->GetPathName() : TEXT("");
	Info.bIsReference = T.bIsReference;
	Info.bIsConst     = T.bIsConst;
	return Info;
}

static FMCPInspectorPinInfo BuildPinInfo(const UEdGraphPin* Pin)
{
	FMCPInspectorPinInfo Info;
	Info.PinName = Pin->PinName.ToString();
	Info.PinType = Pin->PinType.PinCategory.ToString();  // legacy
	Info.PinTypeFull = BuildPinTypeInfo(Pin->PinType);
	Info.Direction = PinDirectionToString(Pin->Direction);
	Info.DefaultValue = Pin->DefaultValue;
	Info.PinId = Pin->PinId.ToString();
	Info.bIsConnected = Pin->LinkedTo.Num() > 0;

	for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
	{
		if (LinkedPin && LinkedPin->GetOwningNode())
		{
			// 레거시 (NodeTitle 기반 — 멀티라인/비고유)
			FString LegacyStr = FString::Printf(TEXT("%s.%s"),
				*LinkedPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::FullTitle).ToString(),
				*LinkedPin->PinName.ToString());
			Info.ConnectedTo.Add(LegacyStr);

			// 신규 안정 ID (NodeName 기반)
			FString StableStr = FString::Printf(TEXT("%s.%s"),
				*LinkedPin->GetOwningNode()->GetName(),
				*LinkedPin->PinName.ToString());
			Info.ConnectedToStable.Add(StableStr);
		}
	}

	return Info;
}

static FMCPInspectorNodeInfo BuildNodeInfo(const UEdGraphNode* Node)
{
	FMCPInspectorNodeInfo Info;
	Info.NodeName = Node->GetName();
	Info.NodeClass = Node->GetClass()->GetName();
	Info.NodeTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
	Info.NodeComment = Node->NodeComment;
	Info.PosX = Node->NodePosX;
	Info.PosY = Node->NodePosY;
	Info.NodeGuid = Node->NodeGuid.ToString();
	Info.GraphName = Node->GetGraph() ? Node->GetGraph()->GetName() : TEXT("");

	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin)
		{
			Info.Pins.Add(BuildPinInfo(Pin));
		}
	}

	return Info;
}

static UEdGraphNode* FindNodeByName(UBlueprint* BP, const FString& NodeName)
{
	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->GetName() == NodeName)
			{
				return Node;
			}
		}
	}
	return nullptr;
}

static UEdGraphNode* FindNodeInGraph(UBlueprint* BP, const FString& GraphName, const FString& NodeName)
{
	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph || Graph->GetName() != GraphName) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->GetName() == NodeName)
			{
				return Node;
			}
		}
	}
	return nullptr;
}

static UEdGraphPin* FindPinOnNode(UEdGraphNode* Node, const FString& PinName)
{
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinName.ToString() == PinName)
		{
			return Pin;
		}
	}
	return nullptr;
}

static void MarkBlueprintModified(UBlueprint* BP)
{
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
}

// ─── Graph / Node 조회 ──────────────────────────────────────────────────────

TArray<FMCPInspectorGraphInfo> UMCPInspectorLibrary::GetBlueprintGraphs(const FString& AssetPath)
{
	TArray<FMCPInspectorGraphInfo> Result;

	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: Failed to load blueprint at %s"), *AssetPath);
		return Result;
	}

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	for (const UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;

		FMCPInspectorGraphInfo Info;
		Info.GraphName = Graph->GetName();
		Info.GraphType = GraphTypeToString(Graph->GetSchema()->GetGraphType(Graph));
		Info.NodeCount = Graph->Nodes.Num();
		Result.Add(Info);
	}

	return Result;
}


TArray<FMCPInspectorNodeInfo> UMCPInspectorLibrary::GetGraphNodesByIndex(const FString& AssetPath, int32 GraphIndex)
{
	TArray<FMCPInspectorNodeInfo> Result;

	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return Result;

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	if (!AllGraphs.IsValidIndex(GraphIndex)) return Result;

	const UEdGraph* Graph = AllGraphs[GraphIndex];
	if (!Graph) return Result;

	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node)
		{
			Result.Add(BuildNodeInfo(Node));
		}
	}

	return Result;
}

TArray<FMCPInspectorNodeInfo> UMCPInspectorLibrary::GetAllNodesInBlueprint(const FString& AssetPath)
{
	TArray<FMCPInspectorNodeInfo> Result;

	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: Failed to load blueprint at %s"), *AssetPath);
		return Result;
	}

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	for (const UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;

		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node)
			{
				Result.Add(BuildNodeInfo(Node));
			}
		}
	}

	return Result;
}

// ─── 핀 연결 / 해제 ─────────────────────────────────────────────────────────

bool UMCPInspectorLibrary::ConnectPins(const FString& AssetPath, const FString& SourceNodeName, const FString& SourcePinName, const FString& TargetNodeName, const FString& TargetPinName)
{
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return false;

	UEdGraphNode* SourceNode = FindNodeByName(BP, SourceNodeName);
	UEdGraphNode* TargetNode = FindNodeByName(BP, TargetNodeName);
	if (!SourceNode || !TargetNode) return false;

	UEdGraphPin* SourcePin = FindPinOnNode(SourceNode, SourcePinName);
	UEdGraphPin* TargetPin = FindPinOnNode(TargetNode, TargetPinName);
	if (!SourcePin || !TargetPin) return false;

	const UEdGraphSchema* Schema = SourceNode->GetGraph()->GetSchema();
	if (!Schema) return false;

	if (Schema->TryCreateConnection(SourcePin, TargetPin))
	{
		MarkBlueprintModified(BP);
		return true;
	}
	return false;
}

bool UMCPInspectorLibrary::DisconnectPin(const FString& AssetPath, const FString& NodeName, const FString& PinName)
{
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return false;

	UEdGraphNode* Node = FindNodeByName(BP, NodeName);
	if (!Node) return false;

	UEdGraphPin* Pin = FindPinOnNode(Node, PinName);
	if (!Pin) return false;

	Pin->BreakAllPinLinks();
	MarkBlueprintModified(BP);
	return true;
}

bool UMCPInspectorLibrary::ConnectPinsInGraph(const FString& AssetPath, const FString& GraphName, const FString& SourceNodeName, const FString& SourcePinName, const FString& TargetNodeName, const FString& TargetPinName)
{
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return false;

	UEdGraphNode* SourceNode = FindNodeInGraph(BP, GraphName, SourceNodeName);
	UEdGraphNode* TargetNode = FindNodeInGraph(BP, GraphName, TargetNodeName);
	if (!SourceNode || !TargetNode) return false;

	UEdGraphPin* SourcePin = FindPinOnNode(SourceNode, SourcePinName);
	UEdGraphPin* TargetPin = FindPinOnNode(TargetNode, TargetPinName);
	if (!SourcePin || !TargetPin) return false;

	const UEdGraphSchema* Schema = SourceNode->GetGraph()->GetSchema();
	if (!Schema) return false;

	if (Schema->TryCreateConnection(SourcePin, TargetPin))
	{
		MarkBlueprintModified(BP);
		return true;
	}
	return false;
}

// ─── 핀 기본값 설정 ─────────────────────────────────────────────────────────

bool UMCPInspectorLibrary::SetPinDefaultValue(const FString& AssetPath, const FString& NodeName, const FString& PinName, const FString& NewValue)
{
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return false;

	UEdGraphNode* Node = FindNodeByName(BP, NodeName);
	if (!Node) return false;

	UEdGraphPin* Pin = FindPinOnNode(Node, PinName);
	if (!Pin) return false;

	const UEdGraphSchema* Schema = Node->GetGraph()->GetSchema();
	if (Schema)
	{
		Schema->TrySetDefaultValue(*Pin, NewValue);
	}
	else
	{
		Pin->DefaultValue = NewValue;
	}

	MarkBlueprintModified(BP);
	return true;
}

bool UMCPInspectorLibrary::SetPinDefaultValueByGraphIndex(const FString& AssetPath, int32 GraphIndex, int32 NodeIndex, const FString& PinName, const FString& NewValue)
{
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return false;

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	if (!AllGraphs.IsValidIndex(GraphIndex)) return false;

	UEdGraph* Graph = AllGraphs[GraphIndex];
	if (!Graph || !Graph->Nodes.IsValidIndex(NodeIndex)) return false;

	UEdGraphNode* Node = Graph->Nodes[NodeIndex];
	if (!Node) return false;

	UEdGraphPin* Pin = FindPinOnNode(Node, PinName);
	if (!Pin) return false;

	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (Schema)
	{
		Schema->TrySetDefaultValue(*Pin, NewValue);
	}
	else
	{
		Pin->DefaultValue = NewValue;
	}

	MarkBlueprintModified(BP);
	return true;
}

// ─── 노드 조작 ──────────────────────────────────────────────────────────────

bool UMCPInspectorLibrary::RemoveNode(const FString& AssetPath, const FString& NodeName)
{
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return false;

	UEdGraphNode* Node = FindNodeByName(BP, NodeName);
	if (!Node) return false;

	UEdGraph* Graph = Node->GetGraph();
	if (!Graph) return false;

	FBlueprintEditorUtils::RemoveNode(BP, Node);
	return true;
}

FString UMCPInspectorLibrary::AddCallFunctionNode(const FString& AssetPath, const FString& GraphName, const FString& FunctionName, int32 PosX, int32 PosY)
{
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return TEXT("");

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	UEdGraph* TargetGraph = nullptr;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			TargetGraph = Graph;
			break;
		}
	}
	if (!TargetGraph) return TEXT("");

#if ENGINE_MAJOR_VERSION >= 5
	UFunction* Func = FindFirstObject<UFunction>(*FunctionName, EFindFirstObjectOptions::NativeFirst);
#else
	UFunction* Func = FindObject<UFunction>(ANY_PACKAGE, *FunctionName);
#endif
	if (!Func) return TEXT("");

	UK2Node_CallFunction* NewNode = NewObject<UK2Node_CallFunction>(TargetGraph);
	NewNode->SetFromFunction(Func);
	NewNode->NodePosX = PosX;
	NewNode->NodePosY = PosY;
	TargetGraph->AddNode(NewNode, false, false);
	NewNode->AllocateDefaultPins();

	MarkBlueprintModified(BP);
	return NewNode->GetName();
}

FString UMCPInspectorLibrary::AddVariableGetNode(const FString& AssetPath, const FString& GraphName, const FString& VarName, int32 PosX, int32 PosY)
{
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return TEXT("");

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	UEdGraph* TargetGraph = nullptr;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			TargetGraph = Graph;
			break;
		}
	}
	if (!TargetGraph) return TEXT("");

	UK2Node_VariableGet* NewNode = NewObject<UK2Node_VariableGet>(TargetGraph);
	NewNode->VariableReference.SetSelfMember(FName(*VarName));
	NewNode->NodePosX = PosX;
	NewNode->NodePosY = PosY;
	TargetGraph->AddNode(NewNode, false, false);
	NewNode->AllocateDefaultPins();

	MarkBlueprintModified(BP);
	return NewNode->GetName();
}

FString UMCPInspectorLibrary::AddVariableSetNode(const FString& AssetPath, const FString& GraphName, const FString& VarName, int32 PosX, int32 PosY)
{
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return TEXT("");

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	UEdGraph* TargetGraph = nullptr;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			TargetGraph = Graph;
			break;
		}
	}
	if (!TargetGraph) return TEXT("");

	UK2Node_VariableSet* NewNode = NewObject<UK2Node_VariableSet>(TargetGraph);
	NewNode->VariableReference.SetSelfMember(FName(*VarName));
	NewNode->NodePosX = PosX;
	NewNode->NodePosY = PosY;
	TargetGraph->AddNode(NewNode, false, false);
	NewNode->AllocateDefaultPins();

	MarkBlueprintModified(BP);
	return NewNode->GetName();
}

FString UMCPInspectorLibrary::AddArrayGetItemNode(const FString& AssetPath, const FString& GraphName, int32 PosX, int32 PosY)
{
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return TEXT("");

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	UEdGraph* TargetGraph = nullptr;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			TargetGraph = Graph;
			break;
		}
	}
	if (!TargetGraph) return TEXT("");

	UK2Node_GetArrayItem* NewNode = NewObject<UK2Node_GetArrayItem>(TargetGraph);
	NewNode->NodePosX = PosX;
	NewNode->NodePosY = PosY;
	TargetGraph->AddNode(NewNode, false, false);
	NewNode->AllocateDefaultPins();

	MarkBlueprintModified(BP);
	return NewNode->GetName();
}

bool UMCPInspectorLibrary::ReconstructNode(const FString& AssetPath, const FString& NodeName)
{
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return false;

	UEdGraphNode* Node = FindNodeByName(BP, NodeName);
	if (!Node) return false;

	Node->ReconstructNode();
	MarkBlueprintModified(BP);
	return true;
}

bool UMCPInspectorLibrary::ReconstructNodeByGraphIndex(const FString& AssetPath, int32 GraphIndex, int32 NodeIndex)
{
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return false;

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	if (!AllGraphs.IsValidIndex(GraphIndex)) return false;

	UEdGraph* Graph = AllGraphs[GraphIndex];
	if (!Graph || !Graph->Nodes.IsValidIndex(NodeIndex)) return false;

	UEdGraphNode* Node = Graph->Nodes[NodeIndex];
	if (!Node) return false;

	// AnimGraphNode_BlendListByEnum의 경우 RemovedPins를 초기화해야 구버전 enum 핀이 제거됨
	if (FProperty* RemovedPinsProp = Node->GetClass()->FindPropertyByName(FName("RemovedPins")))
	{
		if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(RemovedPinsProp))
		{
			FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(Node));
			Helper.EmptyValues();
		}
	}

	Node->ReconstructNode();
	FBlueprintEditorUtils::RefreshAllNodes(BP);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return true;
}

// ─── AnimGraph 노드 생성 ─────────────────────────────────────────────────────


// ─── State Machine 생성 ─────────────────────────────────────────────────────

static UAnimationStateMachineGraph* FindSMGraphByNodeName(UBlueprint* BP, const FString& SMNodeName)
{
	UEdGraphNode* Node = FindNodeByName(BP, SMNodeName);
	UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node);
	if (SMNode)
	{
		return SMNode->EditorStateMachineGraph;
	}
	return nullptr;
}

static UAnimStateNode* FindStateByName(UAnimationStateMachineGraph* SMGraph, const FString& StateName)
{
	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		UAnimStateNode* State = Cast<UAnimStateNode>(Node);
		if (State && State->BoundGraph && State->BoundGraph->GetName() == StateName)
		{
			return State;
		}
	}
	return nullptr;
}





int32 UMCPInspectorLibrary::SetAllTransitionsAutomatic(const FString& AssetPath, const FString& SMNodeName)
{
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return 0;

	UAnimationStateMachineGraph* SMGraph = FindSMGraphByNodeName(BP, SMNodeName);
	if (!SMGraph) return 0;

	int32 Count = 0;
	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(Node);
		if (TransNode)
		{
			TransNode->bAutomaticRuleBasedOnSequencePlayerInState = true;
			Count++;
		}
	}

	if (Count > 0)
	{
		MarkBlueprintModified(BP);
		UE_LOG(LogTemp, Log, TEXT("BlueprintInspector: Set %d transitions to automatic rule"), Count);
	}
	return Count;
}

// ─── BlendListByEnum 관리 ────────────────────────────────────────────────────

int32 UMCPInspectorLibrary::FixBlendListByEnumNodes(const FString& AssetPath, const FString& EnumPath)
{
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return -1;

	UEnum* TargetEnum = FindObject<UEnum>(nullptr, *EnumPath);
	if (!TargetEnum) return -2;

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	int32 FixedCount = 0;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimGraphNode_BlendListByEnum* BlendNode = Cast<UAnimGraphNode_BlendListByEnum>(Node);
			if (!BlendNode) continue;
			if (BlendNode->GetEnum() != TargetEnum) continue;

			// VisibleEnumEntries에서 현재 Enum에 없는 구버전 항목 제거
			if (FArrayProperty* VisibleProp = CastField<FArrayProperty>(
				BlendNode->GetClass()->FindPropertyByName(FName("VisibleEnumEntries"))))
			{
				FScriptArrayHelper Helper(VisibleProp, VisibleProp->ContainerPtrToValuePtr<void>(BlendNode));
				for (int32 i = Helper.Num() - 1; i >= 0; --i)
				{
					FName* EntryName = reinterpret_cast<FName*>(Helper.GetRawPtr(i));
					if (EntryName && TargetEnum->GetIndexByName(*EntryName) == INDEX_NONE)
					{
						Helper.RemoveValues(i, 1);
					}
				}
			}

			BlendNode->ReconstructNode();
			++FixedCount;
		}
	}

	if (FixedCount > 0)
	{
		FBlueprintEditorUtils::RefreshAllNodes(BP);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	}

	return FixedCount;
}

int32 UMCPInspectorLibrary::RebuildBlendListEnumEntries(const FString& AssetPath, const FString& EnumPath, const TArray<FString>& EntryNames)
{
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return -1;

	UEnum* TargetEnum = FindObject<UEnum>(nullptr, *EnumPath);
	if (!TargetEnum) return -2;

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	int32 FixedCount = 0;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimGraphNode_BlendListByEnum* BlendNode = Cast<UAnimGraphNode_BlendListByEnum>(Node);
			if (!BlendNode) continue;
			if (BlendNode->GetEnum() != TargetEnum) continue;

			FArrayProperty* VisibleProp = CastField<FArrayProperty>(
				BlendNode->GetClass()->FindPropertyByName(FName("VisibleEnumEntries")));
			if (!VisibleProp) continue;

			FScriptArrayHelper Helper(VisibleProp, VisibleProp->ContainerPtrToValuePtr<void>(BlendNode));
			Helper.EmptyValues();
			for (const FString& EntryName : EntryNames)
			{
				int32 Idx = Helper.AddValue();
				FName* NamePtr = reinterpret_cast<FName*>(Helper.GetRawPtr(Idx));
				if (NamePtr) *NamePtr = FName(*EntryName);
			}

			BlendNode->ReconstructNode();
			++FixedCount;
		}
	}

	if (FixedCount > 0)
	{
		FBlueprintEditorUtils::RefreshAllNodes(BP);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	}

	return FixedCount;
}

bool UMCPInspectorLibrary::SetBlendListEnumEntriesByIndex(const FString& AssetPath, int32 GraphIndex, int32 NodeIndex, const TArray<FString>& EntryNames)
{
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return false;

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	if (!AllGraphs.IsValidIndex(GraphIndex)) return false;

	UEdGraph* Graph = AllGraphs[GraphIndex];
	if (!Graph || !Graph->Nodes.IsValidIndex(NodeIndex)) return false;

	UAnimGraphNode_BlendListByEnum* BlendNode = Cast<UAnimGraphNode_BlendListByEnum>(Graph->Nodes[NodeIndex]);
	if (!BlendNode) return false;

	// RemovedPins 클리어 (구버전 enum 핀 잔재 제거)
	if (FArrayProperty* RemovedProp = CastField<FArrayProperty>(
		BlendNode->GetClass()->FindPropertyByName(FName("RemovedPins"))))
	{
		FScriptArrayHelper Helper(RemovedProp, RemovedProp->ContainerPtrToValuePtr<void>(BlendNode));
		Helper.EmptyValues();
	}

	// VisibleEnumEntries 완전 교체
	if (FArrayProperty* VisibleProp = CastField<FArrayProperty>(
		BlendNode->GetClass()->FindPropertyByName(FName("VisibleEnumEntries"))))
	{
		FScriptArrayHelper Helper(VisibleProp, VisibleProp->ContainerPtrToValuePtr<void>(BlendNode));
		Helper.EmptyValues();
		for (const FString& EntryName : EntryNames)
		{
			int32 Idx = Helper.AddValue();
			FName* NamePtr = reinterpret_cast<FName*>(Helper.GetRawPtr(Idx));
			if (NamePtr) *NamePtr = FName(*EntryName);
		}
	}

	BlendNode->ReconstructNode();
	FBlueprintEditorUtils::RefreshAllNodes(BP);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return true;
}

TArray<FString> UMCPInspectorLibrary::DumpBlendListEnumInfo(const FString& AssetPath)
{
	TArray<FString> Results;
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) { Results.Add(TEXT("ERROR: BP not found")); return Results; }

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	for (int32 gi = 0; gi < AllGraphs.Num(); ++gi)
	{
		UEdGraph* Graph = AllGraphs[gi];
		if (!Graph) continue;
		for (int32 ni = 0; ni < Graph->Nodes.Num(); ++ni)
		{
			UAnimGraphNode_BlendListByEnum* BlendNode = Cast<UAnimGraphNode_BlendListByEnum>(Graph->Nodes[ni]);
			if (!BlendNode) continue;

			UEnum* BoundEnum = BlendNode->GetEnum();
			FString EnumName = BoundEnum ? BoundEnum->GetName() : TEXT("None");

			TArray<FName> Entries;
			if (FArrayProperty* VisibleProp = CastField<FArrayProperty>(
				BlendNode->GetClass()->FindPropertyByName(FName("VisibleEnumEntries"))))
			{
				FScriptArrayHelper Helper(VisibleProp, VisibleProp->ContainerPtrToValuePtr<void>(BlendNode));
				for (int32 i = 0; i < Helper.Num(); ++i)
				{
					FName* NamePtr = reinterpret_cast<FName*>(Helper.GetRawPtr(i));
					if (NamePtr) Entries.Add(*NamePtr);
				}
			}

			TArray<FString> EntryStrings;
			for (const FName& E : Entries) EntryStrings.Add(E.ToString());

			Results.Add(FString::Printf(TEXT("g[%d]=%s n[%d] enum=%s entries=[%s] pin_count=%d"),
				gi, *Graph->GetName(), ni, *EnumName,
				*FString::Join(EntryStrings, TEXT(",")),
				BlendNode->Pins.Num()));
		}
	}
	return Results;
}

// ─── 컴파일 / 저장 ──────────────────────────────────────────────────────────



// ─── 네스티드 State Machine ──────────────────────────────────────────────────

FString UMCPInspectorLibrary::AddNestedStateMachine(
	const FString& AssetPath,
	const FString& ParentSMNodeName,
	const FString& ParentStateName,
	const FString& NestedMachineName,
	int32 PosX,
	int32 PosY)
{
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return TEXT("");

	UAnimationStateMachineGraph* ParentSMGraph = FindSMGraphByNodeName(BP, ParentSMNodeName);
	if (!ParentSMGraph)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: 부모 SM 노드 '%s' 를 찾을 수 없습니다"), *ParentSMNodeName);
		return TEXT("");
	}

	UAnimStateNode* ParentState = FindStateByName(ParentSMGraph, ParentStateName);
	if (!ParentState || !ParentState->BoundGraph)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: State '%s' 가 없거나 BoundGraph 가 없습니다"), *ParentStateName);
		return TEXT("");
	}

	UEdGraph* StateInnerGraph = ParentState->BoundGraph;

	// 같은 이름의 네스티드 SM 중복 방지
	for (UEdGraphNode* N : StateInnerGraph->Nodes)
	{
		UAnimGraphNode_StateMachine* Existing = Cast<UAnimGraphNode_StateMachine>(N);
		if (Existing && Existing->EditorStateMachineGraph &&
			Existing->EditorStateMachineGraph->GetName() == NestedMachineName)
		{
			UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: 네스티드 SM '%s' 이(가) State '%s' 안에 이미 존재합니다"),
				*NestedMachineName, *ParentStateName);
			return TEXT("");
		}
	}

	// State 내부 그래프에 네스티드 SM 노드 생성
	UAnimGraphNode_StateMachine* NestedSM = NewObject<UAnimGraphNode_StateMachine>(StateInnerGraph);
	NestedSM->NodePosX = PosX;
	NestedSM->NodePosY = PosY;
	StateInnerGraph->AddNode(NestedSM, false, false);
	NestedSM->PostPlacedNewNode();
	NestedSM->AllocateDefaultPins();

	// 네스티드 SM 그래프 이름 설정
	if (NestedSM->EditorStateMachineGraph && !NestedMachineName.IsEmpty())
	{
		NestedSM->EditorStateMachineGraph->Rename(*NestedMachineName);
	}

	// 네스티드 SM 출력 포즈 → StateResult.Result 자동 연결
	// 이미 연결된 노드가 있으면 먼저 끊음
	for (UEdGraphNode* N : StateInnerGraph->Nodes)
	{
		if (N == NestedSM) continue;
		if (!N->GetClass()->GetName().Contains(TEXT("StateResult"))) continue;

		UEdGraphPin* ResultPin = FindPinOnNode(N, TEXT("Result"));
		if (!ResultPin || ResultPin->Direction != EGPD_Input) continue;

		for (UEdGraphPin* Pin : NestedSM->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output)
			{
				ResultPin->BreakAllPinLinks();
				StateInnerGraph->GetSchema()->TryCreateConnection(Pin, ResultPin);
				break;
			}
		}
		break;
	}

	MarkBlueprintModified(BP);
	UE_LOG(LogTemp, Log, TEXT("BlueprintInspector: 네스티드 SM '%s' 생성 완료 (State: '%s') → %s"),
		*NestedMachineName, *ParentStateName, *NestedSM->GetName());
	return NestedSM->GetName();
}

// ─── 데이터 추출 (read-only) ───────────────────────────────────────────────

FMCPInspectorAnimBPHeader UMCPInspectorLibrary::GetAnimBlueprintHeader(const FString& AssetPath)
{
	FMCPInspectorAnimBPHeader Out;
	Out.AssetPath = AssetPath;

	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return Out;

	if (BP->ParentClass)
	{
		Out.ParentClass = BP->ParentClass->GetPathName();
	}
	if (BP->GeneratedClass)
	{
		Out.GeneratedClass = BP->GeneratedClass->GetPathName();
	}
	switch (BP->BlueprintType)
	{
	case BPTYPE_Normal:           Out.BlueprintType = TEXT("Normal"); break;
	case BPTYPE_Const:            Out.BlueprintType = TEXT("Const"); break;
	case BPTYPE_MacroLibrary:     Out.BlueprintType = TEXT("MacroLibrary"); break;
	case BPTYPE_Interface:        Out.BlueprintType = TEXT("Interface"); break;
	case BPTYPE_LevelScript:      Out.BlueprintType = TEXT("LevelScript"); break;
	case BPTYPE_FunctionLibrary:  Out.BlueprintType = TEXT("FunctionLibrary"); break;
	default:                      Out.BlueprintType = TEXT("Unknown"); break;
	}

	if (UAnimBlueprint* ABP = Cast<UAnimBlueprint>(BP))
	{
		if (ABP->TargetSkeleton)
		{
			Out.TargetSkeleton = ABP->TargetSkeleton->GetPathName();
		}
	}

	for (const FBPInterfaceDescription& Iface : BP->ImplementedInterfaces)
	{
		if (Iface.Interface)
		{
			Out.ImplementedInterfaces.Add(Iface.Interface->GetPathName());
		}
	}

	UObject* CDO = (BP->GeneratedClass) ? BP->GeneratedClass->GetDefaultObject(/*bCreateIfNeeded*/ false) : nullptr;
	for (const FBPVariableDescription& V : BP->NewVariables)
	{
		FMCPInspectorVariableInfo Info;
		Info.Name = V.VarName.ToString();
		Info.Type = BuildPinTypeInfo(V.VarType);
		Info.DefaultValue = V.DefaultValue;
		Info.Category = V.Category.ToString();
		Info.Guid = V.VarGuid.ToString();
		Info.PropertyFlags = static_cast<int64>(V.PropertyFlags);

		// MetaData 에서 Tooltip
		for (const FBPVariableMetaDataEntry& E : V.MetaDataArray)
		{
			if (E.DataKey == FName("ToolTip"))
			{
				Info.Tooltip = E.DataValue;
				break;
			}
		}

		// Replication
		Info.bIsReplicated = (V.PropertyFlags & CPF_Net) != 0;
		Info.RepNotifyFunction = V.RepNotifyFunc.ToString();

		// CDO 기반 실제 기본값 (FProperty::ExportText)
		if (CDO)
		{
			if (FProperty* Prop = BP->GeneratedClass->FindPropertyByName(V.VarName))
			{
				FString Exported;
				Prop->ExportText_InContainer(0, Exported, CDO, CDO, CDO, PPF_None);
				Info.CDODefault = Exported;
			}
		}

		Out.Variables.Add(Info);
	}

	for (const UEdGraph* G : BP->FunctionGraphs)
	{
		if (G) Out.FunctionGraphNames.Add(G->GetName());
	}
	for (const UEdGraph* G : BP->MacroGraphs)
	{
		if (G) Out.MacroGraphNames.Add(G->GetName());
	}
	for (const UEdGraph* G : BP->UbergraphPages)
	{
		if (G) Out.UbergraphNames.Add(G->GetName());
	}

	return Out;
}

static void FillAnimGraphNodeDetail(const UEdGraphNode* Node, FMCPInspectorAnimGraphNodeDetail& Out)
{
	Out.GraphName = Node->GetGraph() ? Node->GetGraph()->GetName() : TEXT("");
	Out.NodeName  = Node->GetName();
	Out.NodeClass = Node->GetClass()->GetName();
	Out.NodeGuid  = Node->NodeGuid.ToString();

	if (const UAnimGraphNode_SequencePlayer* SP = Cast<UAnimGraphNode_SequencePlayer>(Node))
	{
		const FAnimNode_SequencePlayer& N = SP->Node;
#if BPI_UE_5_6_OR_LATER
		if (N.GetSequence()) Out.AnimAssetPath = N.GetSequence()->GetPathName();
		Out.bLoop = N.IsLooping();
		Out.PlayRate = N.GetPlayRate();
		Out.ExtraKVPairs.Add(FString::Printf(TEXT("StartPosition=%.4f"), N.GetStartPosition()));
#else
		if (N.Sequence) Out.AnimAssetPath = N.Sequence->GetPathName();
		Out.bLoop = N.bLoopAnimation;
		Out.PlayRate = N.PlayRate;
		Out.ExtraKVPairs.Add(FString::Printf(TEXT("StartPosition=%.4f"), N.StartPosition));
#endif
	}
	else if (const UAnimGraphNode_BlendSpacePlayer* BSP = Cast<UAnimGraphNode_BlendSpacePlayer>(Node))
	{
		const FAnimNode_BlendSpacePlayer& N = BSP->Node;
#if BPI_UE_5_6_OR_LATER
		if (N.GetBlendSpace()) Out.AnimAssetPath = N.GetBlendSpace()->GetPathName();
		Out.bLoop = N.IsLooping();
		Out.PlayRate = N.GetPlayRate();
#else
		if (N.BlendSpace) Out.AnimAssetPath = N.BlendSpace->GetPathName();
		Out.bLoop = N.bLoop;
		Out.PlayRate = N.PlayRate;
#endif
	}
	else if (const UAnimGraphNode_Slot* SlotNode = Cast<UAnimGraphNode_Slot>(Node))
	{
		const FAnimNode_Slot& N = SlotNode->Node;
		Out.SlotName = N.SlotName.ToString();
	}
	else if (const UAnimGraphNode_StateMachineBase* SM = Cast<UAnimGraphNode_StateMachineBase>(Node))
	{
		if (SM->EditorStateMachineGraph)
		{
			Out.StateMachineGraphName = SM->EditorStateMachineGraph->GetName();
		}
	}
}

TArray<FMCPInspectorAnimGraphNodeDetail> UMCPInspectorLibrary::DumpAnimGraphNodeDetails(const FString& AssetPath)
{
	TArray<FMCPInspectorAnimGraphNodeDetail> Result;
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return Result;

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	for (const UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (!Node->IsA(UAnimGraphNode_Base::StaticClass()) &&
			    !Node->IsA(UAnimStateNodeBase::StaticClass())) continue;

			FMCPInspectorAnimGraphNodeDetail D;
			FillAnimGraphNodeDetail(Node, D);
			Result.Add(D);
		}
	}
	return Result;
}

static FString AlphaBlendOptionToString(EAlphaBlendOption Opt)
{
	switch (Opt)
	{
	case EAlphaBlendOption::Linear:        return TEXT("Linear");
	case EAlphaBlendOption::Cubic:         return TEXT("Cubic");
	case EAlphaBlendOption::HermiteCubic:  return TEXT("HermiteCubic");
	case EAlphaBlendOption::Sinusoidal:    return TEXT("Sinusoidal");
	case EAlphaBlendOption::QuadraticInOut:return TEXT("QuadraticInOut");
	case EAlphaBlendOption::CubicInOut:    return TEXT("CubicInOut");
	case EAlphaBlendOption::QuarticInOut:  return TEXT("QuarticInOut");
	case EAlphaBlendOption::QuinticInOut:  return TEXT("QuinticInOut");
	case EAlphaBlendOption::CircularIn:    return TEXT("CircularIn");
	case EAlphaBlendOption::CircularOut:   return TEXT("CircularOut");
	case EAlphaBlendOption::CircularInOut: return TEXT("CircularInOut");
	case EAlphaBlendOption::ExpIn:         return TEXT("ExpIn");
	case EAlphaBlendOption::ExpOut:        return TEXT("ExpOut");
	case EAlphaBlendOption::ExpInOut:      return TEXT("ExpInOut");
	case EAlphaBlendOption::Custom:        return TEXT("Custom");
	default:                               return TEXT("Unknown");
	}
}

// 그래프 → 인덱스 매핑 (UBlueprint::GetAllGraphs 결과의 인덱스)
static int32 FindGraphIndex(const TArray<UEdGraph*>& AllGraphs, const UEdGraph* Target)
{
	if (!Target) return -1;
	for (int32 i = 0; i < AllGraphs.Num(); ++i)
	{
		if (AllGraphs[i] == Target) return i;
	}
	return -1;
}

static void BuildOneSMStructure(UBlueprint* BP, UAnimGraphNode_StateMachineBase* SM, FMCPInspectorStateMachineStructure& Out)
{
	if (!SM) return;
	Out.SMNodeName = SM->GetName();
	UAnimationStateMachineGraph* SMGraph = SM->EditorStateMachineGraph;
	if (!SMGraph) return;
	Out.MachineGraphName = SMGraph->GetName();

	TArray<UEdGraph*> AllGraphs;
	if (BP) BP->GetAllGraphs(AllGraphs);

	for (UEdGraphNode* N : SMGraph->Nodes)
	{
		if (UAnimStateEntryNode* Entry = Cast<UAnimStateEntryNode>(N))
		{
			for (UEdGraphPin* OutPin : Entry->Pins)
			{
				if (OutPin && OutPin->Direction == EGPD_Output)
				{
					for (UEdGraphPin* Linked : OutPin->LinkedTo)
					{
						if (Linked && Linked->GetOwningNode() &&
						    Linked->GetOwningNode()->IsA(UAnimStateNodeBase::StaticClass()))
						{
							Out.EntryStateName = Cast<UAnimStateNodeBase>(Linked->GetOwningNode())->GetStateName();
							break;
						}
					}
				}
			}
			continue;
		}

		if (UAnimStateNodeBase* SNode = Cast<UAnimStateNodeBase>(N))
		{
			if (SNode->IsA(UAnimStateTransitionNode::StaticClass())) continue;

			FMCPInspectorStateInfo S;
			S.StateName = SNode->GetStateName();
			S.StateClass = SNode->GetClass()->GetName();
			S.NodeGuid = SNode->NodeGuid.ToString();
			if (UEdGraph* Bound = SNode->GetBoundGraph())
			{
				S.BoundGraphName = Bound->GetName();
				S.BoundGraphIndex = FindGraphIndex(AllGraphs, Bound);
			}
			Out.States.Add(S);
		}
	}

	for (UEdGraphNode* N : SMGraph->Nodes)
	{
		UAnimStateTransitionNode* T = Cast<UAnimStateTransitionNode>(N);
		if (!T) continue;

		FMCPInspectorTransitionInfo TI;
		TI.TransitionName = T->GetName();
		TI.NodeGuid       = T->NodeGuid.ToString();
		if (UAnimStateNodeBase* From = T->GetPreviousState()) TI.FromStateName = From->GetStateName();
		if (UAnimStateNodeBase* To   = T->GetNextState())     TI.ToStateName   = To->GetStateName();
		TI.CrossfadeDuration = T->CrossfadeDuration;
		TI.BlendMode = AlphaBlendOptionToString(T->BlendMode);
		TI.PriorityOrder = T->PriorityOrder;
		TI.bAutomaticRule = T->bAutomaticRuleBasedOnSequencePlayerInState;
		if (UEdGraph* Bound = T->GetBoundGraph())
		{
			TI.BoundGraphName = Bound->GetName();
			TI.BoundGraphIndex = FindGraphIndex(AllGraphs, Bound);
		}
		Out.Transitions.Add(TI);
	}
}

FMCPInspectorStateMachineStructure UMCPInspectorLibrary::DumpStateMachineStructure(const FString& AssetPath, const FString& SMNodeName)
{
	FMCPInspectorStateMachineStructure Out;
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return Out;

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimGraphNode_StateMachineBase* SM = Cast<UAnimGraphNode_StateMachineBase>(Node);
			if (!SM) continue;
			if (!SMNodeName.IsEmpty() && SM->GetName() != SMNodeName) continue;
			BuildOneSMStructure(BP, SM, Out);
			return Out;
		}
	}
	return Out;
}

TArray<FMCPInspectorStateMachineStructure> UMCPInspectorLibrary::DumpAllStateMachineStructures(const FString& AssetPath)
{
	TArray<FMCPInspectorStateMachineStructure> Result;
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return Result;

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimGraphNode_StateMachineBase* SM = Cast<UAnimGraphNode_StateMachineBase>(Node);
			if (!SM) continue;
			FMCPInspectorStateMachineStructure One;
			BuildOneSMStructure(BP, SM, One);
			Result.Add(One);
		}
	}
	return Result;
}

// ─── 평탄 인덱스 (검색용) ──────────────────────────────────────────────────

TArray<FMCPInspectorAssetRef> UMCPInspectorLibrary::DumpAssetReferences(const FString& AssetPath)
{
	TArray<FMCPInspectorAssetRef> Result;
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return Result;

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	auto MakeRef = [](const UEdGraphNode* N, const FString& Cat, const FString& Path)
	{
		FMCPInspectorAssetRef R;
		R.Category = Cat;
		R.AssetPath = Path;
		R.GraphName = N->GetGraph() ? N->GetGraph()->GetName() : TEXT("");
		R.NodeName = N->GetName();
		R.NodeClass = N->GetClass()->GetName();
		R.NodeGuid = N->NodeGuid.ToString();
		return R;
	};

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (const UAnimGraphNode_SequencePlayer* SP = Cast<UAnimGraphNode_SequencePlayer>(Node))
			{
#if BPI_UE_5_6_OR_LATER
				if (SP->Node.GetSequence()) Result.Add(MakeRef(Node, TEXT("Sequence"), SP->Node.GetSequence()->GetPathName()));
#else
				if (SP->Node.Sequence) Result.Add(MakeRef(Node, TEXT("Sequence"), SP->Node.Sequence->GetPathName()));
#endif
			}
			else if (const UAnimGraphNode_BlendSpacePlayer* BSP = Cast<UAnimGraphNode_BlendSpacePlayer>(Node))
			{
#if BPI_UE_5_6_OR_LATER
				if (BSP->Node.GetBlendSpace()) Result.Add(MakeRef(Node, TEXT("BlendSpace"), BSP->Node.GetBlendSpace()->GetPathName()));
#else
				if (BSP->Node.BlendSpace) Result.Add(MakeRef(Node, TEXT("BlendSpace"), BSP->Node.BlendSpace->GetPathName()));
#endif
			}
			else if (const UAnimGraphNode_Slot* SlotN = Cast<UAnimGraphNode_Slot>(Node))
			{
				const FString Sn = SlotN->Node.SlotName.ToString();
				if (!Sn.IsEmpty()) Result.Add(MakeRef(Node, TEXT("Slot"), Sn));
			}

			// CallFunction 인자 default 의 ObjectProperty 도 자산 참조로 캡처
			if (const UK2Node_CallFunction* CF = Cast<UK2Node_CallFunction>(Node))
			{
				for (UEdGraphPin* Pin : CF->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Input) continue;
					if (Pin->LinkedTo.Num() > 0) continue;
					if (Pin->DefaultObject)
					{
						UClass* Cls = Pin->DefaultObject->GetClass();
						FString Cat = TEXT("Other");
						if (Cls->IsChildOf(UAnimSequence::StaticClass()))           Cat = TEXT("Sequence");
#if BPI_UE_5_6_OR_LATER
						else if (Cls->IsChildOf(UBlendSpace::StaticClass()))        Cat = TEXT("BlendSpace");
#else
						else if (Cls->IsChildOf(UBlendSpaceBase::StaticClass()))    Cat = TEXT("BlendSpace");
#endif
						else if (Cls->IsChildOf(UAnimMontage::StaticClass()))       Cat = TEXT("Montage");
						Result.Add(MakeRef(Node, Cat, Pin->DefaultObject->GetPathName()));
					}
				}
			}
		}
	}
	return Result;
}

TArray<FMCPInspectorFunctionCallRef> UMCPInspectorLibrary::DumpFunctionCalls(const FString& AssetPath)
{
	TArray<FMCPInspectorFunctionCallRef> Result;
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return Result;

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			const UK2Node_CallFunction* CF = Cast<UK2Node_CallFunction>(Node);
			if (!CF) continue;

			FMCPInspectorFunctionCallRef R;
			const FMemberReference& Ref = CF->FunctionReference;
			R.FunctionName = Ref.GetMemberName().ToString();
			if (UClass* TargetClass = Ref.GetMemberParentClass())
			{
				R.TargetClass = TargetClass->GetPathName();
			}
			R.GraphName = Graph->GetName();
			R.NodeName = CF->GetName();
			R.NodeGuid = CF->NodeGuid.ToString();
			R.bIsPure = CF->IsNodePure();
			R.bIsParent = CF->IsA(UK2Node_CallParentFunction::StaticClass());
			Result.Add(R);
		}
	}
	return Result;
}

TArray<FMCPInspectorVariableUsageRef> UMCPInspectorLibrary::DumpVariableUsages(const FString& AssetPath)
{
	TArray<FMCPInspectorVariableUsageRef> Result;
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return Result;

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	auto FillVarRef = [](const UK2Node_Variable* VN, const FString& Access, FMCPInspectorVariableUsageRef& R)
	{
		const FMemberReference& Ref = VN->VariableReference;
		R.VariableName = Ref.GetMemberName().ToString();
		R.AccessType = Access;
		if (Ref.IsSelfContext())
		{
			R.MemberScope = TEXT("Self");
		}
		else if (UClass* Outer = Ref.GetMemberParentClass())
		{
			R.MemberScope = Outer->GetPathName();
		}
		R.GraphName = VN->GetGraph() ? VN->GetGraph()->GetName() : TEXT("");
		R.NodeName = VN->GetName();
		R.NodeGuid = VN->NodeGuid.ToString();
	};

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (const UK2Node_VariableGet* VG = Cast<UK2Node_VariableGet>(Node))
			{
				FMCPInspectorVariableUsageRef R; FillVarRef(VG, TEXT("Get"), R); Result.Add(R);
			}
			else if (const UK2Node_VariableSet* VS = Cast<UK2Node_VariableSet>(Node))
			{
				FMCPInspectorVariableUsageRef R; FillVarRef(VS, TEXT("Set"), R); Result.Add(R);
			}
		}
	}
	return Result;
}

TArray<FMCPInspectorEventEntry> UMCPInspectorLibrary::DumpEventEntries(const FString& AssetPath)
{
	TArray<FMCPInspectorEventEntry> Result;
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return Result;

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			FMCPInspectorEventEntry E;
			E.GraphName = Graph->GetName();
			E.NodeName = Node->GetName();
			E.NodeGuid = Node->NodeGuid.ToString();
			E.NodeClass = Node->GetClass()->GetName();

			if (const UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
			{
				E.EventName = CE->CustomFunctionName.ToString();
				E.EventOwnerClass = TEXT("Self");
				Result.Add(E);
			}
			else if (const UK2Node_Event* EV = Cast<UK2Node_Event>(Node))
			{
				const FMemberReference& Ref = EV->EventReference;
				E.EventName = Ref.GetMemberName().ToString();
				if (UClass* Owner = Ref.GetMemberParentClass())
				{
					E.EventOwnerClass = Owner->GetPathName();
				}
				Result.Add(E);
			}
			else if (const UK2Node_FunctionEntry* FE = Cast<UK2Node_FunctionEntry>(Node))
			{
				E.EventName = FE->CustomGeneratedFunctionName.IsNone()
					? Graph->GetName()
					: FE->CustomGeneratedFunctionName.ToString();
				E.EventOwnerClass = TEXT("Self");
				Result.Add(E);
			}
		}
	}
	return Result;
}

TArray<FMCPInspectorFunctionSignature> UMCPInspectorLibrary::DumpFunctionSignatures(const FString& AssetPath)
{
	TArray<FMCPInspectorFunctionSignature> Result;
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return Result;

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	auto FillParamsFromEntry = [](const UK2Node_FunctionEntry* Entry, FMCPInspectorFunctionSignature& Sig)
	{
		if (!Entry) return;
		for (UEdGraphPin* Pin : Entry->Pins)
		{
			if (!Pin) continue;
			if (Pin->PinName == UEdGraphSchema_K2::PN_Then) continue;
			if (Pin->PinName == UEdGraphSchema_K2::PN_Execute) continue;
			if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;
			if (Pin->Direction != EGPD_Output) continue;  // FunctionEntry 의 출력 핀 = 함수 입력 파라미터
			FMCPInspectorFunctionParam P;
			P.Name = Pin->PinName.ToString();
			P.Type = BuildPinTypeInfo(Pin->PinType);
			P.Direction = TEXT("Input");
			P.DefaultValue = Pin->DefaultValue;
			Sig.Inputs.Add(P);
		}
	};
	auto FillParamsFromResult = [](const UK2Node_FunctionResult* Res, FMCPInspectorFunctionSignature& Sig)
	{
		if (!Res) return;
		for (UEdGraphPin* Pin : Res->Pins)
		{
			if (!Pin) continue;
			if (Pin->PinName == UEdGraphSchema_K2::PN_Then) continue;
			if (Pin->PinName == UEdGraphSchema_K2::PN_Execute) continue;
			if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;
			if (Pin->Direction != EGPD_Input) continue;  // FunctionResult 의 입력 핀 = 함수 출력 파라미터
			FMCPInspectorFunctionParam P;
			P.Name = Pin->PinName.ToString();
			P.Type = BuildPinTypeInfo(Pin->PinType);
			P.Direction = TEXT("Output");
			P.DefaultValue = Pin->DefaultValue;
			Sig.Outputs.Add(P);
		}
	};

	int32 GraphIdx = -1;
	for (UEdGraph* Graph : AllGraphs)
	{
		++GraphIdx;
		if (!Graph) continue;
		const EGraphType GT = Graph->GetSchema()->GetGraphType(Graph);
		if (GT != GT_Function && GT != GT_Macro) continue;

		FMCPInspectorFunctionSignature Sig;
		Sig.GraphName = Graph->GetName();
		Sig.GraphIndex = GraphIdx;
		Sig.GraphKind = (GT == GT_Function) ? TEXT("Function") : TEXT("Macro");
		Sig.FunctionName = Graph->GetName();

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				FillParamsFromEntry(Entry, Sig);
				Sig.bIsPure = (Entry->GetFunctionFlags() & FUNC_BlueprintPure) != 0;
				Sig.bIsConst = (Entry->GetFunctionFlags() & FUNC_Const) != 0;
			}
			else if (const UK2Node_FunctionResult* Res = Cast<UK2Node_FunctionResult>(Node))
			{
				FillParamsFromResult(Res, Sig);
			}
		}
		Result.Add(Sig);
	}
	return Result;
}

FMCPInspectorAnimBPSettings UMCPInspectorLibrary::GetAnimBlueprintSettings(const FString& AssetPath)
{
	FMCPInspectorAnimBPSettings Out;
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP) return Out;

	if (UAnimBlueprint* ABP = Cast<UAnimBlueprint>(BP))
	{
		Out.bUseMultiThreadedAnimationUpdate = ABP->bUseMultiThreadedAnimationUpdate;
		Out.bWarnAboutBlueprintUsage         = ABP->bWarnAboutBlueprintUsage;
		if (USkeletalMesh* PreviewMesh = ABP->GetPreviewMesh())
		{
			Out.PreviewSkeletalMesh = PreviewMesh->GetPathName();
		}
	}
	return Out;
}


// ─── DumpCDOReferences (CDO + SCS reflection walk) ─────────────────────────

namespace
{
	static void CollectReferencesFromObject(UObject* Obj, TSet<FString>& OutPaths, TSet<UObject*>& Visited);

	static bool ShouldEmitPath(const FString& PathName)
	{
		return !PathName.IsEmpty()
			&& (PathName.StartsWith(TEXT("/Game/")) || PathName.StartsWith(TEXT("/Engine/")));
	}

	static void CollectFromProperty(FProperty* Prop, const void* ValuePtr, TSet<FString>& OutPaths, TSet<UObject*>& Visited)
	{
		if (!Prop || !ValuePtr) return;

		if (FArrayProperty* ArrProp = CastField<FArrayProperty>(Prop))
		{
			FScriptArrayHelper Helper(ArrProp, ValuePtr);
			for (int32 i = 0; i < Helper.Num(); ++i)
			{
				CollectFromProperty(ArrProp->Inner, Helper.GetRawPtr(i), OutPaths, Visited);
			}
			return;
		}
		if (FStructProperty* StrProp = CastField<FStructProperty>(Prop))
		{
			for (TFieldIterator<FProperty> It(StrProp->Struct); It; ++It)
			{
				CollectFromProperty(*It, It->ContainerPtrToValuePtr<void>(ValuePtr), OutPaths, Visited);
			}
			return;
		}
		if (FMapProperty* MapProp = CastField<FMapProperty>(Prop))
		{
			FScriptMapHelper Helper(MapProp, ValuePtr);
			for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
			{
				if (!Helper.IsValidIndex(i)) continue;
				CollectFromProperty(MapProp->KeyProp,   Helper.GetKeyPtr(i),   OutPaths, Visited);
				CollectFromProperty(MapProp->ValueProp, Helper.GetValuePtr(i), OutPaths, Visited);
			}
			return;
		}
		if (FSetProperty* SetProp = CastField<FSetProperty>(Prop))
		{
			FScriptSetHelper Helper(SetProp, ValuePtr);
			for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
			{
				if (!Helper.IsValidIndex(i)) continue;
				CollectFromProperty(SetProp->ElementProp, Helper.GetElementPtr(i), OutPaths, Visited);
			}
			return;
		}

		if (FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Prop))
		{
			const FSoftObjectPtr& Ptr = *static_cast<const FSoftObjectPtr*>(ValuePtr);
			const FString S = Ptr.ToString();
			if (ShouldEmitPath(S))
			{
				OutPaths.Add(S);
			}
			return;
		}

		// FObjectPropertyBase covers FObjectProperty, FClassProperty, FWeakObjectProperty, etc.
		if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			UObject* O = ObjProp->GetObjectPropertyValue(ValuePtr);
			if (O)
			{
				const FString PathName = O->GetPathName();
				if (ShouldEmitPath(PathName))
				{
					OutPaths.Add(PathName);
				}
				// 재귀: instanced subobject (Component template, DefaultSubobject) 만.
				// 일반 asset (UStaticMesh* StaticMesh 같은) 은 path 만 기록하고 재귀 안 함.
				const bool bInstanced = Prop->HasAnyPropertyFlags(CPF_InstancedReference);
				const bool bDefaultSub = O->HasAnyFlags(RF_DefaultSubObject | RF_ArchetypeObject);
				if (bInstanced || bDefaultSub)
				{
					CollectReferencesFromObject(O, OutPaths, Visited);
				}
			}
			return;
		}
	}

	static void CollectReferencesFromObject(UObject* Obj, TSet<FString>& OutPaths, TSet<UObject*>& Visited)
	{
		if (!Obj || Visited.Contains(Obj)) return;
		Visited.Add(Obj);

		for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
		{
			CollectFromProperty(*It, It->ContainerPtrToValuePtr<void>(Obj), OutPaths, Visited);
		}
	}
}

TArray<FString> UMCPInspectorLibrary::DumpCDOReferences(const FString& AssetPath)
{
	TArray<FString> Result;
	UBlueprint* BP = LoadBlueprintFromPath(AssetPath);
	if (!BP)
	{
		return Result;
	}

	TSet<FString> Paths;
	TSet<UObject*> Visited;

	// 1) GeneratedClass CDO walk (모든 native UPROPERTY + 부모 클래스 포함)
	if (BP->GeneratedClass)
	{
		if (UObject* CDO = BP->GeneratedClass->GetDefaultObject(/*bCreateIfNeeded*/ false))
		{
			CollectReferencesFromObject(CDO, Paths, Visited);
		}
	}

	// 2) SCS 의 모든 컴포넌트 템플릿 walk
	if (USimpleConstructionScript* SCS = BP->SimpleConstructionScript)
	{
		const TArray<USCS_Node*>& Nodes = SCS->GetAllNodes();
		for (USCS_Node* Node : Nodes)
		{
			if (Node && Node->ComponentTemplate)
			{
				CollectReferencesFromObject(Node->ComponentTemplate, Paths, Visited);
			}
		}
	}

	// 자기 자신 패키지 / 클래스 경로 제외
	const FString SelfPkg = BP->GetOutermost()->GetName();
	Paths.Remove(SelfPkg);
	Paths.Remove(SelfPkg + TEXT(".") + BP->GetName());
	if (BP->GeneratedClass)
	{
		Paths.Remove(SelfPkg + TEXT(".") + BP->GeneratedClass->GetName());
	}

	Result = Paths.Array();
	Result.Sort();
	return Result;
}

// ─── Duplicate + Remap (Phase 6) ───────────────────────────────────────────

namespace
{
	// path 정규화: "/Game/.../Foo" → "/Game/.../Foo.Foo" (Object path 형태로 통일)
	static FString NormalizeObjectPath(const FString& In)
	{
		if (In.IsEmpty()) return In;
		int32 DotIdx;
		if (!In.FindChar(TEXT('.'), DotIdx))
		{
			int32 SlashIdx;
			if (In.FindLastChar(TEXT('/'), SlashIdx))
			{
				const FString Name = In.Mid(SlashIdx + 1);
				return In + TEXT(".") + Name;
			}
		}
		return In;
	}

	// 치환 맵 키/값 모두 정규화
	static TMap<FString, FString> NormalizeSubstitutionMap(const TMap<FString, FString>& In)
	{
		TMap<FString, FString> Out;
		Out.Reserve(In.Num());
		for (const TPair<FString, FString>& Kv : In)
		{
			Out.Add(NormalizeObjectPath(Kv.Key), NormalizeObjectPath(Kv.Value));
		}
		return Out;
	}

	struct FRemapStats
	{
		int32 ObjectPropertyRewrites = 0;
		int32 SoftObjectPropertyRewrites = 0;
		int32 PinDefaultObjectRewrites = 0;
		int32 PinDefaultValueRewrites = 0;
		int32 LoadFailures = 0;
	};

	// 새 path 의 자산을 로드. 캐시 사용.
	static UObject* LoadRemappedAsset(const FString& NewPath, TMap<FString, UObject*>& Cache)
	{
		if (NewPath.IsEmpty()) return nullptr;
		if (UObject** Found = Cache.Find(NewPath)) return *Found;
		UObject* Loaded = StaticLoadObject(UObject::StaticClass(), nullptr, *NewPath);
		if (!Loaded)
		{
			Loaded = UEditorAssetLibrary::LoadAsset(NewPath);
		}
		Cache.Add(NewPath, Loaded);
		return Loaded;
	}

	static void RewriteReferencesOnObject(
		UObject* Obj,
		const TMap<FString, FString>& Substitutions,
		TMap<FString, UObject*>& LoadCache,
		TSet<UObject*>& Visited,
		FRemapStats& Stats);

	static void RewriteFromProperty(
		FProperty* Prop,
		void* ValuePtr,
		const TMap<FString, FString>& Substitutions,
		TMap<FString, UObject*>& LoadCache,
		TSet<UObject*>& Visited,
		FRemapStats& Stats)
	{
		if (!Prop || !ValuePtr) return;

		if (FArrayProperty* ArrProp = CastField<FArrayProperty>(Prop))
		{
			FScriptArrayHelper Helper(ArrProp, ValuePtr);
			for (int32 i = 0; i < Helper.Num(); ++i)
			{
				RewriteFromProperty(ArrProp->Inner, Helper.GetRawPtr(i), Substitutions, LoadCache, Visited, Stats);
			}
			return;
		}
		if (FStructProperty* StrProp = CastField<FStructProperty>(Prop))
		{
			for (TFieldIterator<FProperty> It(StrProp->Struct); It; ++It)
			{
				RewriteFromProperty(*It, It->ContainerPtrToValuePtr<void>(ValuePtr), Substitutions, LoadCache, Visited, Stats);
			}
			return;
		}
		if (FMapProperty* MapProp = CastField<FMapProperty>(Prop))
		{
			FScriptMapHelper Helper(MapProp, ValuePtr);
			for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
			{
				if (!Helper.IsValidIndex(i)) continue;
				RewriteFromProperty(MapProp->KeyProp,   Helper.GetKeyPtr(i),   Substitutions, LoadCache, Visited, Stats);
				RewriteFromProperty(MapProp->ValueProp, Helper.GetValuePtr(i), Substitutions, LoadCache, Visited, Stats);
			}
			return;
		}
		if (FSetProperty* SetProp = CastField<FSetProperty>(Prop))
		{
			FScriptSetHelper Helper(SetProp, ValuePtr);
			for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
			{
				if (!Helper.IsValidIndex(i)) continue;
				RewriteFromProperty(SetProp->ElementProp, Helper.GetElementPtr(i), Substitutions, LoadCache, Visited, Stats);
			}
			return;
		}

		if (FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Prop))
		{
			FSoftObjectPtr* Ptr = static_cast<FSoftObjectPtr*>(ValuePtr);
			const FString Cur = Ptr->ToString();
			if (const FString* NewPath = Substitutions.Find(NormalizeObjectPath(Cur)))
			{
				*Ptr = FSoftObjectPath(*NewPath);
				++Stats.SoftObjectPropertyRewrites;
			}
			return;
		}

		if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			UObject* O = ObjProp->GetObjectPropertyValue(ValuePtr);
			if (O)
			{
				const FString Cur = O->GetPathName();
				if (const FString* NewPath = Substitutions.Find(NormalizeObjectPath(Cur)))
				{
					if (UObject* NewObj = LoadRemappedAsset(*NewPath, LoadCache))
					{
						// FClassProperty 면 UClass* 가 와야 함. 일반 FObjectProperty 도 OK.
						ObjProp->SetObjectPropertyValue(ValuePtr, NewObj);
						++Stats.ObjectPropertyRewrites;
					}
					else
					{
						UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: Remap 대상 자산 로드 실패: %s"), **NewPath);
						++Stats.LoadFailures;
					}
				}
				else
				{
					// 재귀: 인스턴스 서브오브젝트만
					const bool bInstanced = Prop->HasAnyPropertyFlags(CPF_InstancedReference);
					const bool bDefaultSub = O->HasAnyFlags(RF_DefaultSubObject | RF_ArchetypeObject);
					if (bInstanced || bDefaultSub)
					{
						RewriteReferencesOnObject(O, Substitutions, LoadCache, Visited, Stats);
					}
				}
			}
			return;
		}
	}

	static void RewriteReferencesOnObject(
		UObject* Obj,
		const TMap<FString, FString>& Substitutions,
		TMap<FString, UObject*>& LoadCache,
		TSet<UObject*>& Visited,
		FRemapStats& Stats)
	{
		if (!Obj || Visited.Contains(Obj)) return;
		Visited.Add(Obj);

		for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
		{
			RewriteFromProperty(*It, It->ContainerPtrToValuePtr<void>(Obj), Substitutions, LoadCache, Visited, Stats);
		}
	}

	// 핀의 DefaultValue (직렬화된 path 문자열) 을 치환. 토큰 치환이 아니라 정확 일치.
	static bool RewritePinDefaultValueString(FString& InOut, const TMap<FString, FString>& Substitutions)
	{
		if (InOut.IsEmpty()) return false;
		// 핀 DefaultValue 는 "/Game/.../Asset.Asset" 형태가 보통. 정규화 후 lookup.
		const FString Norm = NormalizeObjectPath(InOut);
		if (const FString* NewPath = Substitutions.Find(Norm))
		{
			InOut = *NewPath;
			return true;
		}
		return false;
	}
}

FString UMCPInspectorLibrary::DuplicateBlueprintWithRemap(
	const FString& SourceAssetPath,
	const FString& DestAssetPath,
	const FString& NewSkeleton,
	const TMap<FString, FString>& AssetSubstitutions)
{
	if (SourceAssetPath.IsEmpty() || DestAssetPath.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: Duplicate 실패 — Source/Dest path 비어있음"));
		return TEXT("");
	}

	UBlueprint* SourceBP = LoadBlueprintFromPath(SourceAssetPath);
	if (!SourceBP)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: Duplicate 실패 — 소스 BP 로드 실패: %s"), *SourceAssetPath);
		return TEXT("");
	}

	// dest path 정규화 (".Asset" 형태로) 후 패키지 경로만 추출
	const FString DestNorm = NormalizeObjectPath(DestAssetPath);
	int32 DotIdx;
	const FString DestPackage = DestNorm.FindChar(TEXT('.'), DotIdx) ? DestNorm.Left(DotIdx) : DestNorm;

	if (UEditorAssetLibrary::DoesAssetExist(DestPackage))
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: Duplicate 실패 — Dest 가 이미 존재: %s"), *DestPackage);
		return TEXT("");
	}

	// Duplicate
	UObject* DupedObj = UEditorAssetLibrary::DuplicateLoadedAsset(SourceBP, DestPackage);
	if (!DupedObj)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: Duplicate 실패 — DuplicateLoadedAsset null: %s → %s"),
			*SourceAssetPath, *DestPackage);
		return TEXT("");
	}
	UBlueprint* NewBP = Cast<UBlueprint>(DupedObj);
	if (!NewBP)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: Duplicate 결과가 UBlueprint 가 아님: %s"), *DupedObj->GetClass()->GetName());
		return TEXT("");
	}

	const TMap<FString, FString> Subs = NormalizeSubstitutionMap(AssetSubstitutions);
	TMap<FString, UObject*> LoadCache;
	TSet<UObject*> Visited;
	FRemapStats Stats;

	// 1) GeneratedClass CDO walk
	if (NewBP->GeneratedClass)
	{
		if (UObject* CDO = NewBP->GeneratedClass->GetDefaultObject(/*bCreateIfNeeded*/ false))
		{
			RewriteReferencesOnObject(CDO, Subs, LoadCache, Visited, Stats);
		}
	}

	// 2) SCS 컴포넌트 템플릿 walk
	if (USimpleConstructionScript* SCS = NewBP->SimpleConstructionScript)
	{
		const TArray<USCS_Node*>& Nodes = SCS->GetAllNodes();
		for (USCS_Node* Node : Nodes)
		{
			if (Node && Node->ComponentTemplate)
			{
				RewriteReferencesOnObject(Node->ComponentTemplate, Subs, LoadCache, Visited, Stats);
			}
		}
	}

	// 3) 모든 그래프 노드 walk + 핀 default value/object 치환
	{
		TArray<UEdGraph*> AllGraphs;
		NewBP->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;

				// 노드 자체의 UPROPERTY (예: UAnimGraphNode_SequencePlayer.Node.Sequence)
				RewriteReferencesOnObject(Node, Subs, LoadCache, Visited, Stats);

				// 핀 별 DefaultObject / DefaultValue
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin) continue;
					if (Pin->DefaultObject)
					{
						const FString Cur = Pin->DefaultObject->GetPathName();
						if (const FString* NewPath = Subs.Find(NormalizeObjectPath(Cur)))
						{
							if (UObject* NewObj = LoadRemappedAsset(*NewPath, LoadCache))
							{
								Pin->DefaultObject = NewObj;
								++Stats.PinDefaultObjectRewrites;
							}
							else
							{
								++Stats.LoadFailures;
							}
						}
					}
					if (RewritePinDefaultValueString(Pin->DefaultValue, Subs))
					{
						++Stats.PinDefaultValueRewrites;
					}
					if (RewritePinDefaultValueString(Pin->AutogeneratedDefaultValue, Subs))
					{
						++Stats.PinDefaultValueRewrites;
					}
				}
			}
		}
	}

	// 4) AnimBP 의 TargetSkeleton 변경
	if (!NewSkeleton.IsEmpty())
	{
		if (UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(NewBP))
		{
			USkeleton* SkeletonObj = Cast<USkeleton>(LoadRemappedAsset(NormalizeObjectPath(NewSkeleton), LoadCache));
			if (SkeletonObj)
			{
				AnimBP->TargetSkeleton = SkeletonObj;
				UE_LOG(LogTemp, Log, TEXT("BlueprintInspector: TargetSkeleton 변경 → %s"), *NewSkeleton);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: NewSkeleton 로드 실패 (스킵): %s"), *NewSkeleton);
			}
		}
	}

	// 5) 컴파일 + 저장
	NewBP->Modify();
	NewBP->MarkPackageDirty();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(NewBP);
	FKismetEditorUtilities::CompileBlueprint(NewBP);

	const FString FinalPath = NewBP->GetPathName();
	UEditorAssetLibrary::SaveAsset(DestPackage, false);

	UE_LOG(LogTemp, Log, TEXT("BlueprintInspector: Duplicate 완료 → %s [obj=%d soft=%d pinObj=%d pinVal=%d loadFail=%d compileStatus=%d]"),
		*FinalPath,
		Stats.ObjectPropertyRewrites,
		Stats.SoftObjectPropertyRewrites,
		Stats.PinDefaultObjectRewrites,
		Stats.PinDefaultValueRewrites,
		Stats.LoadFailures,
		(int32)NewBP->Status);

	return FinalPath;
}

// ════════════════════════════════════════════════════════════════════════════
//  Niagara LightRenderer Inspector (read-only)
// ════════════════════════════════════════════════════════════════════════════


TArray<FString> UMCPInspectorLibrary::InspectNiagaraFunctionCalls(
	const FString& SystemAssetPath,
	const FString& EmitterName)
{
	TArray<FString> Out;
	UNiagaraSystem* System = Cast<UNiagaraSystem>(
		StaticLoadObject(UNiagaraSystem::StaticClass(), nullptr, *SystemAssetPath));
	if (!System)
	{
		return Out;
	}
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		if (Handle.GetName().ToString() != EmitterName)
		{
			continue;
		}
		const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		const UNiagaraScriptSource* ScriptSource = EmitterData
			? Cast<UNiagaraScriptSource>(EmitterData->GraphSource)
			: nullptr;
		if (!ScriptSource || !ScriptSource->NodeGraph)
		{
			return Out;
		}
		for (const UEdGraphNode* GraphNode : ScriptSource->NodeGraph->Nodes)
		{
			const UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(GraphNode);
			if (!FunctionCall)
			{
				continue;
			}
			TArray<FString> InputNames;
			TArray<FNiagaraVariable> Inputs;
			TSet<FNiagaraVariable> HiddenInputs;
			const FCompileConstantResolver Resolver(Handle.GetInstance(), ENiagaraScriptUsage::ParticleUpdateScript);
			FNiagaraStackGraphUtilities::GetStackFunctionInputs(
				*FunctionCall, Inputs, HiddenInputs, Resolver,
				FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly);
			for (const FNiagaraVariable& Input : Inputs)
			{
				InputNames.Add(Input.GetName().ToString() + TEXT(":") + Input.GetType().GetName());
			}
			Out.Add(FString::Printf(TEXT("Name=%s|Script=%s|Inputs=%s"),
				*FunctionCall->GetFunctionName(),
				FunctionCall->FunctionScript ? *FunctionCall->FunctionScript->GetPathName() : TEXT("<null>"),
				*FString::Join(InputNames, TEXT(","))));
		}
		break;
	}
	return Out;
}



static void MR_FindEffectLightAssignments(
	UNiagaraGraph& Graph,
	ENiagaraScriptUsage Usage,
	TArray<UNiagaraNodeAssignment*>& OutAssignments)
{
	for (UEdGraphNode* GraphNode : Graph.Nodes)
	{
		UNiagaraNodeAssignment* Assignment = Cast<UNiagaraNodeAssignment>(GraphNode);
		if (!Assignment || FNiagaraStackGraphUtilities::GetOutputNodeUsage(*Assignment) != Usage)
		{
			continue;
		}
		for (const FNiagaraVariable& Target : Assignment->GetAssignmentTargets())
		{
			if (Target.GetName() == FName(TEXT("Particles.MR_LightColor"))
				&& Target.GetType() == FNiagaraTypeDefinition::GetColorDef())
			{
				OutAssignments.Add(Assignment);
				break;
			}
		}
	}
}

static UEdGraphPin* MR_GetNiagaraOverridePin(
	UNiagaraNodeFunctionCall& FunctionCall,
	const FString& InputName,
	const FNiagaraTypeDefinition& InputType,
	const FCompileConstantResolver& Resolver)
{
	TArray<FNiagaraVariable> Inputs;
	TSet<FNiagaraVariable> HiddenInputs;
	FNiagaraStackGraphUtilities::GetStackFunctionInputs(
		FunctionCall, Inputs, HiddenInputs, Resolver,
		FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly);
	for (const FNiagaraVariable& Input : Inputs)
	{
		if ((InputType.IsValid() && Input.GetType() != InputType) || Input.GetName().ToString() != InputName)
		{
			continue;
		}
		const TOptional<FNiagaraVariableMetaData> Metadata = FunctionCall.GetNiagaraGraph()->GetMetaData(Input);
		const FGuid InputGuid = Metadata.IsSet() ? Metadata->GetVariableGuid() : FGuid();
		const FNiagaraParameterHandle AliasedHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
			FNiagaraParameterHandle(Input.GetName()), &FunctionCall);
		return &FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
			FunctionCall, AliasedHandle, Input.GetType(), InputGuid, FGuid());
	}
	return nullptr;
}

static FString MR_DescribeStackFunctionInputs(
	UNiagaraNodeFunctionCall& FunctionCall,
	const FCompileConstantResolver& Resolver)
{
	TArray<FNiagaraVariable> Inputs;
	TSet<FNiagaraVariable> HiddenInputs;
	FNiagaraStackGraphUtilities::GetStackFunctionInputs(
		FunctionCall, Inputs, HiddenInputs, Resolver,
		FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly);
	TArray<FString> Parts;
	for (const FNiagaraVariable& Input : Inputs)
	{
		Parts.Add(Input.GetName().ToString() + TEXT("~") + Input.GetType().GetName());
	}
	if (Parts.Num() == 0)
	{
		return TEXT("<none>");
	}
	return FString::Join(Parts, TEXT(";"));
}

static FString MR_DescribeStaticSwitchPins(
	UNiagaraNodeFunctionCall& FunctionCall,
	const FCompileConstantResolver& Resolver)
{
	FCompileConstantResolver LocalResolver = Resolver;
	TArray<UEdGraphPin*> Pins;
	TSet<UEdGraphPin*> HiddenPins;
	FNiagaraStackGraphUtilities::GetStackFunctionStaticSwitchPins(FunctionCall, Pins, HiddenPins, LocalResolver);
	TArray<FString> Parts;
	for (UEdGraphPin* Pin : Pins)
	{
		if (Pin != nullptr)
		{
			FString Entry = Pin->PinName.ToString() + TEXT("~") + Pin->PinType.PinCategory.ToString() + TEXT("=") + Pin->DefaultValue;
			if (const UEnum* PinEnum = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get()))
			{
				Entry += TEXT("{");
				const int32 EntryCount = PinEnum->NumEnums() - 1;
				for (int32 Index = 0; Index < EntryCount; ++Index)
				{
					Entry += PinEnum->GetNameStringByIndex(Index) + TEXT("/") + PinEnum->GetDisplayNameTextByIndex(Index).ToString() + TEXT(",");
				}
				Entry += TEXT("}");
			}
			Parts.Add(Entry);
		}
	}
	if (Parts.Num() == 0)
	{
		return TEXT("<none>");
	}
	return FString::Join(Parts, TEXT(";"));
}

static bool MR_SetNiagaraStaticSwitch(
	UNiagaraNodeFunctionCall& FunctionCall,
	const FString& PinName,
	const FString& Value,
	const FCompileConstantResolver& Resolver)
{
	FCompileConstantResolver LocalResolver = Resolver;
	TArray<UEdGraphPin*> Pins;
	TSet<UEdGraphPin*> HiddenPins;
	FNiagaraStackGraphUtilities::GetStackFunctionStaticSwitchPins(FunctionCall, Pins, HiddenPins, LocalResolver);
	for (UEdGraphPin* Pin : Pins)
	{
		if (Pin == nullptr || Pin->PinName.ToString() != PinName)
		{
			continue;
		}
		FString Resolved = Value;
		if (const UEnum* PinEnum = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get()))
		{
			Resolved.Reset();
			const int32 EntryCount = PinEnum->NumEnums() - 1;
			for (int32 Index = 0; Index < EntryCount; ++Index)
			{
				const FString EntryName = PinEnum->GetNameStringByIndex(Index);
				const FString DisplayName = PinEnum->GetDisplayNameTextByIndex(Index).ToString();
				if (EntryName == Value || DisplayName == Value)
				{
					Resolved = EntryName;
					break;
				}
			}
			if (Resolved.IsEmpty())
			{
				return false;
			}
		}
		if (Pin->DefaultValue == Resolved)
		{
			return true;
		}
		Pin->Modify();
		Pin->GetSchema()->TrySetDefaultValue(*Pin, Resolved);
		FunctionCall.MarkNodeRequiresSynchronization(TEXT("MR_SetNiagaraStaticSwitch"), true);
		return Pin->DefaultValue == Resolved;
	}
	return false;
}

static bool MR_SetNiagaraConstant(
	UNiagaraNodeFunctionCall& FunctionCall,
	const FString& InputName,
	const FNiagaraTypeDefinition& InputType,
	const FString& Value,
	const FCompileConstantResolver& Resolver)
{
	UEdGraphPin* Pin = MR_GetNiagaraOverridePin(FunctionCall, InputName, InputType, Resolver);
	if (!Pin || Pin->LinkedTo.Num() != 0)
	{
		return false;
	}
	Pin->Modify();
	Pin->GetSchema()->TrySetDefaultValue(*Pin, Value);
	return Pin->DefaultValue == Value || !Pin->DefaultValue.IsEmpty();
}

static bool MR_SetNiagaraLinkedParameter(
	UNiagaraNodeFunctionCall& FunctionCall,
	const FString& InputName,
	const FNiagaraVariable& Parameter,
	const FCompileConstantResolver& Resolver)
{
	UEdGraphPin* Pin = MR_GetNiagaraOverridePin(FunctionCall, InputName, Parameter.GetType(), Resolver);
	if (!Pin)
	{
		return false;
	}
	if (Pin->LinkedTo.Num() == 0)
	{
		TSet<FNiagaraVariableBase> KnownParameters;
		KnownParameters.Add(Parameter);
		FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(
			*Pin, Parameter, KnownParameters, ENiagaraDefaultMode::FailIfPreviouslyNotSet);
	}
	return Pin->LinkedTo.Num() == 1 && Pin->LinkedTo[0]->PinName == Parameter.GetName();
}

static UNiagaraNodeFunctionCall* MR_SetNiagaraDynamicInput(
	UNiagaraNodeFunctionCall& FunctionCall,
	const FString& InputName,
	const FNiagaraTypeDefinition& InputType,
	const FString& ScriptPath,
	const FCompileConstantResolver& Resolver)
{
	UEdGraphPin* Pin = MR_GetNiagaraOverridePin(FunctionCall, InputName, InputType, Resolver);
	if (!Pin)
	{
		return nullptr;
	}
	UNiagaraNodeFunctionCall* DynamicInput = nullptr;
	if (Pin->LinkedTo.Num() == 0)
	{
		UNiagaraScript* Script = Cast<UNiagaraScript>(
			StaticLoadObject(UNiagaraScript::StaticClass(), nullptr, *ScriptPath));
		if (!Script)
		{
			return nullptr;
		}
		FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(*Pin, Script, DynamicInput);
	}
	else if (Pin->LinkedTo.Num() == 1)
	{
		DynamicInput = Cast<UNiagaraNodeFunctionCall>(Pin->LinkedTo[0]->GetOwningNode());
	}
	if (!DynamicInput || !DynamicInput->FunctionScript || DynamicInput->FunctionScript->GetPathName() != ScriptPath)
	{
		return nullptr;
	}
	return DynamicInput;
}



FString UMCPInspectorLibrary::InspectNiagaraModuleGraph(const FString& ModuleScriptPath)
{
	UNiagaraScript* Script = Cast<UNiagaraScript>(
		StaticLoadObject(UNiagaraScript::StaticClass(), nullptr, *ModuleScriptPath));
	if (!Script)
	{
		return TEXT("ERROR|ScriptLoad");
	}
	UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
	if (!Source || !Source->NodeGraph)
	{
		return TEXT("ERROR|Source");
	}

	TArray<FString> Parts;
	Parts.Add(FString::Printf(TEXT("Usage=%d"), (int32)Script->GetUsage()));
	Parts.Add(FString::Printf(TEXT("Nodes=%d"), Source->NodeGraph->Nodes.Num()));

	for (UEdGraphNode* Node : Source->NodeGraph->Nodes)
	{
		if (Node == nullptr)
		{
			continue;
		}
		FString Entry = FString::Printf(TEXT("[%s"), *Node->GetClass()->GetName());
		if (const UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(Node))
		{
			Entry += FString::Printf(TEXT(" usage=%d outputs=%d"), (int32)OutputNode->GetUsage(), OutputNode->Outputs.Num());
			for (const FNiagaraVariable& Var : OutputNode->Outputs)
			{
				Entry += TEXT(" out:") + Var.GetName().ToString() + TEXT("~") + Var.GetType().GetName();
			}
		}
		for (TFieldIterator<FProperty> PropIt(Node->GetClass()); PropIt; ++PropIt)
		{
			FProperty* Prop = *PropIt;
			if (Prop == nullptr || Prop->GetOwnerClass() == UEdGraphNode::StaticClass())
			{
				continue;
			}
			if (const FNameProperty* NameProp = CastField<FNameProperty>(Prop))
			{
				const FName Value = NameProp->GetPropertyValue_InContainer(Node);
				Entry += TEXT(" prop:") + Prop->GetName() + TEXT("=") + Value.ToString();
			}
			else if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop))
			{
				const FString Value = StrProp->GetPropertyValue_InContainer(Node);
				if (!Value.IsEmpty() && Value.Len() < 96)
				{
					Entry += TEXT(" prop:") + Prop->GetName() + TEXT("=") + Value;
				}
			}
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin == nullptr)
			{
				continue;
			}
			Entry += FString::Printf(TEXT(" %s:%s~%s%s"),
				Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"),
				*Pin->PinName.ToString(),
				*Pin->PinType.PinCategory.ToString(),
				Pin->LinkedTo.Num() > 0 ? TEXT("*") : TEXT(""));
		}
		Entry += TEXT("]");
		Parts.Add(Entry);
	}
	return TEXT("OK|") + FString::Join(Parts, TEXT("|"));
}

// UNiagaraNodeWithDynamicPins 의 핀 추가 API 는 export 되지 않아 외부 모듈에서 링크가 안 된다.
// 대신 export 된 스키마 변환 + 엔진 공용 CreatePin 으로 동일한 핀을 만든다.
static UEdGraphPin* MR_CreateNiagaraParameterPin(
	UEdGraphNode& Node,
	const FNiagaraVariable& Parameter,
	EEdGraphPinDirection Direction)
{
	const FEdGraphPinType PinType = UEdGraphSchema_Niagara::TypeDefinitionToPinType(Parameter.GetType());
	// 이름만 보고 재사용하면 타입이 바뀐 뒤에도 구 핀이 살아남아 부착 시 타입 불일치가 난다. 타입이 다르면 지우고 다시 만든다.
	for (int32 Index = Node.Pins.Num() - 1; Index >= 0; --Index)
	{
		UEdGraphPin* Existing = Node.Pins[Index];
		if (Existing == nullptr || Existing->Direction != Direction || Existing->PinName != Parameter.GetName())
		{
			continue;
		}
		if (Existing->PinType == PinType)
		{
			return Existing;
		}
		Existing->BreakAllPinLinks();
		Node.RemovePin(Existing);
	}
	return Node.CreatePin(Direction, PinType, Parameter.GetName());
}


// NiagaraNodeOp / NiagaraNodeConvert 는 클래스에 export 가 없어 StaticClass() 링크가 안 된다.
// 클래스를 경로 이름으로 찾아 NewObject 하고, OpName 은 리플렉션으로 넣는다.
static UEdGraphNode* MR_CreateNiagaraOpNode(UNiagaraGraph& Graph, const TCHAR* OpName)
{
	UClass* OpClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeOp"));
	if (OpClass == nullptr)
	{
		return nullptr;
	}
	UEdGraphNode* Node = NewObject<UEdGraphNode>(&Graph, OpClass, NAME_None, RF_Transactional);
	if (Node == nullptr)
	{
		return nullptr;
	}
	FNameProperty* Prop = CastField<FNameProperty>(OpClass->FindPropertyByName(TEXT("OpName")));
	if (Prop == nullptr)
	{
		return nullptr;
	}
	Prop->SetPropertyValue_InContainer(Node, FName(OpName));
	Graph.AddNode(Node, false, false);
	Node->CreateNewGuid();
	Node->PostPlacedNewNode();
	Node->AllocateDefaultPins();
	return Node;
}

// swizzle Convert 노드. InitAsSwizzle 은 AutowireSwizzle 만 세팅하고 실제 핀은 AutowireNewNode 가 만든다.
// 클래스에 export 가 없어 이름 조회 + 리플렉션으로 문자열을 넣는다.
static UEdGraphNode* MR_CreateNiagaraSwizzleNode(UNiagaraGraph& Graph, const TCHAR* Swizzle, UEdGraphPin& FromPin)
{
	UClass* NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeConvert"));
	if (NodeClass == nullptr)
	{
		return nullptr;
	}
	UEdGraphNode* Node = NewObject<UEdGraphNode>(&Graph, NodeClass, NAME_None, RF_Transactional);
	if (Node == nullptr)
	{
		return nullptr;
	}
	FStrProperty* Prop = CastField<FStrProperty>(NodeClass->FindPropertyByName(TEXT("AutowireSwizzle")));
	if (Prop == nullptr)
	{
		return nullptr;
	}
	Prop->SetPropertyValue_InContainer(Node, FString(Swizzle));
	Graph.AddNode(Node, false, false);
	Node->CreateNewGuid();
	Node->PostPlacedNewNode();
	Node->AllocateDefaultPins();
	Node->AutowireNewNode(&FromPin);
	return Node;
}

// 모듈 그래프 안에서 스톡 다이나믹 인풋 스크립트를 함수콜 노드로 쓴다. 클래스 export 가 없어 이름 조회 + 리플렉션으로 만든다.
static UNiagaraNodeFunctionCall* MR_CreateNiagaraFunctionCallNode(UNiagaraGraph& Graph, const TCHAR* ScriptPath)
{
	UNiagaraScript* Script = Cast<UNiagaraScript>(
		StaticLoadObject(UNiagaraScript::StaticClass(), nullptr, ScriptPath));
	if (Script == nullptr)
	{
		return nullptr;
	}
	UClass* NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeFunctionCall"));
	if (NodeClass == nullptr)
	{
		return nullptr;
	}
	UEdGraphNode* Node = NewObject<UEdGraphNode>(&Graph, NodeClass, NAME_None, RF_Transactional);
	UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Node);
	if (Call == nullptr)
	{
		return nullptr;
	}
	FObjectPropertyBase* Prop = CastField<FObjectPropertyBase>(NodeClass->FindPropertyByName(TEXT("FunctionScript")));
	if (Prop == nullptr)
	{
		return nullptr;
	}
	Prop->SetObjectPropertyValue_InContainer(Call, Script);
	Graph.AddNode(Call, false, false);
	Call->CreateNewGuid();
	Call->PostPlacedNewNode();
	Call->AllocateDefaultPins();
	return Call;
}

// 모듈 그래프에는 스택 컨텍스트가 없어 GetStackFunctionStaticSwitchPins 를 못 쓴다. 노드 핀에서 직접 찾아 enum 으로 해석한다.
static bool MR_SetGraphStaticSwitch(UEdGraphNode& Node, const TCHAR* PinName, const TCHAR* Value)
{
	for (UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin == nullptr || Pin->Direction != EGPD_Input || Pin->PinName != FName(PinName))
		{
			continue;
		}
		FString Resolved = Value;
		if (const UEnum* PinEnum = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get()))
		{
			Resolved.Reset();
			const int32 EntryCount = PinEnum->NumEnums() - 1;
			for (int32 Index = 0; Index < EntryCount; ++Index)
			{
				const FString EntryName = PinEnum->GetNameStringByIndex(Index);
				if (EntryName == Value || PinEnum->GetDisplayNameTextByIndex(Index).ToString() == Value)
				{
					Resolved = EntryName;
					break;
				}
			}
			if (Resolved.IsEmpty())
			{
				return false;
			}
		}
		Pin->Modify();
		Pin->GetSchema()->TrySetDefaultValue(*Pin, Resolved);
		return Pin->DefaultValue == Resolved;
	}
	return false;
}

// 함수콜 노드의 출력은 스크립트마다 이름이 달라 이름으로 못 찾는다. 파라미터맵이 아닌 첫 출력 데이터 핀을 쓴다.
static UEdGraphPin* MR_FindFirstOutputDataPin(UEdGraphNode& Node)
{
	for (UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin == nullptr || Pin->Direction != EGPD_Output)
		{
			continue;
		}
		const FString Category = Pin->PinType.PinCategory.ToString();
		if (Category == TEXT("Misc") || Pin->PinName == FName(TEXT("Add")))
		{
			continue;
		}
		const FNiagaraTypeDefinition Type = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
		if (Type == FNiagaraTypeDefinition::GetParameterMapDef())
		{
			continue;
		}
		return Pin;
	}
	return nullptr;
}

static UEdGraphPin* MR_FindPin(UEdGraphNode& Node, const TCHAR* PinName, EEdGraphPinDirection Direction)
{
	for (UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin != nullptr && Pin->Direction == Direction && Pin->PinName == FName(PinName))
		{
			return Pin;
		}
	}
	return nullptr;
}

static FString MR_DescribeNodePins(UEdGraphNode& Node)
{
	TArray<FString> Parts;
	for (UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin != nullptr)
		{
			Parts.Add((Pin->Direction == EGPD_Input ? TEXT("in:") : TEXT("out:")) + Pin->PinName.ToString());
		}
	}
	return FString::Join(Parts, TEXT(","));
}


// ════════════════════════════════════════════════════════════════════════════
//  Material Inspector (read-only) — UE5.7 expression 그래프 덤프
//    설계: D:\Mir5_Work\PostProcessVolume\MaterialInspector_Extension_DESIGN.md
//    가드: GetEditorOnlyData/GetExpressions = WITH_EDITORONLY_DATA,
//          GetInput/GetCaption/GetExpressionInputDescription = WITH_EDITOR
// ════════════════════════════════════════════════════════════════════════════

namespace
{
	static UMaterialInterface* BPI_LoadMaterialInterface(const FString& AssetPath)
	{
		if (AssetPath.IsEmpty()) return nullptr;
		return Cast<UMaterialInterface>(
			StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, *AssetPath));
	}

	// expression 식별자 = 전체 오브젝트 경로(RC로 머티리얼 편집 시 직접 참조 가능). 입력 cross-ref도 동일 경로라 일관.
	static FString BPI_MaterialNodeId(const UMaterialExpression* Expr)
	{
		if (!Expr) return FString();
		return Expr->GetPathName();
	}

	static void BPI_GetShadingModelNames(const FMaterialShadingModelField& Field, TArray<FString>& Out)
	{
		const UEnum* SMEnum = StaticEnum<EMaterialShadingModel>();
		for (int32 i = 0; i < MSM_NUM; ++i)
		{
			const EMaterialShadingModel SM = static_cast<EMaterialShadingModel>(i);
			if (Field.HasShadingModel(SM))
			{
				Out.Add(SMEnum ? SMEnum->GetNameStringByValue((int64)i) : FString::FromInt(i));
			}
		}
	}

	static FString BPI_CollectionPath(const UMaterialExpressionCollectionParameter* CP)
	{
		return (CP && CP->Collection) ? CP->Collection->GetPathName() : FString();
	}
}



TArray<FMCPInspectorMaterialPropertyLink> UMCPInspectorLibrary::DumpMaterialPropertyConnections(const FString& AssetPath)
{
	TArray<FMCPInspectorMaterialPropertyLink> Out;
	UMaterialInterface* MatIface = BPI_LoadMaterialInterface(AssetPath);
	if (!MatIface) { return Out; }
	UMaterial* Mat = MatIface->GetMaterial();
	if (!Mat) { return Out; }

#if WITH_EDITOR
	static const EMaterialProperty Props[] = {
		MP_BaseColor, MP_Metallic, MP_Specular, MP_Roughness, MP_Anisotropy,
		MP_EmissiveColor, MP_Opacity, MP_OpacityMask, MP_Normal, MP_Tangent,
		MP_WorldPositionOffset, MP_SubsurfaceColor, MP_AmbientOcclusion,
		MP_Refraction, MP_PixelDepthOffset
	};
	const UEnum* PropEnum = StaticEnum<EMaterialProperty>();
	for (EMaterialProperty Prop : Props)
	{
		FMaterialInputDescription Desc;
		if (!Mat->GetExpressionInputDescription(Prop, Desc)) { continue; }
		if (Desc.bHidden) { continue; }

		FMCPInspectorMaterialPropertyLink L;
		L.Property      = PropEnum ? PropEnum->GetNameStringByValue((int64)Prop) : FString::FromInt((int32)Prop);
		L.bUsesConstant = Desc.bUseConstant;
		if (Desc.Input && Desc.Input->Expression)
		{
			L.ConnectedNode = BPI_MaterialNodeId(Desc.Input->Expression);
			L.OutputIndex   = Desc.Input->OutputIndex;
		}
		Out.Add(L);
	}
#endif // WITH_EDITOR
	return Out;
}

TArray<FString> UMCPInspectorLibrary::FindMaterialsUsingCollection(const FString& CollectionPath)
{
	TArray<FString> Out;
	if (CollectionPath.IsEmpty()) { return Out; }

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AR = ARM.Get();
	if (AR.IsLoadingAssets())
	{
		AR.WaitForCompletion();
	}

	// MPC 경로 → 패키지 이름
	FString CollPkg = CollectionPath;
	int32 DotIdx;
	if (CollPkg.FindChar(TEXT('.'), DotIdx)) { CollPkg = CollPkg.Left(DotIdx); }

	// MPC 를 hard-reference 하는 패키지 조회.
	TArray<FName> Referencers;
	AR.GetReferencers(
		FName(*CollPkg), Referencers,
		UE::AssetRegistry::EDependencyCategory::Package,
		UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Hard));

	const FTopLevelAssetPath MaterialClassPath = UMaterial::StaticClass()->GetClassPathName();

	// ★ 머티리얼을 로드하지 않고 AssetRegistry 데이터만으로 식별한다.
	//    (전체 GetAsset() 로드는 가상화(Virtualized) 에셋 페이로드 풀링을 유발해
	//     에디터를 모달로 멈출 수 있음 — UMaterial 이 MPC 를 hard-ref 하면 사실상 소비로 간주)
	for (const FName& RefPkg : Referencers)
	{
		TArray<FAssetData> Assets;
		AR.GetAssetsByPackageName(RefPkg, Assets);
		for (const FAssetData& AD : Assets)
		{
			if (AD.AssetClassPath == MaterialClassPath) // UMaterial 만 (MI/Function 제외), 로드 없이
			{
				Out.AddUnique(AD.GetObjectPathString());
			}
		}
	}
	return Out;
}

// UDS 9.7 migration contracts -------------------------------------------------

static FString BPI_UDS97Serialize(const TSharedRef<FJsonObject>& Object)
{
	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Object, Writer);
	return Output;
}

static FString BPI_UDS97Result(
	const FString& Operation,
	const bool bOk,
	const FString& Status,
	const FString& Error,
	const TSharedPtr<FJsonObject>& Details)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schemaVersion"), 1);
	Root->SetStringField(TEXT("operation"), Operation);
	Root->SetBoolField(TEXT("ok"), bOk);
	Root->SetStringField(TEXT("status"), Status);
	Root->SetStringField(TEXT("error"), Error);
	Root->SetObjectField(TEXT("details"), Details.IsValid() ? Details.ToSharedRef() : MakeShared<FJsonObject>());
	return BPI_UDS97Serialize(Root);
}

static void BPI_UDS97CollectExpressions(
	const FString& AssetPath,
	TArray<UMaterialExpression*>& OutExpressions,
	UMaterial*& OutMaterial)
{
	OutExpressions.Reset();
	OutMaterial = nullptr;
	if (UMaterialInterface* MaterialInterface = BPI_LoadMaterialInterface(AssetPath))
	{
		OutMaterial = MaterialInterface->GetMaterial();
		if (OutMaterial != nullptr)
		{
			for (const TObjectPtr<UMaterialExpression>& Expression : OutMaterial->GetExpressions())
			{
				OutExpressions.Add(Expression.Get());
			}
		}
		return;
	}

	UMaterialFunctionInterface* Function = Cast<UMaterialFunctionInterface>(
		StaticLoadObject(UMaterialFunctionInterface::StaticClass(), nullptr, *AssetPath));
	if (Function != nullptr)
	{
		for (const TObjectPtr<UMaterialExpression>& Expression : Function->GetExpressions())
		{
			OutExpressions.Add(Expression.Get());
		}
	}
}

static TSharedRef<FJsonObject> BPI_UDS97ExpressionLink(const FExpressionInput& Input)
{
	const TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
	Link->SetStringField(TEXT("expression"), Input.Expression != nullptr ? Input.Expression->GetPathName() : TEXT(""));
	Link->SetNumberField(TEXT("outputIndex"), Input.OutputIndex);
	Link->SetNumberField(TEXT("mask"), Input.Mask);
	Link->SetNumberField(TEXT("maskR"), Input.MaskR);
	Link->SetNumberField(TEXT("maskG"), Input.MaskG);
	Link->SetNumberField(TEXT("maskB"), Input.MaskB);
	Link->SetNumberField(TEXT("maskA"), Input.MaskA);
	return Link;
}

FString UMCPInspectorLibrary::DumpMaterialFunctionContractJson(const FString& AssetPath)
{
	UMaterialFunctionInterface* Function = Cast<UMaterialFunctionInterface>(
		StaticLoadObject(UMaterialFunctionInterface::StaticClass(), nullptr, *AssetPath));
	if (Function == nullptr)
	{
		return BPI_UDS97Result(TEXT("DumpMaterialFunctionContractJson"), false, TEXT("failed"), TEXT("Material function not found"), nullptr);
	}

	TArray<TSharedPtr<FJsonValue>> Inputs;
	TArray<TSharedPtr<FJsonValue>> Outputs;
	for (const TObjectPtr<UMaterialExpression>& ExpressionPtr : Function->GetExpressions())
	{
		UMaterialExpression* Expression = ExpressionPtr.Get();
		if (UMaterialExpressionFunctionInput* Input = Cast<UMaterialExpressionFunctionInput>(Expression))
		{
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Input->InputName.ToString());
			Row->SetStringField(TEXT("id"), Input->Id.ToString(EGuidFormats::Digits));
			Row->SetStringField(TEXT("type"), UMaterialExpressionFunctionInput::GetInputTypeDisplayName(Input->InputType));
			Row->SetNumberField(TEXT("sortPriority"), Input->SortPriority);
			Row->SetStringField(TEXT("nodeId"), Input->GetPathName());
			Inputs.Add(MakeShared<FJsonValueObject>(Row));
		}
		else if (UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
		{
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Output->OutputName.ToString());
			Row->SetStringField(TEXT("id"), Output->Id.ToString(EGuidFormats::Digits));
			Row->SetNumberField(TEXT("sortPriority"), Output->SortPriority);
			Row->SetStringField(TEXT("nodeId"), Output->GetPathName());
			Row->SetObjectField(TEXT("source"), BPI_UDS97ExpressionLink(Output->A));
			Outputs.Add(MakeShared<FJsonValueObject>(Row));
		}
	}

	const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("assetPath"), Function->GetPathName());
	Details->SetArrayField(TEXT("inputs"), Inputs);
	Details->SetArrayField(TEXT("outputs"), Outputs);
	return BPI_UDS97Result(TEXT("DumpMaterialFunctionContractJson"), true, TEXT("success"), TEXT(""), Details);
}

FString UMCPInspectorLibrary::DumpMaterialFunctionCallerContractJson(
	const FString& CallerPath,
	const TArray<FString>& FunctionPaths)
{
	TArray<UMaterialExpression*> Expressions;
	UMaterial* Material = nullptr;
	BPI_UDS97CollectExpressions(CallerPath, Expressions, Material);
	if (Expressions.Num() == 0 && Material == nullptr)
	{
		return BPI_UDS97Result(TEXT("DumpMaterialFunctionCallerContractJson"), false, TEXT("failed"), TEXT("Caller not found or has no expressions"), nullptr);
	}

	TSet<FString> RequestedFunctions;
	for (const FString& FunctionPath : FunctionPaths)
	{
		RequestedFunctions.Add(FunctionPath);
	}

	TArray<TSharedPtr<FJsonValue>> Calls;
	for (UMaterialExpression* Expression : Expressions)
	{
		UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
		if (Call == nullptr || Call->MaterialFunction == nullptr)
		{
			continue;
		}
		const FString FunctionPath = Call->MaterialFunction->GetOutermost()->GetName();
		if (!RequestedFunctions.Contains(FunctionPath))
		{
			continue;
		}

		const TSharedRef<FJsonObject> CallRow = MakeShared<FJsonObject>();
		CallRow->SetStringField(TEXT("nodeId"), Call->GetPathName());
		CallRow->SetStringField(TEXT("function"), FunctionPath);
		TArray<TSharedPtr<FJsonValue>> Inputs;
		for (int32 InputIndex = 0; InputIndex < Call->FunctionInputs.Num(); ++InputIndex)
		{
			const FFunctionExpressionInput& FunctionInput = Call->FunctionInputs[InputIndex];
			const TSharedRef<FJsonObject> InputRow = MakeShared<FJsonObject>();
			InputRow->SetNumberField(TEXT("index"), InputIndex);
			InputRow->SetStringField(
				TEXT("name"),
				FunctionInput.ExpressionInput != nullptr ? FunctionInput.ExpressionInput->InputName.ToString() : Call->GetInputName(InputIndex).ToString());
			InputRow->SetStringField(TEXT("expressionInputId"), FunctionInput.ExpressionInputId.ToString(EGuidFormats::Digits));
			InputRow->SetObjectField(TEXT("source"), BPI_UDS97ExpressionLink(FunctionInput.Input));
			Inputs.Add(MakeShared<FJsonValueObject>(InputRow));
		}
		CallRow->SetArrayField(TEXT("inputs"), Inputs);

		TArray<TSharedPtr<FJsonValue>> Outputs;
		for (int32 OutputIndex = 0; OutputIndex < Call->FunctionOutputs.Num(); ++OutputIndex)
		{
			const FFunctionExpressionOutput& FunctionOutput = Call->FunctionOutputs[OutputIndex];
			const TSharedRef<FJsonObject> OutputRow = MakeShared<FJsonObject>();
			OutputRow->SetNumberField(TEXT("index"), OutputIndex);
			OutputRow->SetStringField(
				TEXT("name"),
				FunctionOutput.ExpressionOutput != nullptr ? FunctionOutput.ExpressionOutput->OutputName.ToString() : FString::FromInt(OutputIndex));
			OutputRow->SetStringField(TEXT("expressionOutputId"), FunctionOutput.ExpressionOutputId.ToString(EGuidFormats::Digits));
			Outputs.Add(MakeShared<FJsonValueObject>(OutputRow));
		}
		CallRow->SetArrayField(TEXT("outputs"), Outputs);

		TArray<TSharedPtr<FJsonValue>> Downstream;
		for (UMaterialExpression* Consumer : Expressions)
		{
			if (Consumer == nullptr)
			{
				continue;
			}
			for (int32 ConsumerInputIndex = 0; ConsumerInputIndex < Consumer->CountInputs(); ++ConsumerInputIndex)
			{
				FExpressionInput* ConsumerInput = Consumer->GetInput(ConsumerInputIndex);
				if (ConsumerInput != nullptr && ConsumerInput->Expression == Call)
				{
					const TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
					Link->SetStringField(TEXT("consumerNode"), Consumer->GetPathName());
					Link->SetStringField(TEXT("consumerInput"), Consumer->GetInputName(ConsumerInputIndex).ToString());
					Link->SetNumberField(TEXT("callOutputIndex"), ConsumerInput->OutputIndex);
					Downstream.Add(MakeShared<FJsonValueObject>(Link));
				}
			}
		}
		if (Material != nullptr)
		{
			FExpressionInput* RootInput = Material->GetExpressionInputForProperty(MP_MaterialAttributes);
			if (RootInput != nullptr && RootInput->Expression == Call)
			{
				const TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
				Link->SetStringField(TEXT("consumerNode"), TEXT("MaterialRoot"));
				Link->SetStringField(TEXT("consumerInput"), TEXT("MaterialAttributes"));
				Link->SetNumberField(TEXT("callOutputIndex"), RootInput->OutputIndex);
				Downstream.Add(MakeShared<FJsonValueObject>(Link));
			}
		}
		CallRow->SetArrayField(TEXT("downstream"), Downstream);
		Calls.Add(MakeShared<FJsonValueObject>(CallRow));
	}

	const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("callerPath"), CallerPath);
	Details->SetNumberField(TEXT("callCount"), Calls.Num());
	Details->SetArrayField(TEXT("calls"), Calls);
	return BPI_UDS97Result(TEXT("DumpMaterialFunctionCallerContractJson"), true, TEXT("success"), TEXT(""), Details);
}

FString UMCPInspectorLibrary::DumpMPCContractJson(const FString& CollectionPath)
{
	UMaterialParameterCollection* Collection = Cast<UMaterialParameterCollection>(
		StaticLoadObject(UMaterialParameterCollection::StaticClass(), nullptr, *CollectionPath));
	if (Collection == nullptr)
	{
		return BPI_UDS97Result(TEXT("DumpMPCContractJson"), false, TEXT("failed"), TEXT("MPC not found"), nullptr);
	}

	TArray<TSharedPtr<FJsonValue>> Scalars;
	for (int32 Index = 0; Index < Collection->ScalarParameters.Num(); ++Index)
	{
		const FCollectionScalarParameter& Parameter = Collection->ScalarParameters[Index];
		const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("index"), Index);
		Row->SetStringField(TEXT("name"), Parameter.ParameterName.ToString());
		Row->SetStringField(TEXT("id"), Parameter.Id.ToString(EGuidFormats::Digits));
		Row->SetNumberField(TEXT("default"), Parameter.DefaultValue);
		Scalars.Add(MakeShared<FJsonValueObject>(Row));
	}

	TArray<TSharedPtr<FJsonValue>> Vectors;
	for (int32 Index = 0; Index < Collection->VectorParameters.Num(); ++Index)
	{
		const FCollectionVectorParameter& Parameter = Collection->VectorParameters[Index];
		const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("index"), Index);
		Row->SetStringField(TEXT("name"), Parameter.ParameterName.ToString());
		Row->SetStringField(TEXT("id"), Parameter.Id.ToString(EGuidFormats::Digits));
		TArray<TSharedPtr<FJsonValue>> Default;
		Default.Add(MakeShared<FJsonValueNumber>(Parameter.DefaultValue.R));
		Default.Add(MakeShared<FJsonValueNumber>(Parameter.DefaultValue.G));
		Default.Add(MakeShared<FJsonValueNumber>(Parameter.DefaultValue.B));
		Default.Add(MakeShared<FJsonValueNumber>(Parameter.DefaultValue.A));
		Row->SetArrayField(TEXT("default"), Default);
		Vectors.Add(MakeShared<FJsonValueObject>(Row));
	}

	const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("assetPath"), Collection->GetPathName());
	Details->SetArrayField(TEXT("scalars"), Scalars);
	Details->SetArrayField(TEXT("vectors"), Vectors);
	return BPI_UDS97Result(TEXT("DumpMPCContractJson"), true, TEXT("success"), TEXT(""), Details);
}

FString UMCPInspectorLibrary::DumpMaterialMPCBindingsJson(
	const FString& AssetPath,
	const FString& CollectionPath)
{
	TArray<UMaterialExpression*> Expressions;
	UMaterial* Material = nullptr;
	BPI_UDS97CollectExpressions(AssetPath, Expressions, Material);
	UMaterialParameterCollection* Collection = Cast<UMaterialParameterCollection>(
		StaticLoadObject(UMaterialParameterCollection::StaticClass(), nullptr, *CollectionPath));
	if ((Expressions.Num() == 0 && Material == nullptr) || Collection == nullptr)
	{
		return BPI_UDS97Result(
			TEXT("DumpMaterialMPCBindingsJson"),
			false,
			TEXT("failed"),
			TEXT("Material/function or MPC not found"),
			nullptr);
	}

	TArray<TSharedPtr<FJsonValue>> Bindings;
	for (UMaterialExpression* Expression : Expressions)
	{
		UMaterialExpressionCollectionParameter* Parameter = Cast<UMaterialExpressionCollectionParameter>(Expression);
		if (Parameter == nullptr || Parameter->Collection != Collection)
		{
			continue;
		}
		const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("nodeId"), Parameter->GetPathName());
		Row->SetStringField(TEXT("collection"), Parameter->Collection->GetPathName());
		Row->SetStringField(TEXT("parameterName"), Parameter->ParameterName.ToString());
		Row->SetStringField(TEXT("parameterId"), Parameter->ParameterId.ToString(EGuidFormats::Digits));
		Row->SetStringField(TEXT("expressionGuid"), Parameter->ExpressionGUID.ToString(EGuidFormats::Digits));
		Row->SetStringField(TEXT("group"), Parameter->Group.ToString());
		Row->SetNumberField(TEXT("sortPriority"), Parameter->SortPriority);
		Bindings.Add(MakeShared<FJsonValueObject>(Row));
	}

	const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("assetPath"), AssetPath);
	Details->SetStringField(TEXT("collectionPath"), Collection->GetPathName());
	Details->SetNumberField(TEXT("bindingCount"), Bindings.Num());
	Details->SetArrayField(TEXT("bindings"), Bindings);
	return BPI_UDS97Result(TEXT("DumpMaterialMPCBindingsJson"), true, TEXT("success"), TEXT(""), Details);
}

FString UMCPInspectorLibrary::DumpBlueprintMPCBindingsJson(
	const FString& BlueprintPath,
	const FString& CollectionPath)
{
	UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
	UMaterialParameterCollection* Collection = Cast<UMaterialParameterCollection>(
		StaticLoadObject(UMaterialParameterCollection::StaticClass(), nullptr, *CollectionPath));
	if (Blueprint == nullptr || Collection == nullptr)
	{
		return BPI_UDS97Result(TEXT("DumpBlueprintMPCBindingsJson"), false, TEXT("failed"), TEXT("Blueprint or MPC not found"), nullptr);
	}

	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	TArray<TSharedPtr<FJsonValue>> Bindings;
	for (int32 GraphIndex = 0; GraphIndex < Graphs.Num(); ++GraphIndex)
	{
		UEdGraph* Graph = Graphs[GraphIndex];
		if (Graph == nullptr)
		{
			continue;
		}
		for (UEdGraphNode* GraphNode : Graph->Nodes)
		{
			UK2Node_CallMaterialParameterCollectionFunction* Node = Cast<UK2Node_CallMaterialParameterCollectionFunction>(GraphNode);
			if (Node == nullptr)
			{
				continue;
			}
			UEdGraphPin* CollectionPin = Node->FindPin(TEXT("Collection"));
			if (CollectionPin == nullptr || CollectionPin->DefaultObject != Collection)
			{
				continue;
			}
			UEdGraphPin* ParameterPin = Node->FindPin(TEXT("ParameterName"));
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("graphIndex"), GraphIndex);
			Row->SetStringField(TEXT("graph"), Graph->GetName());
			Row->SetStringField(TEXT("nodeName"), Node->GetName());
			Row->SetStringField(TEXT("nodeGuid"), Node->NodeGuid.ToString(EGuidFormats::Digits));
			Row->SetStringField(TEXT("operation"), Node->FunctionReference.GetMemberName().ToString());
			Row->SetStringField(TEXT("collection"), CollectionPin->DefaultObject->GetPathName());
			Row->SetBoolField(TEXT("parameterNameConnected"), ParameterPin != nullptr && ParameterPin->LinkedTo.Num() > 0);
			Row->SetStringField(TEXT("parameterName"), ParameterPin != nullptr ? ParameterPin->DefaultValue : TEXT(""));
			Bindings.Add(MakeShared<FJsonValueObject>(Row));
		}
	}

	const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
	Details->SetStringField(TEXT("generatedClass"), Blueprint->GeneratedClass != nullptr ? Blueprint->GeneratedClass->GetPathName() : TEXT(""));
	Details->SetNumberField(TEXT("bindingCount"), Bindings.Num());
	Details->SetArrayField(TEXT("bindings"), Bindings);
	return BPI_UDS97Result(TEXT("DumpBlueprintMPCBindingsJson"), true, TEXT("success"), TEXT(""), Details);
}

FString UMCPInspectorLibrary::MergeMaterialParameterCollectionAdditiveChecked(
	const FString& TargetPath,
	const FString& SourcePath,
	const FString& ExpectedTargetContractJson)
{
	const FString CurrentContract = DumpMPCContractJson(TargetPath);
	if (CurrentContract != ExpectedTargetContractJson)
	{
		return BPI_UDS97Result(TEXT("MergeMaterialParameterCollectionAdditiveChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Target MPC contract mismatch"), nullptr);
	}

	UMaterialParameterCollection* Target = Cast<UMaterialParameterCollection>(
		StaticLoadObject(UMaterialParameterCollection::StaticClass(), nullptr, *TargetPath));
	UMaterialParameterCollection* Source = Cast<UMaterialParameterCollection>(
		StaticLoadObject(UMaterialParameterCollection::StaticClass(), nullptr, *SourcePath));
	if (Target == nullptr || Source == nullptr || Target == Source)
	{
		return BPI_UDS97Result(TEXT("MergeMaterialParameterCollectionAdditiveChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Invalid target/source MPC"), nullptr);
	}

	TSet<FGuid> TargetScalarIds;
	TSet<FGuid> TargetVectorIds;
	TSet<FName> TargetNames;
	for (const FCollectionScalarParameter& Parameter : Target->ScalarParameters)
	{
		TargetScalarIds.Add(Parameter.Id);
		TargetNames.Add(Parameter.ParameterName);
	}
	for (const FCollectionVectorParameter& Parameter : Target->VectorParameters)
	{
		TargetVectorIds.Add(Parameter.Id);
		TargetNames.Add(Parameter.ParameterName);
	}

	for (const FCollectionScalarParameter& Parameter : Source->ScalarParameters)
	{
		if (TargetVectorIds.Contains(Parameter.Id))
		{
			return BPI_UDS97Result(TEXT("MergeMaterialParameterCollectionAdditiveChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Scalar/vector GUID conflict"), nullptr);
		}
		if (!TargetScalarIds.Contains(Parameter.Id) && TargetNames.Contains(Parameter.ParameterName))
		{
			return BPI_UDS97Result(TEXT("MergeMaterialParameterCollectionAdditiveChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Scalar name conflict"), nullptr);
		}
	}
	for (const FCollectionVectorParameter& Parameter : Source->VectorParameters)
	{
		if (TargetScalarIds.Contains(Parameter.Id))
		{
			return BPI_UDS97Result(TEXT("MergeMaterialParameterCollectionAdditiveChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Vector/scalar GUID conflict"), nullptr);
		}
		if (!TargetVectorIds.Contains(Parameter.Id) && TargetNames.Contains(Parameter.ParameterName))
		{
			return BPI_UDS97Result(TEXT("MergeMaterialParameterCollectionAdditiveChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Vector name conflict"), nullptr);
		}
	}

	int32 AddedScalars = 0;
	int32 AddedVectors = 0;
	Target->Modify();
	for (const FCollectionScalarParameter& Parameter : Source->ScalarParameters)
	{
		if (!TargetScalarIds.Contains(Parameter.Id))
		{
			Target->ScalarParameters.Add(Parameter);
			++AddedScalars;
		}
	}
	for (const FCollectionVectorParameter& Parameter : Source->VectorParameters)
	{
		if (!TargetVectorIds.Contains(Parameter.Id))
		{
			Target->VectorParameters.Add(Parameter);
			++AddedVectors;
		}
	}

	bool bSaved = true;
	if (AddedScalars > 0 || AddedVectors > 0)
	{
		Target->PostEditChange();
		bSaved = UEditorAssetLibrary::SaveLoadedAsset(Target, false);
	}
	const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetNumberField(TEXT("addedScalars"), AddedScalars);
	Details->SetNumberField(TEXT("addedVectors"), AddedVectors);
	Details->SetBoolField(TEXT("saved"), bSaved);
	const bool bOk = bSaved;
	return BPI_UDS97Result(
		TEXT("MergeMaterialParameterCollectionAdditiveChecked"),
		bOk,
		bOk ? (AddedScalars > 0 || AddedVectors > 0 ? TEXT("success") : TEXT("no_change")) : TEXT("failed_after_dispatch"),
		bOk ? TEXT("") : TEXT("Save failed"),
		Details);
}

FString UMCPInspectorLibrary::RemapMPCBlueprintCallChecked(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& NodeGuid,
	const FString& CollectionPath,
	const FString& ExpectedSourceName,
	const FString& RetainedTargetName,
	const FString& ExpectedSharedGuid)
{
	UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
	UMaterialParameterCollection* Collection = Cast<UMaterialParameterCollection>(
		StaticLoadObject(UMaterialParameterCollection::StaticClass(), nullptr, *CollectionPath));
	FGuid ExpectedGuid;
	if (Blueprint == nullptr || Collection == nullptr || !FGuid::Parse(ExpectedSharedGuid, ExpectedGuid))
	{
		return BPI_UDS97Result(TEXT("RemapMPCBlueprintCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Invalid Blueprint, MPC, or GUID"), nullptr);
	}
	if (Collection->GetParameterId(FName(*RetainedTargetName)) != ExpectedGuid)
	{
		return BPI_UDS97Result(TEXT("RemapMPCBlueprintCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Retained target GUID mismatch"), nullptr);
	}

	UK2Node_CallMaterialParameterCollectionFunction* Match = nullptr;
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph == nullptr || Graph->GetName() != GraphName)
		{
			continue;
		}
		for (UEdGraphNode* GraphNode : Graph->Nodes)
		{
			UK2Node_CallMaterialParameterCollectionFunction* Node = Cast<UK2Node_CallMaterialParameterCollectionFunction>(GraphNode);
			if (Node != nullptr && Node->NodeGuid.ToString(EGuidFormats::Digits).Equals(NodeGuid, ESearchCase::IgnoreCase))
			{
				if (Match != nullptr)
				{
					return BPI_UDS97Result(TEXT("RemapMPCBlueprintCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Ambiguous node identity"), nullptr);
				}
				Match = Node;
			}
		}
	}
	if (Match == nullptr)
	{
		return BPI_UDS97Result(TEXT("RemapMPCBlueprintCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Node not found"), nullptr);
	}
	UEdGraphPin* CollectionPin = Match->FindPin(TEXT("Collection"));
	UEdGraphPin* ParameterPin = Match->FindPin(TEXT("ParameterName"));
	if (CollectionPin == nullptr || CollectionPin->DefaultObject != Collection || ParameterPin == nullptr || ParameterPin->LinkedTo.Num() > 0)
	{
		return BPI_UDS97Result(TEXT("RemapMPCBlueprintCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Node binding is not the expected literal MPC call"), nullptr);
	}
	if (ParameterPin->DefaultValue == RetainedTargetName)
	{
		return BPI_UDS97Result(TEXT("RemapMPCBlueprintCallChecked"), true, TEXT("no_change"), TEXT(""), MakeShared<FJsonObject>());
	}
	if (ParameterPin->DefaultValue != ExpectedSourceName)
	{
		return BPI_UDS97Result(TEXT("RemapMPCBlueprintCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Source literal mismatch"), nullptr);
	}

	Match->Modify();
	ParameterPin->Modify();
	const UEdGraphSchema* Schema = Match->GetGraph() != nullptr ? Match->GetGraph()->GetSchema() : nullptr;
	if (Schema == nullptr)
	{
		return BPI_UDS97Result(TEXT("RemapMPCBlueprintCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Graph schema not found"), nullptr);
	}
	Schema->TrySetDefaultValue(*ParameterPin, RetainedTargetName);
	if (ParameterPin->DefaultValue != RetainedTargetName)
	{
		return BPI_UDS97Result(TEXT("RemapMPCBlueprintCallChecked"), false, TEXT("failed_after_dispatch"), TEXT("Schema rejected literal"), nullptr);
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	const bool bCompiled = Blueprint->Status != BS_Error;
	const bool bSaved = bCompiled && UEditorAssetLibrary::SaveLoadedAsset(Blueprint, false);
	const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetBoolField(TEXT("compiled"), bCompiled);
	Details->SetBoolField(TEXT("saved"), bSaved);
	Details->SetStringField(TEXT("finalName"), ParameterPin->DefaultValue);
	return BPI_UDS97Result(
		TEXT("RemapMPCBlueprintCallChecked"),
		bCompiled && bSaved && ParameterPin->DefaultValue == RetainedTargetName,
		bCompiled && bSaved ? TEXT("success") : TEXT("failed_after_dispatch"),
		bCompiled && bSaved ? TEXT("") : TEXT("Compile or save failed"),
		Details);
}

FString UMCPInspectorLibrary::ReparentBlueprintChecked(
	const FString& BlueprintPath,
	const FString& ExpectedOldParent,
	const FString& NewParent)
{
	UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
	UClass* ExpectedClass = LoadObject<UClass>(nullptr, *ExpectedOldParent);
	UClass* NewParentClass = LoadObject<UClass>(nullptr, *NewParent);
	if (Blueprint == nullptr || ExpectedClass == nullptr || NewParentClass == nullptr)
	{
		return BPI_UDS97Result(TEXT("ReparentBlueprintChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Blueprint or parent class not found"), nullptr);
	}
	if (Blueprint->ParentClass == NewParentClass)
	{
		return BPI_UDS97Result(TEXT("ReparentBlueprintChecked"), true, TEXT("no_change"), TEXT(""), MakeShared<FJsonObject>());
	}
	if (Blueprint->ParentClass != ExpectedClass)
	{
		return BPI_UDS97Result(TEXT("ReparentBlueprintChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Old parent mismatch"), nullptr);
	}
	UBlueprintEditorLibrary::ReparentBlueprint(Blueprint, NewParentClass);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	const bool bCompiled = Blueprint->Status != BS_Error;
	const bool bSaved = bCompiled && UEditorAssetLibrary::SaveLoadedAsset(Blueprint, false);
	const bool bVerified = Blueprint->ParentClass == NewParentClass;
	const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetBoolField(TEXT("compiled"), bCompiled);
	Details->SetBoolField(TEXT("saved"), bSaved);
	Details->SetStringField(TEXT("parent"), Blueprint->ParentClass != nullptr ? Blueprint->ParentClass->GetPathName() : TEXT(""));
	return BPI_UDS97Result(
		TEXT("ReparentBlueprintChecked"),
		bCompiled && bSaved && bVerified,
		bCompiled && bSaved && bVerified ? TEXT("success") : TEXT("failed_after_dispatch"),
		bCompiled && bSaved && bVerified ? TEXT("") : TEXT("Compile, save, or readback failed"),
		Details);
}

FString UMCPInspectorLibrary::ReconnectMaterialFunctionCallChecked(
	const FString& CallerPath,
	const FString& NodeId,
	const FString& ExpectedFunctionPath,
	const FString& ExpectedFunctionContractJson,
	const FString& ReconnectSpecJson)
{
	TArray<UMaterialExpression*> Expressions;
	UMaterial* Material = nullptr;
	BPI_UDS97CollectExpressions(CallerPath, Expressions, Material);
	UObject* CallerAsset = StaticLoadObject(UObject::StaticClass(), nullptr, *CallerPath);
	if (CallerAsset == nullptr || Expressions.Num() == 0)
	{
		return BPI_UDS97Result(TEXT("ReconnectMaterialFunctionCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Caller not found"), nullptr);
	}

	UMaterialExpressionMaterialFunctionCall* Call = nullptr;
	TMap<FString, UMaterialExpression*> ExpressionByPath;
	for (UMaterialExpression* Expression : Expressions)
	{
		if (Expression == nullptr)
		{
			continue;
		}
		ExpressionByPath.Add(Expression->GetPathName(), Expression);
		if (Expression->GetPathName() == NodeId)
		{
			if (Call != nullptr)
			{
				return BPI_UDS97Result(TEXT("ReconnectMaterialFunctionCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Ambiguous call node"), nullptr);
			}
			Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
		}
	}
	if (Call == nullptr || Call->MaterialFunction == nullptr || Call->MaterialFunction->GetOutermost()->GetName() != ExpectedFunctionPath)
	{
		return BPI_UDS97Result(TEXT("ReconnectMaterialFunctionCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Call or function identity mismatch"), nullptr);
	}
	if (DumpMaterialFunctionContractJson(ExpectedFunctionPath) != ExpectedFunctionContractJson)
	{
		return BPI_UDS97Result(TEXT("ReconnectMaterialFunctionCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Installed function contract mismatch"), nullptr);
	}

	TSharedPtr<FJsonObject> Spec;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ReconnectSpecJson);
	if (!FJsonSerializer::Deserialize(Reader, Spec) || !Spec.IsValid())
	{
		return BPI_UDS97Result(TEXT("ReconnectMaterialFunctionCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Invalid reconnect JSON"), nullptr);
	}

	const TArray<TSharedPtr<FJsonValue>>* InputSpecs = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* DownstreamSpecs = nullptr;
	if (!Spec->TryGetArrayField(TEXT("inputs"), InputSpecs) || InputSpecs == nullptr || InputSpecs->Num() != Call->FunctionInputs.Num())
	{
		return BPI_UDS97Result(TEXT("ReconnectMaterialFunctionCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Input specification count mismatch"), nullptr);
	}
	if (!Spec->TryGetArrayField(TEXT("downstream"), DownstreamSpecs) || DownstreamSpecs == nullptr)
	{
		return BPI_UDS97Result(TEXT("ReconnectMaterialFunctionCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Downstream specification missing"), nullptr);
	}

	TArray<int32> ResolvedInputIndices;
	TArray<UMaterialExpression*> ResolvedInputSources;
	for (const TSharedPtr<FJsonValue>& InputValue : *InputSpecs)
	{
		const TSharedPtr<FJsonObject> InputSpec = InputValue.IsValid() ? InputValue->AsObject() : nullptr;
		if (!InputSpec.IsValid())
		{
			return BPI_UDS97Result(TEXT("ReconnectMaterialFunctionCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Invalid input row"), nullptr);
		}
		FString InputName;
		FString SourceNode;
		if (!InputSpec->TryGetStringField(TEXT("name"), InputName) || !InputSpec->TryGetStringField(TEXT("sourceNode"), SourceNode))
		{
			return BPI_UDS97Result(TEXT("ReconnectMaterialFunctionCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Input name/source missing"), nullptr);
		}
		int32 ResolvedIndex = INDEX_NONE;
		for (int32 InputIndex = 0; InputIndex < Call->FunctionInputs.Num(); ++InputIndex)
		{
			const FFunctionExpressionInput& FunctionInput = Call->FunctionInputs[InputIndex];
			const FString CurrentName = FunctionInput.ExpressionInput != nullptr
				? FunctionInput.ExpressionInput->InputName.ToString()
				: Call->GetInputName(InputIndex).ToString();
			if (CurrentName == InputName)
			{
				if (ResolvedIndex != INDEX_NONE)
				{
					return BPI_UDS97Result(TEXT("ReconnectMaterialFunctionCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Ambiguous semantic input"), nullptr);
				}
				ResolvedIndex = InputIndex;
			}
		}
		if (ResolvedIndex == INDEX_NONE)
		{
			return BPI_UDS97Result(TEXT("ReconnectMaterialFunctionCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Semantic input not found"), nullptr);
		}
		UMaterialExpression* SourceExpression = nullptr;
		if (!SourceNode.IsEmpty())
		{
			UMaterialExpression** FoundSource = ExpressionByPath.Find(SourceNode);
			if (FoundSource == nullptr || *FoundSource == nullptr)
			{
				return BPI_UDS97Result(TEXT("ReconnectMaterialFunctionCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Input source node not found"), nullptr);
			}
			SourceExpression = *FoundSource;
		}
		ResolvedInputIndices.Add(ResolvedIndex);
		ResolvedInputSources.Add(SourceExpression);
	}

	TArray<UMaterialExpression*> ResolvedConsumers;
	TArray<int32> ResolvedConsumerInputs;
	for (const TSharedPtr<FJsonValue>& DownstreamValue : *DownstreamSpecs)
	{
		const TSharedPtr<FJsonObject> DownstreamSpec = DownstreamValue.IsValid() ? DownstreamValue->AsObject() : nullptr;
		if (!DownstreamSpec.IsValid())
		{
			return BPI_UDS97Result(TEXT("ReconnectMaterialFunctionCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Invalid downstream row"), nullptr);
		}
		FString ConsumerNode;
		FString ConsumerInput;
		if (!DownstreamSpec->TryGetStringField(TEXT("consumerNode"), ConsumerNode) || !DownstreamSpec->TryGetStringField(TEXT("consumerInput"), ConsumerInput))
		{
			return BPI_UDS97Result(TEXT("ReconnectMaterialFunctionCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Downstream identity missing"), nullptr);
		}
		if (ConsumerNode == TEXT("MaterialRoot"))
		{
			if (Material == nullptr || ConsumerInput != TEXT("MaterialAttributes"))
			{
				return BPI_UDS97Result(TEXT("ReconnectMaterialFunctionCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Invalid material-root downstream"), nullptr);
			}
			ResolvedConsumers.Add(nullptr);
			ResolvedConsumerInputs.Add(INDEX_NONE);
			continue;
		}
		UMaterialExpression** FoundConsumer = ExpressionByPath.Find(ConsumerNode);
		if (FoundConsumer == nullptr || *FoundConsumer == nullptr)
		{
			return BPI_UDS97Result(TEXT("ReconnectMaterialFunctionCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Downstream consumer not found"), nullptr);
		}
		int32 ResolvedConsumerInput = INDEX_NONE;
		for (int32 InputIndex = 0; InputIndex < (*FoundConsumer)->CountInputs(); ++InputIndex)
		{
			if ((*FoundConsumer)->GetInputName(InputIndex).ToString() == ConsumerInput)
			{
				if (ResolvedConsumerInput != INDEX_NONE)
				{
					return BPI_UDS97Result(TEXT("ReconnectMaterialFunctionCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Ambiguous downstream input"), nullptr);
				}
				ResolvedConsumerInput = InputIndex;
			}
		}
		if (ResolvedConsumerInput == INDEX_NONE)
		{
			return BPI_UDS97Result(TEXT("ReconnectMaterialFunctionCallChecked"), false, TEXT("failed_pre_dispatch"), TEXT("Downstream input not found"), nullptr);
		}
		ResolvedConsumers.Add(*FoundConsumer);
		ResolvedConsumerInputs.Add(ResolvedConsumerInput);
	}

	Call->Modify();
	for (int32 SpecIndex = 0; SpecIndex < InputSpecs->Num(); ++SpecIndex)
	{
		const TSharedPtr<FJsonObject> InputSpec = (*InputSpecs)[SpecIndex]->AsObject();
		FFunctionExpressionInput& FunctionInput = Call->FunctionInputs[ResolvedInputIndices[SpecIndex]];
		FunctionInput.Input.Expression = ResolvedInputSources[SpecIndex];
		double NumericValue = 0.0;
		if (InputSpec->TryGetNumberField(TEXT("sourceOutputIndex"), NumericValue))
		{
			FunctionInput.Input.OutputIndex = static_cast<int32>(NumericValue);
		}
		if (InputSpec->TryGetNumberField(TEXT("mask"), NumericValue))
		{
			FunctionInput.Input.Mask = static_cast<int32>(NumericValue);
		}
		if (InputSpec->TryGetNumberField(TEXT("maskR"), NumericValue))
		{
			FunctionInput.Input.MaskR = static_cast<int32>(NumericValue);
		}
		if (InputSpec->TryGetNumberField(TEXT("maskG"), NumericValue))
		{
			FunctionInput.Input.MaskG = static_cast<int32>(NumericValue);
		}
		if (InputSpec->TryGetNumberField(TEXT("maskB"), NumericValue))
		{
			FunctionInput.Input.MaskB = static_cast<int32>(NumericValue);
		}
		if (InputSpec->TryGetNumberField(TEXT("maskA"), NumericValue))
		{
			FunctionInput.Input.MaskA = static_cast<int32>(NumericValue);
		}
	}

	for (int32 SpecIndex = 0; SpecIndex < DownstreamSpecs->Num(); ++SpecIndex)
	{
		const TSharedPtr<FJsonObject> DownstreamSpec = (*DownstreamSpecs)[SpecIndex]->AsObject();
		double OutputValue = 0.0;
		if (!DownstreamSpec->TryGetNumberField(TEXT("callOutputIndex"), OutputValue))
		{
			return BPI_UDS97Result(TEXT("ReconnectMaterialFunctionCallChecked"), false, TEXT("failed_after_dispatch"), TEXT("Downstream output index missing"), nullptr);
		}
		FExpressionInput* ConsumerInput = ResolvedConsumers[SpecIndex] == nullptr
			? Material->GetExpressionInputForProperty(MP_MaterialAttributes)
			: ResolvedConsumers[SpecIndex]->GetInput(ResolvedConsumerInputs[SpecIndex]);
		if (ConsumerInput == nullptr)
		{
			return BPI_UDS97Result(TEXT("ReconnectMaterialFunctionCallChecked"), false, TEXT("failed_after_dispatch"), TEXT("Downstream input disappeared"), nullptr);
		}
		ConsumerInput->Expression = Call;
		ConsumerInput->OutputIndex = static_cast<int32>(OutputValue);
	}

	CallerAsset->PostEditChange();
	CallerAsset->MarkPackageDirty();
	const bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(CallerAsset, false);
	const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetBoolField(TEXT("saved"), bSaved);
	Details->SetStringField(TEXT("callerContract"), DumpMaterialFunctionCallerContractJson(CallerPath, TArray<FString>({ExpectedFunctionPath})));
	return BPI_UDS97Result(
		TEXT("ReconnectMaterialFunctionCallChecked"),
		bSaved,
		bSaved ? TEXT("success") : TEXT("failed_after_dispatch"),
		bSaved ? TEXT("") : TEXT("Save failed"),
		Details);
}

// 머티리얼 결과(Root) 노드 위치 설정 — UMaterial::EditorX/EditorY.
//   결과 노드 위치는 UMaterial 에 저장(MaterialGraphNode_Root 가 NodePosX=Material->EditorX 로 읽음).
//   EditorX/Y 는 플래그 없는 UPROPERTY 라 Python 미노출 → cpp 전용. 저장은 호출자.
bool UMCPInspectorLibrary::SetMaterialRootNodePosition(const FString& AssetPath, int32 PosX, int32 PosY)
{
#if WITH_EDITORONLY_DATA
	UMaterial* Mat = Cast<UMaterial>(
		StaticLoadObject(UMaterial::StaticClass(), nullptr, *AssetPath));
	if (!Mat)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: 베이스 UMaterial 로드 실패(Root pos): %s"), *AssetPath);
		return false;
	}
	Mat->Modify();
	Mat->EditorX = PosX;
	Mat->EditorY = PosY;
	Mat->MarkPackageDirty();
	return true;
#else
	return false;
#endif
}

// Custom Data 0/1 핀에 상수 연결.
//   Python 의 unreal.MaterialProperty 에는 MP_CustomData0/1 이 없어(노출 누락) 이 핀만 파이썬으로 못 건드린다.
//   ★핀이 비활성인 셰이딩 모델은 그래프 연결을 무시하고 하드 디폴트를 컴파일한다(Material.cpp IsPropertyActive).
//     연결해도 화면이 안 바뀌면 셰이딩 모델이 활성 목록에 없는 것이다.
bool UMCPInspectorLibrary::SetMaterialCustomDataConstant(const FString& AssetPath, int32 CustomDataIndex, float Value)
{
#if WITH_EDITORONLY_DATA
	if (CustomDataIndex != 0 && CustomDataIndex != 1)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: CustomDataIndex 는 0 또는 1: %d"), CustomDataIndex);
		return false;
	}

	UMaterial* Mat = Cast<UMaterial>(
		StaticLoadObject(UMaterial::StaticClass(), nullptr, *AssetPath));
	if (!Mat)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: 베이스 UMaterial 로드 실패(CustomData): %s"), *AssetPath);
		return false;
	}

	UMaterialEditorOnlyData* EditorOnly = Mat->GetEditorOnlyData();
	if (!EditorOnly)
	{
		return false;
	}

	Mat->Modify();

	UMaterialExpressionConstant* ConstExpr = NewObject<UMaterialExpressionConstant>(Mat);
	ConstExpr->R = Value;
	ConstExpr->MaterialExpressionEditorX = -400;
	ConstExpr->MaterialExpressionEditorY = 600;
	Mat->GetExpressionCollection().AddExpression(ConstExpr);

	// UMaterialEditorOnlyData 는 CustomData0/1 을 ClearCoat / ClearCoatRoughness 필드로 재사용한다.
	// (MP_CustomData1 의 하드 디폴트가 0.1 인 것도 ClearCoatRoughness 기본값에서 온다)
	FScalarMaterialInput& Input = (CustomDataIndex == 0) ? EditorOnly->ClearCoat : EditorOnly->ClearCoatRoughness;

	// ★UseConstant 는 Expression 보다 우선한다(MaterialShared.cpp FScalarMaterialInput::CompileWithDefault).
	//   핀이 비활성이던 머티리얼에는 UseConstant=1/Constant=0 이 잠들어 있을 수 있어, 끄지 않으면 연결이 무시된다.
	Input.UseConstant = 0;
	Input.Constant = Value;
	Input.Expression = ConstExpr;
	Input.OutputIndex = 0;

	Mat->PostEditChange();
	Mat->MarkPackageDirty();
	return true;
#else
	return false;
#endif
}

FString UMCPInspectorLibrary::GetMaterialCustomDataInfo(const FString& AssetPath, int32 CustomDataIndex)
{
#if WITH_EDITORONLY_DATA
	if (CustomDataIndex != 0 && CustomDataIndex != 1)
	{
		return TEXT("ERROR: index must be 0 or 1");
	}

	UMaterial* Mat = Cast<UMaterial>(
		StaticLoadObject(UMaterial::StaticClass(), nullptr, *AssetPath));
	if (!Mat)
	{
		return TEXT("ERROR: not a base UMaterial");
	}

	UMaterialEditorOnlyData* EditorOnly = Mat->GetEditorOnlyData();
	if (!EditorOnly)
	{
		return TEXT("ERROR: no editor-only data");
	}

	const FScalarMaterialInput& Input = (CustomDataIndex == 0) ? EditorOnly->ClearCoat : EditorOnly->ClearCoatRoughness;
	const FString ExprName = Input.Expression ? Input.Expression->GetName() : TEXT("None");

	return FString::Printf(TEXT("UseConstant=%d Constant=%.4f Expression=%s"),
		Input.UseConstant ? 1 : 0, Input.Constant, *ExprName);
#else
	return TEXT("ERROR: no editor-only data");
#endif
}
