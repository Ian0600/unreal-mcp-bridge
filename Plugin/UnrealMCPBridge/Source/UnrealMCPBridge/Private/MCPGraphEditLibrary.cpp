#include "MCPGraphEditLibrary.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "UObject/Interface.h"

#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Kismet/KismetMathLibrary.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UnrealMCPBridgeModule.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/BlendSpace.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimationStateMachineGraph.h"
#include "AnimStateNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateEntryNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_Slot.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimGraphNode_UseCachedPose.h"

UEdGraph* UMCPGraphEditLibrary::FindGraphByName(UBlueprint* BP, const FString& GraphName)
{
    if (!BP) return nullptr;

    auto Search = [&](const TArray<UEdGraph*>& Graphs) -> UEdGraph*
    {
        for (UEdGraph* G : Graphs)
        {
            if (G && G->GetName() == GraphName) return G;
        }
        return nullptr;
    };

    if (UEdGraph* G = Search(BP->UbergraphPages)) return G;
    if (UEdGraph* G = Search(BP->FunctionGraphs)) return G;
    if (UEdGraph* G = Search(BP->MacroGraphs))    return G;

    // Not a top-level graph — check nested AnimGraph graphs (state machine inner graphs,
    // state bound graphs, transition rule graphs), which BP's top-level arrays don't include.
    auto SearchStateMachineNested = [&](UEdGraph* TopGraph) -> UEdGraph*
    {
        if (!TopGraph) return nullptr;
        for (UEdGraphNode* Node : TopGraph->Nodes)
        {
            UAnimGraphNode_StateMachineBase* MachineNode = Cast<UAnimGraphNode_StateMachineBase>(Node);
            if (!MachineNode || !MachineNode->EditorStateMachineGraph) continue;

            UAnimationStateMachineGraph* InnerGraph = MachineNode->EditorStateMachineGraph;
            if (InnerGraph->GetName() == GraphName) return InnerGraph;

            for (UEdGraphNode* InnerNode : InnerGraph->Nodes)
            {
                if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(InnerNode))
                {
                    if (StateNode->BoundGraph && StateNode->BoundGraph->GetName() == GraphName)
                    {
                        return StateNode->BoundGraph;
                    }
                }
                else if (UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(InnerNode))
                {
                    if (TransNode->BoundGraph && TransNode->BoundGraph->GetName() == GraphName)
                    {
                        return TransNode->BoundGraph;
                    }
                }
            }
        }
        return nullptr;
    };

    for (UEdGraph* TopGraph : BP->FunctionGraphs)
    {
        if (UEdGraph* Found = SearchStateMachineNested(TopGraph)) return Found;
    }

    return nullptr;
}

UEdGraphNode* UMCPGraphEditLibrary::FindNodeByName(UEdGraph* Graph, const FString& NodeName)
{
    if (!Graph) return nullptr;
    for (UEdGraphNode* N : Graph->Nodes)
    {
        if (N && N->GetName() == NodeName) return N;
    }
    return nullptr;
}

UEdGraphPin* UMCPGraphEditLibrary::FindPinByName(UEdGraphNode* Node, const FString& PinName)
{
    if (!Node) return nullptr;
    for (UEdGraphPin* P : Node->Pins)
    {
        if (P && P->PinName.ToString() == PinName) return P;
    }
    return nullptr;
}

FString UMCPGraphEditLibrary::AddCallFunctionNode(
    UBlueprint* Blueprint,
    const FString& GraphName,
    const FString& FunctionClassPath,
    const FString& FunctionName,
    int32 PosX,
    int32 PosY)
{
    if (!Blueprint)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddCallFunctionNode: Blueprint is null"));
        return FString();
    }

    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddCallFunctionNode: graph '%s' not found"), *GraphName);
        return FString();
    }

    UClass* FuncClass = LoadObject<UClass>(nullptr, *FunctionClassPath);
    if (!FuncClass)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddCallFunctionNode: class '%s' not found"), *FunctionClassPath);
        return FString();
    }

    UFunction* Func = FuncClass->FindFunctionByName(FName(*FunctionName));
    if (!Func)
    {
        UE_LOG(LogMCPBridge, Warning,
            TEXT("AddCallFunctionNode: function '%s' not found on '%s'"),
            *FunctionName, *FunctionClassPath);
        return FString();
    }

    UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
    Graph->AddNode(Node, /*bFromUI*/ false, /*bSelectNewNode*/ false);
    Node->CreateNewGuid();
    Node->SetFromFunction(Func);
    Node->NodePosX = PosX;
    Node->NodePosY = PosY;
    Node->AllocateDefaultPins();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Blueprint->MarkPackageDirty();
    return Node->GetName();
}

FString UMCPGraphEditLibrary::AddIfThenElseNode(
    UBlueprint* Blueprint,
    const FString& GraphName,
    int32 PosX,
    int32 PosY)
{
    if (!Blueprint)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddIfThenElseNode: Blueprint is null"));
        return FString();
    }

    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddIfThenElseNode: graph '%s' not found"), *GraphName);
        return FString();
    }

    UK2Node_IfThenElse* Node = NewObject<UK2Node_IfThenElse>(Graph);
    Graph->AddNode(Node, /*bFromUI*/ false, /*bSelectNewNode*/ false);
    Node->CreateNewGuid();
    Node->NodePosX = PosX;
    Node->NodePosY = PosY;
    Node->AllocateDefaultPins();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Blueprint->MarkPackageDirty();
    return Node->GetName();
}

FString UMCPGraphEditLibrary::AddVariableGetNode(
    UBlueprint* Blueprint,
    const FString& GraphName,
    const FString& PropertyName,
    int32 PosX,
    int32 PosY)
{
    if (!Blueprint)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddVariableGetNode: Blueprint is null"));
        return FString();
    }

    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddVariableGetNode: graph '%s' not found"), *GraphName);
        return FString();
    }

    UClass* InstanceClass = Blueprint->GeneratedClass ? Blueprint->GeneratedClass : Blueprint->ParentClass;
    FProperty* Prop = InstanceClass ? InstanceClass->FindPropertyByName(FName(*PropertyName)) : nullptr;
    if (!Prop)
    {
        UE_LOG(LogMCPBridge, Warning,
            TEXT("AddVariableGetNode: property '%s' not found on %s"),
            *PropertyName, InstanceClass ? *InstanceClass->GetName() : TEXT("<null>"));
        return FString();
    }

    UK2Node_VariableGet* Node = NewObject<UK2Node_VariableGet>(Graph);
    Graph->AddNode(Node, /*bFromUI*/ false, /*bSelectNewNode*/ false);
    Node->CreateNewGuid();
    Node->VariableReference.SetSelfMember(Prop->GetFName());
    Node->NodePosX = PosX;
    Node->NodePosY = PosY;
    Node->AllocateDefaultPins();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Blueprint->MarkPackageDirty();
    return Node->GetName();
}

bool UMCPGraphEditLibrary::ConnectPins(
    UBlueprint* Blueprint,
    const FString& GraphName,
    const FString& SrcNodeName,
    const FString& SrcPinName,
    const FString& DstNodeName,
    const FString& DstPinName)
{
    if (!Blueprint) return false;
    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph) return false;

    UEdGraphNode* SrcNode = FindNodeByName(Graph, SrcNodeName);
    UEdGraphNode* DstNode = FindNodeByName(Graph, DstNodeName);
    if (!SrcNode || !DstNode)
    {
        UE_LOG(LogMCPBridge, Warning,
            TEXT("ConnectPins: node not found (src=%s found=%d, dst=%s found=%d)"),
            *SrcNodeName, SrcNode != nullptr, *DstNodeName, DstNode != nullptr);
        return false;
    }

    UEdGraphPin* SrcPin = FindPinByName(SrcNode, SrcPinName);
    UEdGraphPin* DstPin = FindPinByName(DstNode, DstPinName);
    if (!SrcPin || !DstPin)
    {
        UE_LOG(LogMCPBridge, Warning,
            TEXT("ConnectPins: pin not found (src=%s.%s found=%d, dst=%s.%s found=%d)"),
            *SrcNodeName, *SrcPinName, SrcPin != nullptr,
            *DstNodeName, *DstPinName, DstPin != nullptr);
        return false;
    }

    // Through the schema, not SrcPin->MakeLinkTo(DstPin): a raw MakeLinkTo produces a link that reads
    // back correctly (and compiles without complaint) while the anim compiler never turns it into a
    // pose LinkID, so the graph silently evaluates to ref pose. TryCreateConnection is what the editor
    // itself does on a drag — it validates the pair, notifies both nodes, and inserts any conversion
    // node the pair needs (e.g. local <-> component space pose).
    const UEdGraphSchema* Schema = Graph->GetSchema();
    if (!Schema)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("ConnectPins: graph '%s' has no schema"), *GraphName);
        return false;
    }

    const FPinConnectionResponse Response = Schema->CanCreateConnection(SrcPin, DstPin);
    if (Response.Response == CONNECT_RESPONSE_DISALLOW)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("ConnectPins: schema refused %s.%s -> %s.%s: %s"),
            *SrcNodeName, *SrcPinName, *DstNodeName, *DstPinName, *Response.Message.ToString());
        return false;
    }

    if (!Schema->TryCreateConnection(SrcPin, DstPin))
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("ConnectPins: TryCreateConnection failed %s.%s -> %s.%s"),
            *SrcNodeName, *SrcPinName, *DstNodeName, *DstPinName);
        return false;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Blueprint->MarkPackageDirty();
    return true;
}

bool UMCPGraphEditLibrary::DisconnectPinLink(
    UBlueprint* Blueprint,
    const FString& GraphName,
    const FString& NodeA,
    const FString& PinA,
    const FString& NodeB,
    const FString& PinB)
{
    if (!Blueprint) return false;
    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph) return false;

    UEdGraphNode* NA = FindNodeByName(Graph, NodeA);
    UEdGraphNode* NB = FindNodeByName(Graph, NodeB);
    if (!NA || !NB) return false;

    UEdGraphPin* PA = FindPinByName(NA, PinA);
    UEdGraphPin* PB = FindPinByName(NB, PinB);
    if (!PA || !PB) return false;

    PA->BreakLinkTo(PB);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Blueprint->MarkPackageDirty();
    return true;
}

bool UMCPGraphEditLibrary::EditBlendSpaceSampleValue(
    UBlendSpace* BlendSpace,
    int32 SampleIndex,
    float X,
    float Y)
{
    if (!BlendSpace) return false;

    BlendSpace->Modify();
    const bool bOk = BlendSpace->EditSampleValue(SampleIndex, FVector(X, Y, 0.f));
    if (bOk)
    {
        BlendSpace->PostEditChange();
        BlendSpace->MarkPackageDirty();
    }
    return bOk;
}

bool UMCPGraphEditLibrary::SetBlendSpaceAxisRange(
    UBlendSpace* BlendSpace,
    int32 AxisIndex,
    float Min,
    float Max)
{
    if (!BlendSpace || AxisIndex < 0 || AxisIndex > 2) return false;

    // BlendParameters is protected, so go through FProperty reflection (which ignores C++ access
    // specifiers) rather than needing a public setter that doesn't exist on UBlendSpace.
    FStructProperty* Prop = FindFProperty<FStructProperty>(UBlendSpace::StaticClass(), TEXT("BlendParameters"));
    if (!Prop) return false;

    FBlendParameter* Param = Prop->ContainerPtrToValuePtr<FBlendParameter>(BlendSpace, AxisIndex);
    if (!Param) return false;

    BlendSpace->Modify();
    Param->Min = Min;
    Param->Max = Max;
    BlendSpace->ValidateSampleData();
    BlendSpace->PostEditChange();
    BlendSpace->MarkPackageDirty();
    return true;
}

bool UMCPGraphEditLibrary::CompileBlueprint(UBlueprint* Blueprint)
{
    if (!Blueprint) return false;
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    return Blueprint->Status != BS_Error;
}

bool UMCPGraphEditLibrary::RemoveNode(
    UBlueprint* Blueprint,
    const FString& GraphName,
    const FString& NodeName)
{
    if (!Blueprint) return false;
    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph) return false;
    UEdGraphNode* Node = FindNodeByName(Graph, NodeName);
    if (!Node) return false;
    Node->Modify();
    Graph->Modify();
    // DestroyNode() (not a manual BreakAllNodeLinks+RemoveNode) so node-type-specific cleanup runs —
    // e.g. UAnimGraphNode_StateMachineBase::DestroyNode() also removes its EditorStateMachineGraph
    // via FBlueprintEditorUtils::RemoveGraph, which a plain Graph->RemoveNode() would silently skip
    // (leaving an orphaned inner graph behind).
    Node->DestroyNode();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Blueprint->MarkPackageDirty();
    return true;
}

bool UMCPGraphEditLibrary::RemoveOrphanedGraph(
    UBlueprint* Blueprint,
    const FString& GraphName)
{
    if (!Blueprint) return false;

    UEdGraph* Found = nullptr;
    ForEachObjectWithOuter(Blueprint, [&](UObject* Obj)
    {
        if (Found) return;
        if (UEdGraph* G = Cast<UEdGraph>(Obj))
        {
            if (G->GetName() == GraphName)
            {
                Found = G;
            }
        }
    }, /*bIncludeNestedObjects=*/true); // UE 5.3: ForEachObjectWithOuter takes a bool, not EGetObjectsFlags

    if (!Found)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("RemoveOrphanedGraph: no graph named '%s' found under %s"), *GraphName, *Blueprint->GetName());
        return false;
    }

    Found->Modify();
    FBlueprintEditorUtils::RemoveGraph(Blueprint, Found, EGraphRemoveFlags::Recompile);
    Blueprint->MarkPackageDirty();
    return true;
}

// ---- AnimGraph ----

UAnimStateNodeBase* UMCPGraphEditLibrary::FindStateByName(UAnimGraphNode_StateMachineBase* MachineNode, const FString& StateName)
{
    if (!MachineNode || !MachineNode->EditorStateMachineGraph) return nullptr;
    for (UEdGraphNode* N : MachineNode->EditorStateMachineGraph->Nodes)
    {
        if (UAnimStateNodeBase* StateNode = Cast<UAnimStateNodeBase>(N))
        {
            if (StateNode->GetStateName() == StateName)
            {
                return StateNode;
            }
        }
    }
    return nullptr;
}

FString UMCPGraphEditLibrary::AddAnimStateMachineNode(
    UAnimBlueprint* AnimBlueprint,
    const FString& GraphName,
    int32 PosX,
    int32 PosY)
{
    if (!AnimBlueprint)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddAnimStateMachineNode: AnimBlueprint is null"));
        return FString();
    }

    UEdGraph* Graph = FindGraphByName(AnimBlueprint, GraphName);
    if (!Graph)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddAnimStateMachineNode: graph '%s' not found"), *GraphName);
        return FString();
    }

    UAnimGraphNode_StateMachine* Node = NewObject<UAnimGraphNode_StateMachine>(Graph);
    Graph->AddNode(Node, /*bFromUI*/ false, /*bSelectNewNode*/ false);
    Node->CreateNewGuid();
    Node->NodePosX = PosX;
    Node->NodePosY = PosY;
    Node->AllocateDefaultPins();
    Node->PostPlacedNewNode(); // creates EditorStateMachineGraph + Entry node

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
    AnimBlueprint->MarkPackageDirty();
    return Node->GetName();
}

FString UMCPGraphEditLibrary::AddAnimState(
    UAnimBlueprint* AnimBlueprint,
    const FString& GraphName,
    const FString& StateMachineNodeName,
    const FString& StateName,
    int32 PosX,
    int32 PosY)
{
    if (!AnimBlueprint)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddAnimState: AnimBlueprint is null"));
        return FString();
    }

    UEdGraph* Graph = FindGraphByName(AnimBlueprint, GraphName);
    if (!Graph) return FString();

    UAnimGraphNode_StateMachineBase* MachineNode = Cast<UAnimGraphNode_StateMachineBase>(FindNodeByName(Graph, StateMachineNodeName));
    if (!MachineNode || !MachineNode->EditorStateMachineGraph)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddAnimState: state machine '%s' not found or has no inner graph"), *StateMachineNodeName);
        return FString();
    }

    UAnimationStateMachineGraph* InnerGraph = MachineNode->EditorStateMachineGraph;
    // UE 5.3: UAnimStateEntryNode has no GetOutputPin() (it derives from UEdGraphNode, not
    // UAnimStateNodeBase). Its single output pin is created in AllocateDefaultPins, so use Pins[0].
    UEdGraphPin* EntryOutPin = (InnerGraph->EntryNode && InnerGraph->EntryNode->Pins.Num() > 0)
        ? InnerGraph->EntryNode->Pins[0] : nullptr;
    const bool bIsFirstState = EntryOutPin && EntryOutPin->LinkedTo.Num() == 0;

    UAnimStateNode* StateNode = NewObject<UAnimStateNode>(InnerGraph);
    InnerGraph->AddNode(StateNode, false, false);
    StateNode->CreateNewGuid();
    StateNode->NodePosX = PosX;
    StateNode->NodePosY = PosY;
    StateNode->AllocateDefaultPins();
    StateNode->PostPlacedNewNode(); // creates BoundGraph + Result sink
    StateNode->OnRenameNode(StateName);

    // Give the state's inner (bound) graph a predictable name matching StateName, so callers
    // can address it directly via GraphName (e.g. for AddBlendSpacePlayerToState, AddVariableGetNode,
    // ConnectPins) instead of having to discover an auto-generated graph name.
    if (StateNode->BoundGraph && StateNode->BoundGraph->GetName() != StateName)
    {
        StateNode->BoundGraph->Rename(*StateName, nullptr, REN_DontCreateRedirectors);
    }

    if (bIsFirstState && EntryOutPin)
    {
        EntryOutPin->MakeLinkTo(StateNode->GetInputPin());
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
    AnimBlueprint->MarkPackageDirty();
    return StateNode->GetName();
}

FString UMCPGraphEditLibrary::AddBlendSpacePlayerToState(
    UAnimBlueprint* AnimBlueprint,
    const FString& GraphName,
    const FString& StateMachineNodeName,
    const FString& StateName,
    const FString& BlendSpaceAssetPath,
    int32 PosX,
    int32 PosY)
{
    if (!AnimBlueprint) return FString();
    UEdGraph* Graph = FindGraphByName(AnimBlueprint, GraphName);
    if (!Graph) return FString();

    UAnimGraphNode_StateMachineBase* MachineNode = Cast<UAnimGraphNode_StateMachineBase>(FindNodeByName(Graph, StateMachineNodeName));
    if (!MachineNode)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddBlendSpacePlayerToState: state machine '%s' not found"), *StateMachineNodeName);
        return FString();
    }

    UAnimStateNode* StateNode = Cast<UAnimStateNode>(FindStateByName(MachineNode, StateName));
    if (!StateNode || !StateNode->BoundGraph)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddBlendSpacePlayerToState: state '%s' not found or has no bound graph"), *StateName);
        return FString();
    }

    UBlendSpace* BlendSpaceAsset = LoadObject<UBlendSpace>(nullptr, *BlendSpaceAssetPath);
    if (!BlendSpaceAsset)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddBlendSpacePlayerToState: could not load blend space '%s'"), *BlendSpaceAssetPath);
        return FString();
    }

    // Build it the way the editor does. A BlendSpacePlayer's axis pins (X/Y) are generated from the
    // *asset's* axes, so the asset has to be set before pins are allocated — raw NewObject +
    // AllocateDefaultPins() leaves the axis pins unreconstructed and nothing can bind to Speed.
    // FGraphNodeCreator::Finalize() runs PostPlacedNewNode + AllocateDefaultPins in the right order.
    FGraphNodeCreator<UAnimGraphNode_BlendSpacePlayer> Creator(*StateNode->BoundGraph);
    UAnimGraphNode_BlendSpacePlayer* BSNode = Creator.CreateNode(/*bSelectNewNode*/ false);
    BSNode->NodePosX = PosX;
    BSNode->NodePosY = PosY;
    BSNode->Node.SetBlendSpace(BlendSpaceAsset);
    Creator.Finalize();
    BSNode->ReconstructNode();

    UEdGraphPin* PoseSink = StateNode->GetPoseSinkPinInsideState();
    UEdGraphPin* PoseOutput = FindPinByName(BSNode, TEXT("Pose"));
    if (PoseSink && PoseOutput)
    {
        PoseOutput->MakeLinkTo(PoseSink);
    }
    else
    {
        UE_LOG(LogMCPBridge, Warning,
            TEXT("AddBlendSpacePlayerToState: could not connect Pose pins (sink=%d output=%d) — connect manually with ConnectPins"),
            PoseSink != nullptr, PoseOutput != nullptr);
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
    AnimBlueprint->MarkPackageDirty();
    return BSNode->GetName();
}

FString UMCPGraphEditLibrary::AddSequencePlayerToState(
    UAnimBlueprint* AnimBlueprint,
    const FString& GraphName,
    const FString& StateMachineNodeName,
    const FString& StateName,
    const FString& SequenceAssetPath,
    bool bLoop,
    int32 PosX,
    int32 PosY)
{
    if (!AnimBlueprint) return FString();
    UEdGraph* Graph = FindGraphByName(AnimBlueprint, GraphName);
    if (!Graph) return FString();

    UAnimGraphNode_StateMachineBase* MachineNode = Cast<UAnimGraphNode_StateMachineBase>(FindNodeByName(Graph, StateMachineNodeName));
    if (!MachineNode)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddSequencePlayerToState: state machine '%s' not found"), *StateMachineNodeName);
        return FString();
    }

    UAnimStateNode* StateNode = Cast<UAnimStateNode>(FindStateByName(MachineNode, StateName));
    if (!StateNode || !StateNode->BoundGraph)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddSequencePlayerToState: state '%s' not found or has no bound graph"), *StateName);
        return FString();
    }

    UAnimSequenceBase* SequenceAsset = LoadObject<UAnimSequenceBase>(nullptr, *SequenceAssetPath);
    if (!SequenceAsset)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddSequencePlayerToState: could not load sequence '%s'"), *SequenceAssetPath);
        return FString();
    }

    // Same reason as the blend space above: the sequence has to be set before pins are allocated, which
    // is what the editor's FGraphNodeCreator ordering guarantees.
    FGraphNodeCreator<UAnimGraphNode_SequencePlayer> Creator(*StateNode->BoundGraph);
    UAnimGraphNode_SequencePlayer* SeqNode = Creator.CreateNode(/*bSelectNewNode*/ false);
    SeqNode->NodePosX = PosX;
    SeqNode->NodePosY = PosY;
    SeqNode->Node.SetSequence(SequenceAsset);
    SeqNode->Node.SetLoopAnimation(bLoop);
    Creator.Finalize();
    SeqNode->ReconstructNode();

    UEdGraphPin* PoseSink = StateNode->GetPoseSinkPinInsideState();
    UEdGraphPin* PoseOutput = FindPinByName(SeqNode, TEXT("Pose"));
    if (PoseSink && PoseOutput)
    {
        PoseOutput->MakeLinkTo(PoseSink);
    }
    else
    {
        UE_LOG(LogMCPBridge, Warning,
            TEXT("AddSequencePlayerToState: could not connect Pose pins (sink=%d output=%d) — connect manually with ConnectPins"),
            PoseSink != nullptr, PoseOutput != nullptr);
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
    AnimBlueprint->MarkPackageDirty();
    return SeqNode->GetName();
}

FString UMCPGraphEditLibrary::AddAnimGraphNode(
    UAnimBlueprint* AnimBlueprint,
    const FString& GraphName,
    const FString& NodeClassPath,
    int32 PosX,
    int32 PosY)
{
    if (!AnimBlueprint) return FString();
    UEdGraph* Graph = FindGraphByName(AnimBlueprint, GraphName);
    if (!Graph)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddAnimGraphNode: graph '%s' not found"), *GraphName);
        return FString();
    }

    // LoadClass rejects anything that isn't a UAnimGraphNode_Base, so a wrong path fails here rather
    // than by leaving a node the anim compiler can't make sense of in the graph.
    UClass* NodeClass = LoadClass<UAnimGraphNode_Base>(nullptr, *NodeClassPath);
    if (!NodeClass)
    {
        UE_LOG(LogMCPBridge, Warning,
            TEXT("AddAnimGraphNode: '%s' is not a loadable UAnimGraphNode_Base class"), *NodeClassPath);
        return FString();
    }
    if (NodeClass->HasAnyClassFlags(CLASS_Abstract))
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddAnimGraphNode: '%s' is abstract"), *NodeClassPath);
        return FString();
    }

    // Mirrors FEdGraphSchemaAction_NewNode::PerformAction, the editor's own placement path. The order
    // matters and PostPlacedNewNode is not optional: it is where an anim node finishes constructing
    // itself, and a node that skips it can end up in the graph looking correct while compiling to
    // nothing.
    UAnimGraphNode_Base* NewNode = NewObject<UAnimGraphNode_Base>(Graph, NodeClass);
    NewNode->SetFlags(RF_Transactional);
    Graph->AddNode(NewNode, false, false);
    NewNode->CreateNewGuid();
    NewNode->PostPlacedNewNode();
    NewNode->NodePosX = PosX;
    NewNode->NodePosY = PosY;
    NewNode->AllocateDefaultPins();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
    AnimBlueprint->MarkPackageDirty();
    return NewNode->GetName();
}

bool UMCPGraphEditLibrary::LinkUseCachedPose(
    UAnimBlueprint* AnimBlueprint,
    const FString& GraphName,
    const FString& UseNodeName,
    const FString& SaveNodeName)
{
    if (!AnimBlueprint) return false;
    UEdGraph* Graph = FindGraphByName(AnimBlueprint, GraphName);
    if (!Graph) return false;

    UAnimGraphNode_UseCachedPose* UseNode = Cast<UAnimGraphNode_UseCachedPose>(FindNodeByName(Graph, UseNodeName));
    UAnimGraphNode_SaveCachedPose* SaveNode = Cast<UAnimGraphNode_SaveCachedPose>(FindNodeByName(Graph, SaveNodeName));
    if (!UseNode || !SaveNode)
    {
        UE_LOG(LogMCPBridge, Warning,
            TEXT("LinkUseCachedPose: node missing or wrong type (use=%s ok=%d, save=%s ok=%d)"),
            *UseNodeName, UseNode != nullptr, *SaveNodeName, SaveNode != nullptr);
        return false;
    }

    UseNode->SaveCachedPoseNode = SaveNode;
    // The Use node names itself after its target and only grows its pose pin once it has one, so it has
    // to be rebuilt rather than just assigned to.
    UseNode->ReconstructNode();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
    AnimBlueprint->MarkPackageDirty();
    return true;
}

FString UMCPGraphEditLibrary::AddAnimSlotNode(
    UAnimBlueprint* AnimBlueprint,
    const FString& GraphName,
    const FString& SlotName,
    int32 PosX,
    int32 PosY)
{
    if (!AnimBlueprint) return FString();
    UEdGraph* Graph = FindGraphByName(AnimBlueprint, GraphName);
    if (!Graph) return FString();

    UAnimGraphNode_Slot* SlotNode = NewObject<UAnimGraphNode_Slot>(Graph);
    Graph->AddNode(SlotNode, false, false);
    SlotNode->CreateNewGuid();
    SlotNode->NodePosX = PosX;
    SlotNode->NodePosY = PosY;
    SlotNode->Node.SlotName = FName(*SlotName);
    SlotNode->AllocateDefaultPins();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
    AnimBlueprint->MarkPackageDirty();
    return SlotNode->GetName();
}

FString UMCPGraphEditLibrary::AddAnimTransition(
    UAnimBlueprint* AnimBlueprint,
    const FString& GraphName,
    const FString& StateMachineNodeName,
    const FString& FromStateName,
    const FString& ToStateName,
    const FString& ConditionPropertyName,
    bool bNegateCondition)
{
    if (!AnimBlueprint) return FString();
    UEdGraph* Graph = FindGraphByName(AnimBlueprint, GraphName);
    if (!Graph) return FString();

    UAnimGraphNode_StateMachineBase* MachineNode = Cast<UAnimGraphNode_StateMachineBase>(FindNodeByName(Graph, StateMachineNodeName));
    if (!MachineNode || !MachineNode->EditorStateMachineGraph)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddAnimTransition: state machine '%s' not found or has no inner graph"), *StateMachineNodeName);
        return FString();
    }

    UAnimationStateMachineGraph* InnerGraph = MachineNode->EditorStateMachineGraph;

    UAnimStateNodeBase* FromState = FindStateByName(MachineNode, FromStateName);
    UAnimStateNodeBase* ToState = FindStateByName(MachineNode, ToStateName);
    if (!FromState || !ToState)
    {
        UE_LOG(LogMCPBridge, Warning,
            TEXT("AddAnimTransition: from/to state not found (from=%d to=%d)"),
            FromState != nullptr, ToState != nullptr);
        return FString();
    }

    UAnimStateTransitionNode* TransNode = NewObject<UAnimStateTransitionNode>(InnerGraph);
    InnerGraph->AddNode(TransNode, false, false);
    TransNode->CreateNewGuid();
    TransNode->NodePosX = (FromState->NodePosX + ToState->NodePosX) / 2;
    TransNode->NodePosY = (FromState->NodePosY + ToState->NodePosY) / 2;
    TransNode->AllocateDefaultPins();
    TransNode->PostPlacedNewNode(); // creates BoundGraph (bool rule graph) + TransitionResult sink

    if (UEdGraphPin* FromOut = FromState->GetOutputPin())
    {
        FromOut->MakeLinkTo(TransNode->GetInputPin());
    }
    if (UEdGraphPin* ToIn = ToState->GetInputPin())
    {
        TransNode->GetOutputPin()->MakeLinkTo(ToIn);
    }

    if (!ConditionPropertyName.IsEmpty() && TransNode->BoundGraph)
    {
        UAnimGraphNode_TransitionResult* ResultNode = nullptr;
        for (UEdGraphNode* N : TransNode->BoundGraph->Nodes)
        {
            ResultNode = Cast<UAnimGraphNode_TransitionResult>(N);
            if (ResultNode) break;
        }

        UClass* InstanceClass = AnimBlueprint->ParentClass;
        FProperty* Prop = InstanceClass ? InstanceClass->FindPropertyByName(FName(*ConditionPropertyName)) : nullptr;

        if (ResultNode && Prop)
        {
            UEdGraphPin* ResultBoolPin = FindPinByName(ResultNode, TEXT("bCanEnterTransition"));

            UK2Node_VariableGet* GetterNode = NewObject<UK2Node_VariableGet>(TransNode->BoundGraph);
            TransNode->BoundGraph->AddNode(GetterNode, false, false);
            GetterNode->CreateNewGuid();
            GetterNode->VariableReference.SetSelfMember(Prop->GetFName());
            GetterNode->NodePosX = ResultNode->NodePosX - 300;
            GetterNode->NodePosY = ResultNode->NodePosY;
            GetterNode->AllocateDefaultPins();

            UEdGraphPin* GetterValuePin = FindPinByName(GetterNode, ConditionPropertyName);

            if (ResultBoolPin && GetterValuePin)
            {
                if (bNegateCondition)
                {
                    UK2Node_CallFunction* NotNode = NewObject<UK2Node_CallFunction>(TransNode->BoundGraph);
                    TransNode->BoundGraph->AddNode(NotNode, false, false);
                    NotNode->CreateNewGuid();
                    UFunction* NotFunc = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Not_PreBool"));
                    if (NotFunc)
                    {
                        NotNode->SetFromFunction(NotFunc);
                        NotNode->NodePosX = ResultNode->NodePosX - 150;
                        NotNode->NodePosY = ResultNode->NodePosY;
                        NotNode->AllocateDefaultPins();
                        UEdGraphPin* NotInput = FindPinByName(NotNode, TEXT("A"));
                        UEdGraphPin* NotOutput = FindPinByName(NotNode, TEXT("ReturnValue"));
                        if (NotInput && NotOutput)
                        {
                            GetterValuePin->MakeLinkTo(NotInput);
                            NotOutput->MakeLinkTo(ResultBoolPin);
                        }
                    }
                    else
                    {
                        UE_LOG(LogMCPBridge, Warning, TEXT("AddAnimTransition: could not find Not_PreBool function"));
                    }
                }
                else
                {
                    GetterValuePin->MakeLinkTo(ResultBoolPin);
                }
            }
            else
            {
                UE_LOG(LogMCPBridge, Warning,
                    TEXT("AddAnimTransition: could not wire condition (result_pin=%d getter_pin=%d)"),
                    ResultBoolPin != nullptr, GetterValuePin != nullptr);
            }
        }
        else
        {
            UE_LOG(LogMCPBridge, Warning,
                TEXT("AddAnimTransition: condition property '%s' not found on %s, or no TransitionResult node"),
                *ConditionPropertyName, InstanceClass ? *InstanceClass->GetName() : TEXT("<null>"));
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
    AnimBlueprint->MarkPackageDirty();
    return TransNode->GetName();
}

// ---- Blueprint authoring: structure ----

bool UMCPGraphEditLibrary::MakePinTypeFromString(const FString& TypeString, const FString& SubTypeObjectPath, bool bIsArray, FEdGraphPinType& OutPinType)
{
    OutPinType = FEdGraphPinType();
    const FString T = TypeString.TrimStartAndEnd().ToLower();

    auto LoadSub = [&SubTypeObjectPath]() -> UObject* { return SubTypeObjectPath.IsEmpty() ? nullptr : LoadObject<UObject>(nullptr, *SubTypeObjectPath); };

    if (T == TEXT("bool") || T == TEXT("boolean")) { OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean; }
    else if (T == TEXT("byte"))                    { OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte; }
    else if (T == TEXT("int") || T == TEXT("int32") || T == TEXT("integer")) { OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int; }
    else if (T == TEXT("int64"))                   { OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64; }
    else if (T == TEXT("float") || T == TEXT("double") || T == TEXT("real")) { OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real; OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Double; }
    else if (T == TEXT("string"))                  { OutPinType.PinCategory = UEdGraphSchema_K2::PC_String; }
    else if (T == TEXT("name"))                    { OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name; }
    else if (T == TEXT("text"))                    { OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text; }
    else if (T == TEXT("vector"))                  { OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct; OutPinType.PinSubCategoryObject = TBaseStructure<FVector>::Get(); }
    else if (T == TEXT("rotator"))                 { OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct; OutPinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get(); }
    else if (T == TEXT("transform"))               { OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct; OutPinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get(); }
    else if (T == TEXT("object") || T == TEXT("class"))
    {
        UClass* SubClass = Cast<UClass>(LoadSub());
        if (!SubClass) { SubClass = UObject::StaticClass(); }
        OutPinType.PinCategory = (T == TEXT("class")) ? UEdGraphSchema_K2::PC_Class : UEdGraphSchema_K2::PC_Object;
        OutPinType.PinSubCategoryObject = SubClass;
    }
    else if (T == TEXT("struct"))                  { UScriptStruct* S = Cast<UScriptStruct>(LoadSub()); if (!S) return false; OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct; OutPinType.PinSubCategoryObject = S; }
    else if (T == TEXT("enum"))                    { UEnum* E = Cast<UEnum>(LoadSub()); if (!E) return false; OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte; OutPinType.PinSubCategoryObject = E; }
    else
    {
        // Fallback: interpret TypeString itself as a struct / class / enum object path.
        if (UScriptStruct* S = LoadObject<UScriptStruct>(nullptr, *TypeString)) { OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct; OutPinType.PinSubCategoryObject = S; }
        else if (UClass* C = LoadObject<UClass>(nullptr, *TypeString))          { OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object; OutPinType.PinSubCategoryObject = C; }
        else if (UEnum* En = LoadObject<UEnum>(nullptr, *TypeString))           { OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;   OutPinType.PinSubCategoryObject = En; }
        else { return false; }
    }

    OutPinType.ContainerType = bIsArray ? EPinContainerType::Array : EPinContainerType::None;
    return true;
}

bool UMCPGraphEditLibrary::AddMemberVariable(UBlueprint* Blueprint, const FString& VarName, const FString& TypeString, const FString& SubTypeObjectPath, bool bIsArray, const FString& DefaultValue)
{
    if (!Blueprint || VarName.IsEmpty()) return false;

    FEdGraphPinType PinType;
    if (!MakePinTypeFromString(TypeString, SubTypeObjectPath, bIsArray, PinType))
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddMemberVariable: unrecognized type '%s' (subtype '%s')"), *TypeString, *SubTypeObjectPath);
        return false;
    }

    const bool bOk = FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*VarName), PinType, DefaultValue);
    if (bOk) { Blueprint->MarkPackageDirty(); }
    return bOk;
}

FString UMCPGraphEditLibrary::AddFunctionGraph(UBlueprint* Blueprint, const FString& FunctionName)
{
    if (!Blueprint || FunctionName.IsEmpty()) return FString();

    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint, FName(*FunctionName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    if (!NewGraph) return FString();

    // SignatureType=UClass, no signature source -> a fresh user function with editable entry/result nodes.
    FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewGraph, /*bIsUserCreated=*/true, static_cast<UClass*>(nullptr));
    Blueprint->MarkPackageDirty();
    return NewGraph->GetName();
}

FString UMCPGraphEditLibrary::AddComponent(UBlueprint* Blueprint, const FString& ComponentClassPath, const FString& ComponentName, const FString& ParentComponentName)
{
    if (!Blueprint || !Blueprint->SimpleConstructionScript) return FString();

    UClass* CompClass = LoadObject<UClass>(nullptr, *ComponentClassPath);
    if (!CompClass || !CompClass->IsChildOf(UActorComponent::StaticClass()))
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddComponent: '%s' is not a UActorComponent class"), *ComponentClassPath);
        return FString();
    }

    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    USCS_Node* NewNode = SCS->CreateNode(CompClass, FName(*ComponentName));
    if (!NewNode) return FString();

    USCS_Node* ParentNode = nullptr;
    if (!ParentComponentName.IsEmpty())
    {
        for (USCS_Node* N : SCS->GetAllNodes())
        {
            if (N && N->GetVariableName() == FName(*ParentComponentName)) { ParentNode = N; break; }
        }
    }

    if (ParentNode) { ParentNode->AddChildNode(NewNode); }
    else            { SCS->AddNode(NewNode); }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Blueprint->MarkPackageDirty();
    return NewNode->GetVariableName().ToString();
}

bool UMCPGraphEditLibrary::SetClassDefaultProperty(UBlueprint* Blueprint, const FString& PropertyName, const FString& ValueText)
{
    if (!Blueprint) return false;
    UClass* GenClass = Blueprint->GeneratedClass;
    if (!GenClass) return false;
    UObject* CDO = GenClass->GetDefaultObject();
    if (!CDO) return false;

    FProperty* Prop = GenClass->FindPropertyByName(FName(*PropertyName));
    if (!Prop)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("SetClassDefaultProperty: property '%s' not found on %s"), *PropertyName, *GenClass->GetName());
        return false;
    }

    void* Addr = Prop->ContainerPtrToValuePtr<void>(CDO);
    CDO->Modify();
    const TCHAR* Result = Prop->ImportText_Direct(*ValueText, Addr, CDO, PPF_None);
    if (Result == nullptr) return false;

    Blueprint->MarkPackageDirty();
    return true;
}

// ---- Blueprint authoring: graph nodes ----

FString UMCPGraphEditLibrary::AddK2Node(UBlueprint* Blueprint, const FString& GraphName, const FString& NodeClassPath, int32 PosX, int32 PosY)
{
    if (!Blueprint) return FString();
    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph) { UE_LOG(LogMCPBridge, Warning, TEXT("AddK2Node: graph '%s' not found"), *GraphName); return FString(); }

    UClass* NodeClass = LoadObject<UClass>(nullptr, *NodeClassPath);
    if (!NodeClass || !NodeClass->IsChildOf(UEdGraphNode::StaticClass()))
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddK2Node: '%s' is not a UEdGraphNode class"), *NodeClassPath);
        return FString();
    }

    UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NodeClass);
    if (!Node) return FString();
    Graph->AddNode(Node, /*bFromUI*/ false, /*bSelectNewNode*/ false);
    Node->CreateNewGuid();
    Node->NodePosX = PosX;
    Node->NodePosY = PosY;
    Node->PostPlacedNewNode();
    if (Node->Pins.Num() == 0) { Node->AllocateDefaultPins(); }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Blueprint->MarkPackageDirty();
    return Node->GetName();
}

FString UMCPGraphEditLibrary::AddVariableSetNode(UBlueprint* Blueprint, const FString& GraphName, const FString& PropertyName, int32 PosX, int32 PosY)
{
    if (!Blueprint) return FString();
    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph) return FString();

    UClass* InstanceClass = Blueprint->GeneratedClass ? Blueprint->GeneratedClass : Blueprint->ParentClass;
    FProperty* Prop = InstanceClass ? InstanceClass->FindPropertyByName(FName(*PropertyName)) : nullptr;
    if (!Prop)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddVariableSetNode: property '%s' not found"), *PropertyName);
        return FString();
    }

    UK2Node_VariableSet* Node = NewObject<UK2Node_VariableSet>(Graph);
    Graph->AddNode(Node, /*bFromUI*/ false, /*bSelectNewNode*/ false);
    Node->CreateNewGuid();
    Node->VariableReference.SetSelfMember(Prop->GetFName());
    Node->NodePosX = PosX;
    Node->NodePosY = PosY;
    Node->AllocateDefaultPins();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Blueprint->MarkPackageDirty();
    return Node->GetName();
}

bool UMCPGraphEditLibrary::SetPinDefaultValue(UBlueprint* Blueprint, const FString& GraphName, const FString& NodeName, const FString& PinName, const FString& Value)
{
    if (!Blueprint) return false;
    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph) return false;
    UEdGraphNode* Node = FindNodeByName(Graph, NodeName);
    if (!Node) return false;
    UEdGraphPin* Pin = FindPinByName(Node, PinName);
    if (!Pin) return false;

    const UEdGraphSchema* Schema = Graph->GetSchema();
    if (!Schema) return false;
    Schema->TrySetDefaultValue(*Pin, Value);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Blueprint->MarkPackageDirty();
    return true;
}

// ---- Blueprint authoring: interfaces & function signatures ----

bool UMCPGraphEditLibrary::ImplementInterface(UBlueprint* Blueprint, const FString& InterfaceClassPath)
{
    if (!Blueprint) return false;
    UClass* IfaceClass = LoadObject<UClass>(nullptr, *InterfaceClassPath);
    if (!IfaceClass)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("ImplementInterface: could not load interface class '%s'"), *InterfaceClassPath);
        return false;
    }
    const bool bOk = FBlueprintEditorUtils::ImplementNewInterface(Blueprint, IfaceClass->GetClassPathName());
    if (bOk) { Blueprint->MarkPackageDirty(); }
    return bOk;
}

FString UMCPGraphEditLibrary::AddFunctionParam(UBlueprint* Blueprint, const FString& FunctionGraphName, const FString& ParamName, const FString& TypeString, const FString& SubTypeObjectPath, bool bIsArray, bool bIsOutput)
{
    if (!Blueprint || ParamName.IsEmpty()) return FString();

    UEdGraph* Graph = FindGraphByName(Blueprint, FunctionGraphName);
    if (!Graph) { UE_LOG(LogMCPBridge, Warning, TEXT("AddFunctionParam: function graph '%s' not found"), *FunctionGraphName); return FString(); }

    FEdGraphPinType PinType;
    if (!MakePinTypeFromString(TypeString, SubTypeObjectPath, bIsArray, PinType))
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddFunctionParam: unrecognized type '%s'"), *TypeString);
        return FString();
    }

    // A function INPUT param is an OUTPUT pin on the entry node; a function OUTPUT param is an INPUT pin
    // on the result node. Locate (or, for outputs, create) the right editable node.
    UK2Node_EditablePinBase* Target = nullptr;
    if (bIsOutput)
    {
        for (UEdGraphNode* N : Graph->Nodes) { if (UK2Node_FunctionResult* R = Cast<UK2Node_FunctionResult>(N)) { Target = R; break; } }
        if (!Target)
        {
            UK2Node_FunctionEntry* Entry = nullptr;
            for (UEdGraphNode* N : Graph->Nodes) { if (UK2Node_FunctionEntry* E = Cast<UK2Node_FunctionEntry>(N)) { Entry = E; break; } }
            UK2Node_FunctionResult* NewResult = NewObject<UK2Node_FunctionResult>(Graph);
            Graph->AddNode(NewResult, /*bFromUI*/ false, /*bSelectNewNode*/ false);
            NewResult->CreateNewGuid();
            NewResult->NodePosX = Entry ? Entry->NodePosX + 400 : 400;
            NewResult->NodePosY = Entry ? Entry->NodePosY : 0;
            NewResult->PostPlacedNewNode();
            NewResult->AllocateDefaultPins();
            Target = NewResult;
        }
    }
    else
    {
        for (UEdGraphNode* N : Graph->Nodes) { if (UK2Node_FunctionEntry* E = Cast<UK2Node_FunctionEntry>(N)) { Target = E; break; } }
    }

    if (!Target) { UE_LOG(LogMCPBridge, Warning, TEXT("AddFunctionParam: no entry/result node in '%s'"), *FunctionGraphName); return FString(); }

    const EEdGraphPinDirection Dir = bIsOutput ? EGPD_Input : EGPD_Output;
    UEdGraphPin* NewPin = Target->CreateUserDefinedPin(FName(*ParamName), PinType, Dir, /*bUseUniqueName=*/true);
    if (!NewPin) { UE_LOG(LogMCPBridge, Warning, TEXT("AddFunctionParam: CreateUserDefinedPin failed for '%s'"), *ParamName); return FString(); }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Blueprint->MarkPackageDirty();
    return NewPin->PinName.ToString();
}

