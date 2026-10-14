#include "MCPGraphEditLibrary.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_Select.h"

#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet/KismetMathLibrary.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

#include "Core/CameraRigAsset.h"
#include "Core/CameraNode.h"
#include "Core/CameraObjectInterface.h"
#include "Core/CameraVariableTableFwd.h"
#include "UObject/UnrealType.h"
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

    // A full object path wins over the name search. Nested graph names are not unique — an
    // AnimBlueprint can hold several `AnimationBlendStackGraph_0`, one per Blend Stack / Motion
    // Matching node — so a caller that needs a specific one has no way to say which by name alone.
    // Paths are checked to actually live under BP: resolving to another asset's graph and editing it
    // would be a silent cross-asset write.
    if (GraphName.Contains(TEXT(":")) || GraphName.Contains(TEXT("/")))
    {
        if (UEdGraph* ByPath = FindObject<UEdGraph>(nullptr, *GraphName))
        {
            for (UObject* Outer = ByPath->GetOuter(); Outer; Outer = Outer->GetOuter())
            {
                if (Outer == BP) return ByPath;
            }
            UE_LOG(LogMCPBridge, Warning,
                TEXT("FindGraphByName: '%s' resolved but is not owned by blueprint '%s'"),
                *GraphName, *BP->GetName());
            return nullptr;
        }
    }

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

    // Interface function implementations (functions with return/out params) live in
    // ImplementedInterfaces[].Graphs, not FunctionGraphs — search those too.
    for (const FBPInterfaceDescription& Iface : BP->ImplementedInterfaces)
    {
        for (const TObjectPtr<UEdGraph>& G : Iface.Graphs)
        {
            if (G && G->GetName() == GraphName) return G;
        }
    }

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

    // Still nothing — walk every graph a node says it owns. This is what reaches the Blend Stack
    // graph inside a Motion Matching / Blend Stack node (`AnimationBlendStackGraph_*`, where GASP
    // keeps Orientation Warping and Steering), and it picks up any future node type that nests a
    // graph without needing a new special case here.
    //
    // The explicit state machine walk above stays: it is the path that is exercised today, and this
    // one only runs when that came up empty.
    {
        TArray<UEdGraph*> Queue;
        TSet<const UEdGraph*> Visited;

        auto Enqueue = [&Queue, &Visited](UEdGraph* G)
        {
            // Visited is what stops a node that reports its own graph (or a sibling that reports
            // back) from queueing forever.
            if (G && !Visited.Contains(G))
            {
                Visited.Add(G);
                Queue.Add(G);
            }
        };

        for (UEdGraph* G : BP->UbergraphPages) { Enqueue(G); }
        for (UEdGraph* G : BP->FunctionGraphs) { Enqueue(G); }
        for (UEdGraph* G : BP->MacroGraphs)    { Enqueue(G); }
        for (const FBPInterfaceDescription& Iface : BP->ImplementedInterfaces)
        {
            for (const TObjectPtr<UEdGraph>& G : Iface.Graphs) { Enqueue(G); }
        }

        // Index-based loop: Enqueue appends while we iterate, which is the breadth-first descent.
        for (int32 Index = 0; Index < Queue.Num(); ++Index)
        {
            UEdGraph* Graph = Queue[Index];
            if (Graph->GetName() == GraphName) return Graph;

            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node) continue;
                for (UEdGraph* Sub : Node->GetSubGraphs()) { Enqueue(Sub); }
            }
        }
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

bool UMCPGraphEditLibrary::ImplementInterface(UBlueprint* Blueprint, const FString& InterfaceClassPath)
{
    if (!Blueprint)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("ImplementInterface: Blueprint is null"));
        return false;
    }

    UClass* IfaceClass = LoadObject<UClass>(nullptr, *InterfaceClassPath);
    if (!IfaceClass)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("ImplementInterface: interface class '%s' not found"), *InterfaceClassPath);
        return false;
    }

    // Same call the editor's "Class Settings > Interfaces > Add" makes; creates function graphs
    // (Entry + Result) for any interface functions that have outputs.
    const bool bAdded = FBlueprintEditorUtils::ImplementNewInterface(Blueprint, IfaceClass->GetClassPathName());
    if (bAdded)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        Blueprint->MarkPackageDirty();
    }
    return bAdded;
}

FString UMCPGraphEditLibrary::AddMakeStructNode(
    UBlueprint* Blueprint,
    const FString& GraphName,
    const FString& StructPath,
    int32 PosX,
    int32 PosY)
{
    if (!Blueprint)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddMakeStructNode: Blueprint is null"));
        return FString();
    }

    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddMakeStructNode: graph '%s' not found"), *GraphName);
        return FString();
    }

    UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *StructPath);
    if (!Struct)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddMakeStructNode: struct '%s' not found"), *StructPath);
        return FString();
    }

    UK2Node_MakeStruct* Node = NewObject<UK2Node_MakeStruct>(Graph);
    Graph->AddNode(Node, /*bFromUI*/ false, /*bSelectNewNode*/ false);
    Node->CreateNewGuid();
    Node->StructType = Struct;
    Node->bMadeAfterOverridePinRemoval = true;   // new node: don't emit the legacy override-pin warning
    Node->NodePosX = PosX;
    Node->NodePosY = PosY;
    Node->AllocateDefaultPins();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Blueprint->MarkPackageDirty();
    return Node->GetName();
}

FString UMCPGraphEditLibrary::AddSelectNode(
    UBlueprint* Blueprint,
    const FString& GraphName,
    int32 PosX,
    int32 PosY)
{
    if (!Blueprint)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddSelectNode: Blueprint is null"));
        return FString();
    }

    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("AddSelectNode: graph '%s' not found"), *GraphName);
        return FString();
    }

    UK2Node_Select* Node = NewObject<UK2Node_Select>(Graph);
    Graph->AddNode(Node, /*bFromUI*/ false, /*bSelectNewNode*/ false);
    Node->CreateNewGuid();
    Node->NodePosX = PosX;
    Node->NodePosY = PosY;
    Node->AllocateDefaultPins();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Blueprint->MarkPackageDirty();
    return Node->GetName();
}

bool UMCPGraphEditLibrary::SetPinDefaultValue(
    UBlueprint* Blueprint,
    const FString& GraphName,
    const FString& NodeName,
    const FString& PinName,
    const FString& Value)
{
    if (!Blueprint) return false;
    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("SetPinDefaultValue: graph '%s' not found"), *GraphName);
        return false;
    }

    UEdGraphNode* Node = FindNodeByName(Graph, NodeName);
    if (!Node)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("SetPinDefaultValue: node '%s' not found"), *NodeName);
        return false;
    }

    UEdGraphPin* Pin = FindPinByName(Node, PinName);
    if (!Pin)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("SetPinDefaultValue: pin '%s.%s' not found"), *NodeName, *PinName);
        return false;
    }

    // Through the schema (not a raw Pin->DefaultValue write) so enum/struct/object literals are parsed
    // and validated and the owning node is notified — what typing a value into the pin in-editor does.
    const UEdGraphSchema* Schema = Graph->GetSchema();
    if (!Schema)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("SetPinDefaultValue: graph '%s' has no schema"), *GraphName);
        return false;
    }

    Schema->TrySetDefaultValue(*Pin, Value);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Blueprint->MarkPackageDirty();
    return true;
}

bool UMCPGraphEditLibrary::SetNodeObjectProperty(
    UBlueprint* Blueprint,
    const FString& GraphName,
    const FString& NodeName,
    const FString& PropertyName,
    const FString& ObjectPath)
{
    if (!Blueprint) return false;
    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("SetNodeObjectProperty: graph '%s' not found"), *GraphName);
        return false;
    }

    UEdGraphNode* Node = FindNodeByName(Graph, NodeName);
    if (!Node)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("SetNodeObjectProperty: node '%s' not found"), *NodeName);
        return false;
    }

    // The property is often protected / non-Edit, so go straight through reflection rather than the
    // scripting layer that set_editor_property uses.
    FObjectPropertyBase* ObjProp = FindFProperty<FObjectPropertyBase>(Node->GetClass(), *PropertyName);
    if (!ObjProp)
    {
        UE_LOG(LogMCPBridge, Warning,
            TEXT("SetNodeObjectProperty: object property '%s' not found on node '%s' (%s)"),
            *PropertyName, *NodeName, *Node->GetClass()->GetName());
        return false;
    }

    UObject* Value = nullptr;
    if (!ObjectPath.IsEmpty())
    {
        Value = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
        if (!Value)
        {
            UE_LOG(LogMCPBridge, Warning, TEXT("SetNodeObjectProperty: failed to load object '%s'"), *ObjectPath);
            return false;
        }
        if (ObjProp->PropertyClass && !Value->IsA(ObjProp->PropertyClass))
        {
            UE_LOG(LogMCPBridge, Warning,
                TEXT("SetNodeObjectProperty: '%s' is a %s, not the expected %s"),
                *ObjectPath, *Value->GetClass()->GetName(), *ObjProp->PropertyClass->GetName());
            return false;
        }
    }

    Node->Modify();
    ObjProp->SetObjectPropertyValue_InContainer(Node, Value);
    // Refresh title/pins the way the editor does after such an edit; connections restore by pin name.
    Node->ReconstructNode();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Blueprint->MarkPackageDirty();
    return true;
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

namespace
{
    // Shared reflection walk for the two Pose Search Database entry helpers. Everything is looked up
    // by name so this compiles without the PoseSearch module: the array is private and the element
    // struct's fields are editor-only, which is exactly what FProperty access is for.
    // Visit(EntryPtr, AnimAssetProp, EnabledProp) is called once per entry; either FProperty may be
    // null if the struct layout ever changes, so callers must check.
    bool ForEachPoseSearchDatabaseEntry(
        UObject* Database,
        const TFunctionRef<void(void*, FObjectPropertyBase*, FBoolProperty*)>& Visit)
    {
        if (!Database) return false;

        FArrayProperty* ArrayProp =
            FindFProperty<FArrayProperty>(Database->GetClass(), TEXT("DatabaseAnimationAssets"));
        if (!ArrayProp) return false;

        FStructProperty* ElemProp = CastField<FStructProperty>(ArrayProp->Inner);
        if (!ElemProp || !ElemProp->Struct) return false;

        // AnimAsset is TObjectPtr<UObject>, so the property is FObjectProperty — take the base class
        // to stay tolerant of it being declared as a soft/typed object property instead.
        FObjectPropertyBase* AssetProp =
            FindFProperty<FObjectPropertyBase>(ElemProp->Struct, TEXT("AnimAsset"));
        FBoolProperty* EnabledProp = FindFProperty<FBoolProperty>(ElemProp->Struct, TEXT("bEnabled"));

        FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(Database));
        for (int32 i = 0; i < Helper.Num(); ++i)
        {
            Visit(Helper.GetRawPtr(i), AssetProp, EnabledProp);
        }
        return true;
    }
}

int32 UMCPGraphEditLibrary::SetPoseSearchDatabaseEntriesEnabled(
    UObject* Database,
    const TArray<FString>& NameSubstrings,
    bool bEnabled)
{
    if (!Database)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("SetPoseSearchDatabaseEntriesEnabled: null database"));
        return -1;
    }

    int32 Changed = 0;
    const bool bWalked = ForEachPoseSearchDatabaseEntry(Database,
        [&](void* Entry, FObjectPropertyBase* AssetProp, FBoolProperty* EnabledProp)
        {
            if (!AssetProp || !EnabledProp) return;

            const UObject* Asset = AssetProp->GetObjectPropertyValue_InContainer(Entry);
            if (!Asset) return;

            const FString AssetName = Asset->GetName();
            const bool bMatches = NameSubstrings.ContainsByPredicate(
                [&AssetName](const FString& Needle) { return AssetName.Contains(Needle); });
            if (!bMatches) return;

            // Only count entries this actually changes, so a caller can tell "matched nothing" from
            // "matched, already in that state".
            if (EnabledProp->GetPropertyValue_InContainer(Entry) == bEnabled) return;

            Database->Modify();
            EnabledProp->SetPropertyValue_InContainer(Entry, bEnabled);
            ++Changed;
        });

    if (!bWalked)
    {
        UE_LOG(LogMCPBridge, Warning,
            TEXT("SetPoseSearchDatabaseEntriesEnabled: '%s' has no reachable DatabaseAnimationAssets"),
            *Database->GetName());
        return -1;
    }

    if (Changed > 0)
    {
        // The database caches a built search index; without a PostEditChange it keeps serving the old
        // one and the disabled clips stay selectable until something else re-cooks the asset.
        Database->PostEditChange();
        Database->MarkPackageDirty();
    }
    return Changed;
}

TArray<FString> UMCPGraphEditLibrary::ListPoseSearchDatabaseEntries(UObject* Database)
{
    TArray<FString> Out;
    ForEachPoseSearchDatabaseEntry(Database,
        [&](void* Entry, FObjectPropertyBase* AssetProp, FBoolProperty* EnabledProp)
        {
            const UObject* Asset = AssetProp ? AssetProp->GetObjectPropertyValue_InContainer(Entry) : nullptr;
            const bool bOn = EnabledProp ? EnabledProp->GetPropertyValue_InContainer(Entry) : true;
            Out.Add(FString::Printf(TEXT("%d %s"), bOn ? 1 : 0, Asset ? *Asset->GetName() : TEXT("<none>")));
        });
    return Out;
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
    }, EGetObjectsFlags::IncludeNestedObjects);

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
    const bool bIsFirstState = InnerGraph->EntryNode
        && InnerGraph->EntryNode->GetOutputPin()
        && InnerGraph->EntryNode->GetOutputPin()->LinkedTo.Num() == 0;

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

    if (bIsFirstState && InnerGraph->EntryNode && InnerGraph->EntryNode->GetOutputPin())
    {
        InnerGraph->EntryNode->GetOutputPin()->MakeLinkTo(StateNode->GetInputPin());
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

bool UMCPGraphEditLibrary::RebuildBlendSpace(UBlendSpace* BlendSpace)
{
    if (!BlendSpace)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("RebuildBlendSpace: BlendSpace is null"));
        return false;
    }

    // ResampleData validates the samples against the current axis ranges and then rebuilds the
    // interpolation data (triangulation, or the grid when bInterpolateUsingGrid). Set the axes
    // *before* the samples when authoring, or out-of-range samples are marked invalid here.
    BlendSpace->ResampleData();
    BlendSpace->MarkPackageDirty();
    return true;
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

namespace
{
    // The Nth UCameraNode of the given short class name under the rig, or null.
    UCameraNode* FindCameraNodeByClass(UCameraRigAsset* Rig, const FString& NodeClass, int32 NodeIndex)
    {
        TArray<UCameraNode*> Nodes;
        ForEachObjectWithOuter(Rig, [&Nodes](UObject* Obj)
        {
            if (UCameraNode* Node = Cast<UCameraNode>(Obj))
            {
                Nodes.Add(Node);
            }
        }, EGetObjectsFlags::IncludeNestedObjects);

        int32 MatchIndex = 0;
        for (UCameraNode* Node : Nodes)
        {
            // Tolerate both "BoomArmCameraNode" and "UBoomArmCameraNode".
            const FString ClassName = Node->GetClass()->GetName();
            const bool bMatches = ClassName.Equals(NodeClass, ESearchCase::IgnoreCase)
                || (TEXT("U") + ClassName).Equals(NodeClass, ESearchCase::IgnoreCase);
            if (bMatches)
            {
                if (MatchIndex == NodeIndex) { return Node; }
                ++MatchIndex;
            }
        }
        return nullptr;
    }

    // An exposable camera node property is a struct named F<Type>CameraParameter, where <Type> is
    // an ECameraVariableType entry (FVector3dCameraParameter -> Vector3d, FFloatCameraParameter ->
    // Float). Deriving the type from the property is what stops a caller from pairing a parameter
    // with the wrong type and getting one that builds but never resolves.
    bool InferCameraVariableType(const FProperty* Prop, ECameraVariableType& OutType, FString& OutStructName)
    {
        const FStructProperty* StructProp = CastField<FStructProperty>(Prop);
        if (!StructProp || !StructProp->Struct)
        {
            return false;
        }

        OutStructName = StructProp->Struct->GetName();
        if (!OutStructName.EndsWith(TEXT("CameraParameter")))
        {
            return false;
        }

        const FString TypeName = OutStructName.LeftChop(FString(TEXT("CameraParameter")).Len());
        const UEnum* Enum = StaticEnum<ECameraVariableType>();
        const int64 Value = Enum ? Enum->GetValueByNameString(TypeName) : INDEX_NONE;
        if (Value == INDEX_NONE)
        {
            return false;
        }

        OutType = static_cast<ECameraVariableType>(Value);
        return true;
    }
}

FString UMCPGraphEditLibrary::ExposeCameraRigParameter(
    UCameraRigAsset* Rig,
    const FString& ParameterName,
    const FString& NodeClass,
    const FString& PropertyName,
    int32 NodeIndex)
{
    if (!Rig)
    {
        return TEXT("Rig is null");
    }
    if (ParameterName.IsEmpty())
    {
        return TEXT("ParameterName must not be empty");
    }

    if (Rig->Interface.FindBlendableParameterByName(ParameterName))
    {
        return FString::Printf(TEXT("Rig already exposes a blendable parameter named '%s'"), *ParameterName);
    }

    UCameraNode* Target = FindCameraNodeByClass(Rig, NodeClass, NodeIndex);
    if (!Target)
    {
        return FString::Printf(TEXT("No node of class '%s' (index %d) in rig"), *NodeClass, NodeIndex);
    }

    const FProperty* Prop = FindFProperty<FProperty>(Target->GetClass(), *PropertyName);
    if (!Prop)
    {
        return FString::Printf(TEXT("Property '%s' not found on %s"), *PropertyName, *Target->GetClass()->GetName());
    }

    ECameraVariableType VariableType;
    FString StructName;
    if (!InferCameraVariableType(Prop, VariableType, StructName))
    {
        return FString::Printf(
            TEXT("Property '%s' on %s is '%s', not an F<Type>CameraParameter struct — only camera parameters can be exposed"),
            *PropertyName, *Target->GetClass()->GetName(),
            StructName.IsEmpty() ? *Prop->GetCPPType() : *StructName);
    }

    // Refuse to double-drive one property: two getters on the same target would leave which
    // parameter wins up to connection order.
    if (Rig->Connections.FindByTarget(Target, *PropertyName))
    {
        return FString::Printf(TEXT("%s.%s is already driven by an existing connection"),
            *Target->GetName(), *PropertyName);
    }

    Rig->Modify();

    // Mirrors FCameraRigAssembler::AddBlendableParameter (GameplayCameras
    // Build/CameraAssetAssembleUtils.h): the parameter object alone is inert — what binds it to the
    // node is a getter in the rig's connection list, keyed by the parameter's Guid.
    UCameraObjectInterfaceBlendableParameter* Parameter =
        NewObject<UCameraObjectInterfaceBlendableParameter>(Rig, NAME_None, RF_Transactional);
    Parameter->InterfaceParameterName = ParameterName;
    Parameter->VariableType = VariableType;
    Rig->Interface.BlendableParameters.Add(Parameter);

    if (!Parameter->GetGuid().IsValid())
    {
        return TEXT("New interface parameter has an invalid Guid");
    }

    UCameraObjectInterfaceParameterGetter* Getter =
        NewObject<UCameraObjectInterfaceParameterGetter>(Rig, NAME_None, RF_Transactional);
    Getter->ParameterGuid = Parameter->GetGuid();
    Rig->Connections.Add(Getter, NAME_None, Target, *PropertyName);

    // Build here rather than leaving it to the caller: the parameter definitions the runtime looks
    // up (GetParameterDefinitions) are produced by the build, so an unbuilt rig still reports
    // "No such camera parameter".
    Rig->DirtyBuildStatus();
    Rig->BuildCameraRig();
    Rig->MarkPackageDirty();

    return FString();
}

TArray<FString> UMCPGraphEditLibrary::ListCameraRigParameters(UCameraRigAsset* Rig)
{
    TArray<FString> Out;
    if (!Rig)
    {
        return Out;
    }

    const UEnum* TypeEnum = StaticEnum<ECameraVariableType>();
    for (const UCameraObjectInterfaceBlendableParameter* Parameter : Rig->Interface.BlendableParameters)
    {
        if (!Parameter) continue;

        const FString TypeName = TypeEnum
            ? TypeEnum->GetNameStringByValue(static_cast<int64>(Parameter->VariableType))
            : TEXT("?");

        // A parameter with no getter connection is exposed but wired to nothing — worth seeing.
        FString Bound = TEXT("(unbound)");
        for (const FCameraObjectConnection& Connection : Rig->Connections.Connections)
        {
            const UCameraObjectInterfaceParameterGetter* Getter =
                Cast<UCameraObjectInterfaceParameterGetter>(Connection.Source);
            if (Getter && Getter->ParameterGuid == Parameter->GetGuid())
            {
                Bound = FString::Printf(TEXT("%s.%s"),
                    Connection.Target ? *Connection.Target->GetName() : TEXT("None"),
                    *Connection.TargetPropertyName.ToString());
                break;
            }
        }

        Out.Add(FString::Printf(TEXT("%s|%s|%s"), *Parameter->InterfaceParameterName, *TypeName, *Bound));
    }
    return Out;
}

