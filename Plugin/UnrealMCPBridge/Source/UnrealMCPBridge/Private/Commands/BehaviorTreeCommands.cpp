#include "Commands/BehaviorTreeCommands.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "UnrealMCPBridgeModule.h"
#include "Dom/JsonObject.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "UObject/UnrealType.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"

// Editor graph: only UBehaviorTreeGraph (BEHAVIORTREEEDITOR_API) and UAIGraphNode (AIGRAPH_API) are
// DLL-exported. The concrete UBehaviorTreeGraphNode_* classes are NOT exported, so they cannot be
// referenced by C++ symbol here — they are instantiated by runtime UClass lookup and manipulated
// through the exported UAIGraphNode base instead.
#include "BehaviorTreeGraph.h"
#include "AIGraphNode.h"

namespace
{
    // Runtime paths of the (unexported) editor graph node classes.
    const TCHAR* GN_Composite = TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Composite");
    const TCHAR* GN_Task      = TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Task");
    const TCHAR* GN_Decorator = TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Decorator");
    const TCHAR* GN_Service   = TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Service");
    const TCHAR* GN_Root_Name = TEXT("BehaviorTreeGraphNode_Root");

    UBehaviorTree* LoadBT(const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        FString Path;
        if (!Params.IsValid() || !Params->TryGetStringField(TEXT("tree_path"), Path) || Path.IsEmpty())
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = TEXT("tree_path is required and must be non-empty");
            return nullptr;
        }
        UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *Path);
        if (!BT)
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("Could not load BehaviorTree: %s"), *Path);
        }
        return BT;
    }

    UBehaviorTreeGraph* GetBTGraph(UBehaviorTree* BT, MCPProtocol::FMCPError& OutError)
    {
        UBehaviorTreeGraph* Graph = BT ? Cast<UBehaviorTreeGraph>(BT->BTGraph) : nullptr;
        if (!Graph)
        {
            OutError.Code = MCPProtocol::FMCPError::InternalError;
            OutError.Message = TEXT("BehaviorTree has no editor graph (BTGraph is null)");
        }
        return Graph;
    }

    bool BTReqStr(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FString& Out, MCPProtocol::FMCPError& OutError)
    {
        if (!Params->TryGetStringField(Field, Out) || Out.IsEmpty())
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("%s is required and must be non-empty"), Field);
            return false;
        }
        return true;
    }

    int32 BTGetIntOr(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, int32 Fallback)
    {
        double Val = 0.0;
        return Params->TryGetNumberField(Field, Val) ? static_cast<int32>(Val) : Fallback;
    }

    FString InstanceClassName(const UObject* Instance)
    {
        return Instance ? Instance->GetClass()->GetName() : FString(TEXT("None"));
    }

    // Dumps a runtime node instance's editable properties as import-text strings.
    TSharedPtr<FJsonObject> DumpInstanceProps(UObject* Instance)
    {
        TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
        if (!Instance) return Props;
        for (TFieldIterator<FProperty> It(Instance->GetClass()); It; ++It)
        {
            FProperty* P = *It;
            if (!P || !P->HasAnyPropertyFlags(CPF_Edit)) continue;
            FString ValueStr;
            P->ExportText_InContainer(0, ValueStr, Instance, Instance, Instance, PPF_None);
            Props->SetStringField(P->GetName(), ValueStr);
        }
        return Props;
    }

    TSharedPtr<FJsonObject> SubNodeJson(UAIGraphNode* Sub)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Sub->GetName());
        Obj->SetStringField(TEXT("instance_class"), InstanceClassName(Sub->NodeInstance));
        Obj->SetObjectField(TEXT("properties"), DumpInstanceProps(Sub->NodeInstance));
        return Obj;
    }

    // Maps a keyword or path to a UBlackboardKeyType subclass.
    UClass* ResolveBlackboardKeyType(const FString& KeyType)
    {
        const FString T = KeyType.TrimStartAndEnd().ToLower();
        auto Native = [](const TCHAR* ClassName) -> UClass*
        {
            return LoadObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/AIModule.%s"), ClassName));
        };
        if (T == TEXT("bool"))     return Native(TEXT("BlackboardKeyType_Bool"));
        if (T == TEXT("int"))      return Native(TEXT("BlackboardKeyType_Int"));
        if (T == TEXT("float"))    return Native(TEXT("BlackboardKeyType_Float"));
        if (T == TEXT("string"))   return Native(TEXT("BlackboardKeyType_String"));
        if (T == TEXT("name"))     return Native(TEXT("BlackboardKeyType_Name"));
        if (T == TEXT("vector"))   return Native(TEXT("BlackboardKeyType_Vector"));
        if (T == TEXT("rotator"))  return Native(TEXT("BlackboardKeyType_Rotator"));
        if (T == TEXT("object"))   return Native(TEXT("BlackboardKeyType_Object"));
        if (T == TEXT("class"))    return Native(TEXT("BlackboardKeyType_Class"));
        if (T == TEXT("enum"))     return Native(TEXT("BlackboardKeyType_Enum"));
        UClass* Loaded = LoadObject<UClass>(nullptr, *KeyType);
        return (Loaded && Loaded->IsChildOf(UBlackboardKeyType::StaticClass())) ? Loaded : nullptr;
    }

    // Finds a graph node (or a decorator/service subnode) by object name.
    UAIGraphNode* FindGraphNodeByName(UEdGraph* Graph, const FString& NodeName)
    {
        for (UEdGraphNode* N : Graph->Nodes)
        {
            UAIGraphNode* AIN = Cast<UAIGraphNode>(N);
            if (!AIN) continue;
            if (AIN->GetName() == NodeName) return AIN;
            for (UAIGraphNode* Sub : AIN->SubNodes) { if (Sub && Sub->GetName() == NodeName) return Sub; }
        }
        return nullptr;
    }

    UAIGraphNode* FindRootGraphNode(UEdGraph* Graph)
    {
        for (UEdGraphNode* N : Graph->Nodes)
        {
            UAIGraphNode* AIN = Cast<UAIGraphNode>(N);
            if (AIN && AIN->GetClass()->GetName() == GN_Root_Name) { return AIN; }
        }
        return nullptr;
    }

    // Spawns an editor graph node of the (unexported) class at GraphNodeClassPath, wrapping a freshly
    // created runtime node instance of InstanceClass. Returns it as the exported UAIGraphNode base.
    UAIGraphNode* SpawnGraphNode(UEdGraph* Graph, const TCHAR* GraphNodeClassPath, UClass* InstanceClass, UBehaviorTree* BT)
    {
        UClass* GraphNodeClass = LoadObject<UClass>(nullptr, GraphNodeClassPath);
        if (!GraphNodeClass || !GraphNodeClass->IsChildOf(UAIGraphNode::StaticClass())) return nullptr;
        UAIGraphNode* Node = NewObject<UAIGraphNode>(Graph, GraphNodeClass);
        if (!Node) return nullptr;
        Node->NodeInstance = NewObject<UBTNode>(BT, InstanceClass);
        Node->UpdateNodeClassData();
        return Node;
    }
}

void FBehaviorTreeCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    // behaviortree.get_tree — walk the editor graph. Params: tree_path. Returns a flat node list (each
    // with class, instance class, decorators/services with their instance properties, child node names,
    // and the node instance's editable properties), the root node name, and the blackboard path.
    Registry.Register(TEXT("behaviortree.get_tree"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBehaviorTree* BT = LoadBT(Params, OutError); if (!BT) return nullptr;
            UBehaviorTreeGraph* Graph = GetBTGraph(BT, OutError); if (!Graph) return nullptr;

            TArray<TSharedPtr<FJsonValue>> Nodes;
            FString RootName;
            for (UEdGraphNode* EdNode : Graph->Nodes)
            {
                UAIGraphNode* Node = Cast<UAIGraphNode>(EdNode);
                if (!Node) continue;
                const bool bIsRoot = Node->GetClass()->GetName() == GN_Root_Name;
                if (bIsRoot) { RootName = Node->GetName(); }

                TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
                NodeObj->SetStringField(TEXT("name"), Node->GetName());
                NodeObj->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
                NodeObj->SetStringField(TEXT("instance_class"), InstanceClassName(Node->NodeInstance));
                NodeObj->SetNumberField(TEXT("pos_x"), Node->NodePosX);
                NodeObj->SetNumberField(TEXT("pos_y"), Node->NodePosY);
                NodeObj->SetObjectField(TEXT("properties"), DumpInstanceProps(Node->NodeInstance));

                // Classify subnodes into decorators/services by the runtime instance type.
                TArray<TSharedPtr<FJsonValue>> Decos, Svcs;
                for (UAIGraphNode* Sub : Node->SubNodes)
                {
                    if (!Sub) continue;
                    if (Cast<UBTService>(Sub->NodeInstance))        { Svcs.Add(MakeShared<FJsonValueObject>(SubNodeJson(Sub))); }
                    else                                            { Decos.Add(MakeShared<FJsonValueObject>(SubNodeJson(Sub))); }
                }
                NodeObj->SetArrayField(TEXT("decorators"), Decos);
                NodeObj->SetArrayField(TEXT("services"), Svcs);

                TArray<TSharedPtr<FJsonValue>> Children;
                if (UEdGraphPin* OutPin = Node->GetOutputPin())
                {
                    for (UEdGraphPin* Linked : OutPin->LinkedTo)
                    {
                        if (Linked && Linked->GetOwningNode())
                        {
                            Children.Add(MakeShared<FJsonValueString>(Linked->GetOwningNode()->GetName()));
                        }
                    }
                }
                NodeObj->SetArrayField(TEXT("children"), Children);
                Nodes.Add(MakeShared<FJsonValueObject>(NodeObj));
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("nodes"), Nodes);
            Result->SetStringField(TEXT("root"), RootName);
            Result->SetStringField(TEXT("blackboard"), BT->BlackboardAsset ? BT->BlackboardAsset->GetPathName() : FString());
            return Result;
        });

    // behaviortree.list_blackboard_keys — Params: tree_path.
    Registry.Register(TEXT("behaviortree.list_blackboard_keys"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBehaviorTree* BT = LoadBT(Params, OutError); if (!BT) return nullptr;
            UBlackboardData* BB = BT->BlackboardAsset;
            if (!BB)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("BehaviorTree has no Blackboard asset assigned");
                return nullptr;
            }

            TArray<TSharedPtr<FJsonValue>> Keys;
            for (const FBlackboardEntry& Entry : BB->Keys)
            {
                TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
                KeyObj->SetStringField(TEXT("name"), Entry.EntryName.ToString());
                KeyObj->SetStringField(TEXT("key_type"), Entry.KeyType ? Entry.KeyType->GetClass()->GetName() : FString(TEXT("None")));
                Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
            }
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("keys"), Keys);
            Result->SetStringField(TEXT("blackboard"), BB->GetPathName());
            return Result;
        });

    // behaviortree.add_blackboard_key — Params: tree_path, key_name, key_type
    // (bool|int|float|string|name|vector|rotator|object|class|enum, or a /Script/... path).
    Registry.Register(TEXT("behaviortree.add_blackboard_key"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBehaviorTree* BT = LoadBT(Params, OutError); if (!BT) return nullptr;
            UBlackboardData* BB = BT->BlackboardAsset;
            if (!BB)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("BehaviorTree has no Blackboard asset assigned");
                return nullptr;
            }

            FString KeyName, KeyType;
            if (!BTReqStr(Params, TEXT("key_name"), KeyName, OutError)) return nullptr;
            if (!BTReqStr(Params, TEXT("key_type"), KeyType, OutError)) return nullptr;

            UClass* KeyTypeClass = ResolveBlackboardKeyType(KeyType);
            if (!KeyTypeClass)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("unrecognized blackboard key_type '%s'"), *KeyType);
                return nullptr;
            }

            FBlackboardEntry Entry;
            Entry.EntryName = FName(*KeyName);
            Entry.KeyType = NewObject<UBlackboardKeyType>(BB, KeyTypeClass);
            BB->Modify();
            BB->Keys.Add(Entry);
            BB->MarkPackageDirty();

            auto R = MakeShared<FJsonObject>();
            R->SetBoolField(TEXT("ok"), true);
            return R;
        });

    // behaviortree.set_node_property — Params: tree_path, node_name, property, value.
    Registry.Register(TEXT("behaviortree.set_node_property"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBehaviorTree* BT = LoadBT(Params, OutError); if (!BT) return nullptr;
            UBehaviorTreeGraph* Graph = GetBTGraph(BT, OutError); if (!Graph) return nullptr;

            FString NodeName, PropName, Value;
            if (!BTReqStr(Params, TEXT("node_name"), NodeName, OutError)) return nullptr;
            if (!BTReqStr(Params, TEXT("property"), PropName, OutError)) return nullptr;
            if (!Params->TryGetStringField(TEXT("value"), Value))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams; OutError.Message = TEXT("value is required"); return nullptr;
            }

            UAIGraphNode* Node = FindGraphNodeByName(Graph, NodeName);
            if (!Node || !Node->NodeInstance)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("node '%s' not found or has no instance"), *NodeName);
                return nullptr;
            }

            UObject* Instance = Node->NodeInstance;
            FProperty* Prop = Instance->GetClass()->FindPropertyByName(FName(*PropName));
            if (!Prop)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("property '%s' not found on %s"), *PropName, *Instance->GetClass()->GetName());
                return nullptr;
            }
            void* Addr = Prop->ContainerPtrToValuePtr<void>(Instance);
            Instance->Modify();
            const TCHAR* Result = Prop->ImportText_Direct(*Value, Addr, Instance, PPF_None);
            if (Result == nullptr)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("failed to import value '%s' for %s"), *Value, *PropName);
                return nullptr;
            }
            BT->MarkPackageDirty();
            auto R = MakeShared<FJsonObject>();
            R->SetBoolField(TEXT("ok"), true);
            return R;
        });

    // behaviortree.compile — regenerate the runtime tree from the editor graph. Params: tree_path.
    Registry.Register(TEXT("behaviortree.compile"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBehaviorTree* BT = LoadBT(Params, OutError); if (!BT) return nullptr;
            UBehaviorTreeGraph* Graph = GetBTGraph(BT, OutError); if (!Graph) return nullptr;

            Graph->UpdateAsset(UBehaviorTreeGraph::KeepRebuildCounter);
            BT->MarkPackageDirty();

            auto R = MakeShared<FJsonObject>();
            R->SetBoolField(TEXT("compiled"), true);
            return R;
        });

    // behaviortree.add_node — add a composite or task node under a parent (default: root). Params:
    // tree_path, node_kind ("composite"|"task"), instance_class_path, [parent_node_name], [pos_x], [pos_y].
    // Call behaviortree.compile afterwards. Returns { node_name }.
    Registry.Register(TEXT("behaviortree.add_node"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBehaviorTree* BT = LoadBT(Params, OutError); if (!BT) return nullptr;
            UBehaviorTreeGraph* Graph = GetBTGraph(BT, OutError); if (!Graph) return nullptr;

            FString NodeKind, InstanceClassPath;
            if (!BTReqStr(Params, TEXT("node_kind"), NodeKind, OutError)) return nullptr;
            if (!BTReqStr(Params, TEXT("instance_class_path"), InstanceClassPath, OutError)) return nullptr;

            UClass* InstanceClass = LoadObject<UClass>(nullptr, *InstanceClassPath);
            const bool bComposite = NodeKind.ToLower() == TEXT("composite");
            UClass* RequiredBase = bComposite ? UBTCompositeNode::StaticClass() : UBTTaskNode::StaticClass();
            if (!InstanceClass || !InstanceClass->IsChildOf(RequiredBase))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("'%s' is not a %s subclass"), *InstanceClassPath, bComposite ? TEXT("UBTCompositeNode") : TEXT("UBTTaskNode"));
                return nullptr;
            }

            UAIGraphNode* GraphNode = SpawnGraphNode(Graph, bComposite ? GN_Composite : GN_Task, InstanceClass, BT);
            if (!GraphNode)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("failed to spawn BT graph node (editor class unavailable?)");
                return nullptr;
            }
            Graph->AddNode(GraphNode, /*bFromUI*/ false, /*bSelectNewNode*/ false);
            GraphNode->CreateNewGuid();
            GraphNode->NodePosX = BTGetIntOr(Params, TEXT("pos_x"), 0);
            GraphNode->NodePosY = BTGetIntOr(Params, TEXT("pos_y"), 0);
            GraphNode->AllocateDefaultPins();

            // Wire parent's output pin -> this node's input pin (parent defaults to the root node).
            FString ParentName;
            Params->TryGetStringField(TEXT("parent_node_name"), ParentName);
            UAIGraphNode* Parent = ParentName.IsEmpty() ? FindRootGraphNode(Graph) : FindGraphNodeByName(Graph, ParentName);
            if (Parent)
            {
                UEdGraphPin* OutPin = Parent->GetOutputPin();
                UEdGraphPin* InPin = GraphNode->GetInputPin();
                if (OutPin && InPin)
                {
                    if (const UEdGraphSchema* Schema = Graph->GetSchema()) { Schema->TryCreateConnection(OutPin, InPin); }
                    else { OutPin->MakeLinkTo(InPin); }
                }
            }

            BT->MarkPackageDirty();
            auto R = MakeShared<FJsonObject>();
            R->SetStringField(TEXT("node_name"), GraphNode->GetName());
            return R;
        });

    // behaviortree.add_subnode — attach a decorator or service to a node. Params: tree_path,
    // parent_node_name, subnode_kind ("decorator"|"service"), instance_class_path. Returns { node_name }.
    Registry.Register(TEXT("behaviortree.add_subnode"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBehaviorTree* BT = LoadBT(Params, OutError); if (!BT) return nullptr;
            UBehaviorTreeGraph* Graph = GetBTGraph(BT, OutError); if (!Graph) return nullptr;

            FString ParentName, SubKind, InstanceClassPath;
            if (!BTReqStr(Params, TEXT("parent_node_name"), ParentName, OutError)) return nullptr;
            if (!BTReqStr(Params, TEXT("subnode_kind"), SubKind, OutError)) return nullptr;
            if (!BTReqStr(Params, TEXT("instance_class_path"), InstanceClassPath, OutError)) return nullptr;

            UAIGraphNode* Parent = FindGraphNodeByName(Graph, ParentName);
            if (!Parent)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("parent node '%s' not found"), *ParentName);
                return nullptr;
            }

            const bool bDecorator = SubKind.ToLower() == TEXT("decorator");
            UClass* InstanceClass = LoadObject<UClass>(nullptr, *InstanceClassPath);
            UClass* RequiredBase = bDecorator ? UBTDecorator::StaticClass() : UBTService::StaticClass();
            if (!InstanceClass || !InstanceClass->IsChildOf(RequiredBase))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("'%s' is not a %s subclass"), *InstanceClassPath, bDecorator ? TEXT("UBTDecorator") : TEXT("UBTService"));
                return nullptr;
            }

            UAIGraphNode* SubNode = SpawnGraphNode(Graph, bDecorator ? GN_Decorator : GN_Service, InstanceClass, BT);
            if (!SubNode)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("failed to spawn BT subnode (editor class unavailable?)");
                return nullptr;
            }
            SubNode->CreateNewGuid();
            Parent->AddSubNode(SubNode, Graph);

            BT->MarkPackageDirty();
            auto R = MakeShared<FJsonObject>();
            R->SetStringField(TEXT("node_name"), SubNode->GetName());
            return R;
        });
}
