#include "Commands/StateTreeCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "StateTree.h"
#include "StateTreeState.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeTypes.h"
#include "PropertyBindingPath.h"
#include "Commands/InspectorInvoke.h"

namespace
{
    using UnrealMCPBridge::Json::RequireString;

    UStateTree* LoadTree(const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        FString Path;
        if (!Params.IsValid() || !Params->TryGetStringField(TEXT("tree_path"), Path) || Path.IsEmpty())
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = TEXT("tree_path is required and must be non-empty");
            return nullptr;
        }
        UStateTree* Tree = LoadObject<UStateTree>(nullptr, *Path);
        if (!Tree)
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("Could not load StateTree: %s"), *Path);
        }
        return Tree;
    }

    UStateTreeEditorData* GetEditorData(UStateTree* Tree, MCPProtocol::FMCPError& OutError)
    {
        UStateTreeEditorData* Ed = Tree ? Cast<UStateTreeEditorData>(Tree->EditorData) : nullptr;
        if (!Ed)
        {
            OutError.Code = MCPProtocol::FMCPError::InternalError;
            OutError.Message = TEXT("StateTree has no editor data (was it created with a schema?)");
        }
        return Ed;
    }

    // Resolves a struct path like "/Script/IFSR.STEval_SenseTarget" to a UScriptStruct.
    const UScriptStruct* FindNodeStruct(const FString& Path)
    {
        return LoadObject<UScriptStruct>(nullptr, *Path);
    }

    // Finds a state by its GUID id (string). Returns nullptr if not found.
    UStateTreeState* FindStateById(UStateTreeEditorData* Ed, const FGuid& Id)
    {
        UStateTreeState* Found = nullptr;
        Ed->VisitHierarchy([&Found, &Id](UStateTreeState& State, UStateTreeState* /*Parent*/)
        {
            if (State.ID == Id)
            {
                Found = &State;
                return EStateTreeVisitor::Break;
            }
            return EStateTreeVisitor::Continue;
        });
        return Found;
    }

    // Finds the first root subtree state (parent == nullptr).
    UStateTreeState* FindRootState(UStateTreeEditorData* Ed)
    {
        UStateTreeState* Root = nullptr;
        Ed->VisitHierarchy([&Root](UStateTreeState& State, UStateTreeState* Parent)
        {
            if (Parent == nullptr)
            {
                Root = &State;
                return EStateTreeVisitor::Break;
            }
            return EStateTreeVisitor::Continue;
        });
        return Root;
    }

    EStateTreeTransitionTrigger ParseTrigger(const FString& Str)
    {
        if (Str.Equals(TEXT("OnStateCompleted"), ESearchCase::IgnoreCase)) return EStateTreeTransitionTrigger::OnStateCompleted;
        if (Str.Equals(TEXT("OnStateSucceeded"), ESearchCase::IgnoreCase)) return EStateTreeTransitionTrigger::OnStateSucceeded;
        if (Str.Equals(TEXT("OnEvent"), ESearchCase::IgnoreCase))          return EStateTreeTransitionTrigger::OnEvent;
        return EStateTreeTransitionTrigger::OnTick; // default: evaluate every tick
    }

    // Adds an editor node (task/eval) of the given struct into an array, returns its new GUID (or invalid).
    FGuid AddNodeToArray(TArray<FStateTreeEditorNode>& Array, UObject* Outer, const UScriptStruct* Struct)
    {
        FStateTreeEditorNode& Node = Array.AddDefaulted_GetRef();
        Node.InitializeAs(Outer, Struct);
        Node.ID = FGuid::NewGuid();
        return Node.ID;
    }

    // Finds a transition by its GUID across every state. Returns nullptr if not found.
    FStateTreeTransition* FindTransitionById(UStateTreeEditorData* Ed, const FGuid& Id)
    {
        FStateTreeTransition* Found = nullptr;
        Ed->VisitHierarchy([&Found, &Id](UStateTreeState& State, UStateTreeState* /*Parent*/)
        {
            for (FStateTreeTransition& T : State.Transitions)
            {
                if (T.ID == Id) { Found = &T; return EStateTreeVisitor::Break; }
            }
            return EStateTreeVisitor::Continue;
        });
        return Found;
    }

    // Finds an editor node (evaluator / global task / state task / single task / enter- or
    // transition-condition) by its GUID. Returns nullptr if not found.
    FStateTreeEditorNode* FindNodeById(UStateTreeEditorData* Ed, const FGuid& Id)
    {
        for (FStateTreeEditorNode& N : Ed->Evaluators)  { if (N.ID == Id) return &N; }
        for (FStateTreeEditorNode& N : Ed->GlobalTasks) { if (N.ID == Id) return &N; }
        FStateTreeEditorNode* Found = nullptr;
        Ed->VisitHierarchy([&Found, &Id](UStateTreeState& State, UStateTreeState* /*Parent*/)
        {
            for (FStateTreeEditorNode& N : State.Tasks)           { if (N.ID == Id) { Found = &N; return EStateTreeVisitor::Break; } }
            for (FStateTreeEditorNode& N : State.EnterConditions) { if (N.ID == Id) { Found = &N; return EStateTreeVisitor::Break; } }
            if (State.SingleTask.ID == Id) { Found = &State.SingleTask; return EStateTreeVisitor::Break; }
            for (FStateTreeTransition& T : State.Transitions)
            {
                for (FStateTreeEditorNode& N : T.Conditions) { if (N.ID == Id) { Found = &N; return EStateTreeVisitor::Break; } }
            }
            return EStateTreeVisitor::Continue;
        });
        return Found;
    }
}

void FStateTreeCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    // ---- BEGIN commands forwarded to BlueprintInspector (generated from its function headers) ----
    Registry.Register(TEXT("statetree.list_binding_sources"), MakeInspectorCommand(TEXT("/Script/UnrealMCPBridge.MCPInspectorStateTreeToolset"), TEXT("ListStateTreeBindingSources"), { TEXT("tree_path"), TEXT("target_node_id") }));
    Registry.Register(TEXT("statetree.unbind_property"), MakeInspectorCommand(TEXT("/Script/UnrealMCPBridge.MCPInspectorStateTreeToolset"), TEXT("UnbindStateTreeProperty"), { TEXT("tree_path"), TEXT("target_node_id"), TEXT("target_property_path") }));
    Registry.Register(TEXT("statetree.add_parameter"), MakeInspectorCommand(TEXT("/Script/UnrealMCPBridge.MCPInspectorStateTreeToolset"), TEXT("AddStateTreeParameter"), { TEXT("tree_path"), TEXT("name"), TEXT("type") }));
    // ---- END forwarded commands ----

    // statetree.add_state — adds a state. Params: tree_path, state_name, parent_state_id (optional; empty=root).
    // Returns { state_id }.
    Registry.Register(TEXT("statetree.add_state"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UStateTree* Tree = LoadTree(Params, OutError); if (!Tree) return nullptr;
            UStateTreeEditorData* Ed = GetEditorData(Tree, OutError); if (!Ed) return nullptr;

            FString StateName; if (!RequireString(Params, TEXT("state_name"), StateName, OutError)) return nullptr;
            FString ParentId; Params->TryGetStringField(TEXT("parent_state_id"), ParentId);

            UStateTreeState* Parent = nullptr;
            if (!ParentId.IsEmpty())
            {
                FGuid Guid; FGuid::Parse(ParentId, Guid);
                Parent = FindStateById(Ed, Guid);
                if (!Parent) { OutError.Code = MCPProtocol::FMCPError::InvalidParams; OutError.Message = TEXT("parent_state_id not found"); return nullptr; }
            }
            else
            {
                Parent = FindRootState(Ed);
                if (!Parent) { Parent = &Ed->AddRootState(); }
            }

            UStateTreeState& NewState = Parent->AddChildState(FName(*StateName));
            Tree->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("state_id"), NewState.ID.ToString());
            return Result;
        });

    // statetree.add_evaluator — Params: tree_path, struct_path. Returns { node_id }.
    Registry.Register(TEXT("statetree.add_evaluator"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UStateTree* Tree = LoadTree(Params, OutError); if (!Tree) return nullptr;
            UStateTreeEditorData* Ed = GetEditorData(Tree, OutError); if (!Ed) return nullptr;

            FString StructPath; if (!RequireString(Params, TEXT("struct_path"), StructPath, OutError)) return nullptr;
            const UScriptStruct* Struct = FindNodeStruct(StructPath);
            if (!Struct) { OutError.Code = MCPProtocol::FMCPError::InvalidParams; OutError.Message = FString::Printf(TEXT("struct not found: %s"), *StructPath); return nullptr; }

            const FGuid Id = AddNodeToArray(Ed->Evaluators, Ed, Struct);
            Tree->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("node_id"), Id.ToString());
            return Result;
        });

    // statetree.add_task — Params: tree_path, state_id, struct_path. Returns { node_id }.
    Registry.Register(TEXT("statetree.add_task"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UStateTree* Tree = LoadTree(Params, OutError); if (!Tree) return nullptr;
            UStateTreeEditorData* Ed = GetEditorData(Tree, OutError); if (!Ed) return nullptr;

            FString StateId, StructPath;
            if (!RequireString(Params, TEXT("state_id"), StateId, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("struct_path"), StructPath, OutError)) return nullptr;

            FGuid Guid; FGuid::Parse(StateId, Guid);
            UStateTreeState* State = FindStateById(Ed, Guid);
            if (!State) { OutError.Code = MCPProtocol::FMCPError::InvalidParams; OutError.Message = TEXT("state_id not found"); return nullptr; }

            const UScriptStruct* Struct = FindNodeStruct(StructPath);
            if (!Struct) { OutError.Code = MCPProtocol::FMCPError::InvalidParams; OutError.Message = FString::Printf(TEXT("struct not found: %s"), *StructPath); return nullptr; }

            const FGuid Id = AddNodeToArray(State->Tasks, Ed, Struct);
            Tree->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("node_id"), Id.ToString());
            return Result;
        });

    // statetree.add_transition — Params: tree_path, from_state_id, to_state_id, trigger (default OnTick).
    Registry.Register(TEXT("statetree.add_transition"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UStateTree* Tree = LoadTree(Params, OutError); if (!Tree) return nullptr;
            UStateTreeEditorData* Ed = GetEditorData(Tree, OutError); if (!Ed) return nullptr;

            FString FromId, ToId;
            if (!RequireString(Params, TEXT("from_state_id"), FromId, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("to_state_id"), ToId, OutError)) return nullptr;
            FString TriggerStr; Params->TryGetStringField(TEXT("trigger"), TriggerStr);

            FGuid FromGuid, ToGuid; FGuid::Parse(FromId, FromGuid); FGuid::Parse(ToId, ToGuid);
            UStateTreeState* From = FindStateById(Ed, FromGuid);
            UStateTreeState* To = FindStateById(Ed, ToGuid);
            if (!From || !To) { OutError.Code = MCPProtocol::FMCPError::InvalidParams; OutError.Message = TEXT("from/to state not found"); return nullptr; }

            FStateTreeTransition& Transition = From->AddTransition(ParseTrigger(TriggerStr), EStateTreeTransitionType::GotoState, To);
            Tree->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("transition_id"), Transition.ID.ToString());
            return Result;
        });

    // statetree.remove_transition — removes one transition from a state. Params: tree_path,
    // from_state_id, transition_id (both from statetree.list_states). Returns { removed }.
    //
    // Identified by transition id rather than index: list_states reports ids, and an index would go
    // stale the moment anything else on that state is edited — removing the wrong transition is silent
    // and only shows up as the AI taking a path it should not.
    Registry.Register(TEXT("statetree.remove_transition"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UStateTree* Tree = LoadTree(Params, OutError); if (!Tree) return nullptr;
            UStateTreeEditorData* Ed = GetEditorData(Tree, OutError); if (!Ed) return nullptr;

            FString FromId, TransitionId;
            if (!RequireString(Params, TEXT("from_state_id"), FromId, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("transition_id"), TransitionId, OutError)) return nullptr;

            FGuid FromGuid, TransGuid;
            FGuid::Parse(FromId, FromGuid);
            FGuid::Parse(TransitionId, TransGuid);

            UStateTreeState* From = FindStateById(Ed, FromGuid);
            if (!From)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("from_state not found");
                return nullptr;
            }

            const int32 Index = From->Transitions.IndexOfByPredicate(
                [&TransGuid](const FStateTreeTransition& T) { return T.ID == TransGuid; });
            if (Index == INDEX_NONE)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("transition %s not found on state %s"),
                    *TransitionId, *From->Name.ToString());
                return nullptr;
            }

            From->Modify();
            From->Transitions.RemoveAt(Index);
            Tree->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("removed"), true);
            Result->SetNumberField(TEXT("remaining"), From->Transitions.Num());
            return Result;
        });

    // statetree.add_binding — binds an output of source node to an input of target node.
    // Params: tree_path, source_node_id, source_path, target_node_id, target_path.
    Registry.Register(TEXT("statetree.add_binding"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UStateTree* Tree = LoadTree(Params, OutError); if (!Tree) return nullptr;
            UStateTreeEditorData* Ed = GetEditorData(Tree, OutError); if (!Ed) return nullptr;

            FString SrcId, SrcPath, TgtId, TgtPath;
            if (!RequireString(Params, TEXT("source_node_id"), SrcId, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("source_path"), SrcPath, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("target_node_id"), TgtId, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("target_path"), TgtPath, OutError)) return nullptr;

            FGuid SrcGuid, TgtGuid; FGuid::Parse(SrcId, SrcGuid); FGuid::Parse(TgtId, TgtGuid);

            FPropertyBindingPath Source; Source.SetStructID(SrcGuid);
            FPropertyBindingPath Target; Target.SetStructID(TgtGuid);
            if (!Source.FromString(SrcPath) || !Target.FromString(TgtPath))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("invalid source/target property path");
                return nullptr;
            }
            Ed->AddPropertyBinding(Source, Target);
            Tree->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            return Result;
        });

    // statetree.remove_state — removes a state (and its subtree) by id. Params: tree_path, state_id.
    // Returns { removed }. Root-level states are removed from the tree's SubTrees.
    Registry.Register(TEXT("statetree.remove_state"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UStateTree* Tree = LoadTree(Params, OutError); if (!Tree) return nullptr;
            UStateTreeEditorData* Ed = GetEditorData(Tree, OutError); if (!Ed) return nullptr;

            FString StateId; if (!RequireString(Params, TEXT("state_id"), StateId, OutError)) return nullptr;
            FGuid Guid; FGuid::Parse(StateId, Guid);
            UStateTreeState* State = FindStateById(Ed, Guid);
            if (!State) { OutError.Code = MCPProtocol::FMCPError::InvalidParams; OutError.Message = TEXT("state_id not found"); return nullptr; }

            if (UStateTreeState* Parent = State->Parent)
            {
                Parent->Modify();
                Parent->Children.Remove(State);
            }
            else
            {
                Ed->Modify();
                Ed->SubTrees.Remove(State);
            }
            Tree->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("removed"), true);
            return Result;
        });

    // statetree.list_states — dumps the state hierarchy. Params: tree_path. Returns { states: [...] },
    // each entry { id, name, tasks[], enter_conditions[], transitions[], children[] }.
    //
    // 쓰기 명령만 있고 읽기가 없으면 트리를 **눈으로 열어봐야만** 편집할 수 있다. 상태 선택은 형제
    // 순서가 결정하므로(먼저 조건을 만족하는 형제가 이긴다) 어디에 넣을지조차 순서를 모르면 정할 수
    // 없고, 애셋 EditorData 는 protected 라 에디터 파이썬으로도 못 읽는다. 그 구멍을 메우는 명령이다.
    Registry.Register(TEXT("statetree.list_states"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UStateTree* Tree = LoadTree(Params, OutError); if (!Tree) return nullptr;
            UStateTreeEditorData* Ed = GetEditorData(Tree, OutError); if (!Ed) return nullptr;

            // 노드 배열을 구조체 경로 문자열로 편다. 이름만으로는 같은 이름의 다른 모듈 노드와 구분이
            // 안 되므로 add_task/add_condition 에 그대로 되먹일 수 있는 전체 경로로 낸다.
            auto NodeNames = [](const TArray<FStateTreeEditorNode>& Nodes)
            {
                TArray<TSharedPtr<FJsonValue>> Out;
                for (const FStateTreeEditorNode& N : Nodes)
                {
                    const UScriptStruct* Struct = N.Node.GetScriptStruct();
                    Out.Add(MakeShared<FJsonValueString>(Struct ? Struct->GetPathName() : TEXT("(empty)")));
                }
                return Out;
            };

            // 재귀 람다. TFunction 으로 자기 자신을 잡아야 하위 상태까지 내려간다.
            TFunction<TSharedPtr<FJsonObject>(UStateTreeState&)> Describe;
            Describe = [&Describe, &NodeNames](UStateTreeState& State) -> TSharedPtr<FJsonObject>
            {
                auto Obj = MakeShared<FJsonObject>();
                Obj->SetStringField(TEXT("id"), State.ID.ToString());
                Obj->SetStringField(TEXT("name"), State.Name.ToString());

                TArray<FStateTreeEditorNode> Tasks = State.Tasks;
                if (State.SingleTask.Node.IsValid())
                {
                    // 단일 태스크는 배열 밖에 따로 산다. 여기서 합쳐 두지 않으면 태스크가 하나뿐인
                    // 상태가 "태스크 없음"으로 보인다.
                    Tasks.Add(State.SingleTask);
                }
                Obj->SetArrayField(TEXT("tasks"), NodeNames(Tasks));
                Obj->SetArrayField(TEXT("enter_conditions"), NodeNames(State.EnterConditions));

                TArray<TSharedPtr<FJsonValue>> Transitions;
                for (const FStateTreeTransition& T : State.Transitions)
                {
                    auto TObj = MakeShared<FJsonObject>();
                    TObj->SetStringField(TEXT("id"), T.ID.ToString());
                    // EStateTreeTransitionTrigger 는 스코프드 enum(uint8 비트플래그)이라 숫자로 바로 못 넘긴다.
                    TObj->SetNumberField(TEXT("trigger"), static_cast<double>(static_cast<uint8>(T.Trigger)));
                    TObj->SetStringField(TEXT("target"), T.State.Name.ToString());
                    TObj->SetNumberField(TEXT("condition_count"), T.Conditions.Num());
                    Transitions.Add(MakeShared<FJsonValueObject>(TObj));
                }
                Obj->SetArrayField(TEXT("transitions"), Transitions);

                TArray<TSharedPtr<FJsonValue>> Children;
                for (UStateTreeState* Child : State.Children)
                {
                    if (Child)
                    {
                        Children.Add(MakeShared<FJsonValueObject>(Describe(*Child)));
                    }
                }
                Obj->SetArrayField(TEXT("children"), Children);
                return Obj;
            };

            TArray<TSharedPtr<FJsonValue>> Roots;
            for (UStateTreeState* Sub : Ed->SubTrees)
            {
                if (Sub)
                {
                    Roots.Add(MakeShared<FJsonValueObject>(Describe(*Sub)));
                }
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("states"), Roots);
            return Result;
        });

    // statetree.move_state — reorders a state among its siblings, optionally reparenting it.
    // Params: tree_path, state_id, index (0-based; <0 or past the end = last),
    //         parent_state_id (optional; omit to keep the current parent, empty string = root subtree list).
    // Returns { moved, parent, index }.
    //
    // add_state 는 AddChildState 라 **항상 끝에 붙는다.** 그런데 조건 없는 상태(Patrol 같은)가 앞에 있으면
    // 뒤의 형제는 영원히 선택되지 않으므로, 새로 심은 상태는 대개 옮겨야 쓸모가 생긴다. 그 한 걸음 때문에
    // 매번 에디터를 열어 드래그하게 두지 않으려고 있는 명령이다.
    Registry.Register(TEXT("statetree.move_state"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UStateTree* Tree = LoadTree(Params, OutError); if (!Tree) return nullptr;
            UStateTreeEditorData* Ed = GetEditorData(Tree, OutError); if (!Ed) return nullptr;

            FString StateId; if (!RequireString(Params, TEXT("state_id"), StateId, OutError)) return nullptr;
            FGuid Guid; FGuid::Parse(StateId, Guid);
            UStateTreeState* State = FindStateById(Ed, Guid);
            if (!State) { OutError.Code = MCPProtocol::FMCPError::InvalidParams; OutError.Message = TEXT("state_id not found"); return nullptr; }

            // 새 부모를 정한다. 필드를 아예 안 주면 지금 부모를 유지한다(순수 순서 변경) — 빈 문자열은
            // "루트 서브트리로"라는 뜻이라 서로 다르다. add_state 의 parent_state_id 규칙과 같은 축이다.
            const bool bReparent = Params->HasField(TEXT("parent_state_id"));
            UStateTreeState* NewParent = State->Parent;
            if (bReparent)
            {
                FString ParentId;
                Params->TryGetStringField(TEXT("parent_state_id"), ParentId);
                if (ParentId.IsEmpty())
                {
                    NewParent = nullptr;
                }
                else
                {
                    FGuid ParentGuid; FGuid::Parse(ParentId, ParentGuid);
                    NewParent = FindStateById(Ed, ParentGuid);
                    if (!NewParent) { OutError.Code = MCPProtocol::FMCPError::InvalidParams; OutError.Message = TEXT("parent_state_id not found"); return nullptr; }
                }

                // 자기 자신이나 자기 자손 밑으로 넣으면 계층이 고리가 되어 순회가 무한이 된다.
                for (UStateTreeState* Ancestor = NewParent; Ancestor; Ancestor = Ancestor->Parent)
                {
                    if (Ancestor == State)
                    {
                        OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                        OutError.Message = TEXT("cannot move a state under itself or its own descendant");
                        return nullptr;
                    }
                }
            }

            int32 Index = -1;
            if (Params->HasTypedField<EJson::Number>(TEXT("index")))
            {
                Index = static_cast<int32>(Params->GetNumberField(TEXT("index")));
            }

            TArray<TObjectPtr<UStateTreeState>>& FromArray = State->Parent ? State->Parent->Children : Ed->SubTrees;
            TArray<TObjectPtr<UStateTreeState>>& ToArray   = NewParent      ? NewParent->Children      : Ed->SubTrees;

            Ed->Modify();
            if (State->Parent) { State->Parent->Modify(); }
            if (NewParent)     { NewParent->Modify(); }
            State->Modify();

            FromArray.Remove(State);

            // 같은 배열 안에서 옮길 때는 제거 뒤 길이를 기준으로 자른다 — 제거 전 길이로 클램프하면
            // "맨 뒤로" 요청이 범위를 벗어난다.
            if (Index < 0 || Index > ToArray.Num()) { Index = ToArray.Num(); }
            ToArray.Insert(State, Index);
            State->Parent = NewParent;

            Tree->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("moved"), true);
            Result->SetStringField(TEXT("parent"), NewParent ? NewParent->Name.ToString() : TEXT("(root)"));
            Result->SetNumberField(TEXT("index"), Index);
            return Result;
        });

    // statetree.add_condition — adds a condition node, gating either a transition (transition_id) or a
    // state's selection (state_id, an enter condition). Params: tree_path, one of transition_id/state_id,
    // struct_path (optional; default FStateTreeCompareBoolCondition). Returns { node_id }.
    // Typical wiring: bind an evaluator bool output to the condition input via add_binding
    // (target_path "bLeft"), then set the compared constant via set_node_property (property "bRight").
    Registry.Register(TEXT("statetree.add_condition"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UStateTree* Tree = LoadTree(Params, OutError); if (!Tree) return nullptr;
            UStateTreeEditorData* Ed = GetEditorData(Tree, OutError); if (!Ed) return nullptr;

            FString TransId, StateId;
            Params->TryGetStringField(TEXT("transition_id"), TransId);
            Params->TryGetStringField(TEXT("state_id"), StateId);
            if (TransId.IsEmpty() == StateId.IsEmpty())
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("provide exactly one of transition_id (transition condition) or state_id (enter condition)");
                return nullptr;
            }

            FString StructPath; Params->TryGetStringField(TEXT("struct_path"), StructPath);
            if (StructPath.IsEmpty()) { StructPath = TEXT("/Script/StateTreeModule.StateTreeCompareBoolCondition"); }
            const UScriptStruct* Struct = FindNodeStruct(StructPath);
            if (!Struct) { OutError.Code = MCPProtocol::FMCPError::InvalidParams; OutError.Message = FString::Printf(TEXT("struct not found: %s"), *StructPath); return nullptr; }

            TArray<FStateTreeEditorNode>* Target = nullptr;
            if (!TransId.IsEmpty())
            {
                FGuid TransGuid; FGuid::Parse(TransId, TransGuid);
                FStateTreeTransition* Transition = FindTransitionById(Ed, TransGuid);
                if (!Transition) { OutError.Code = MCPProtocol::FMCPError::InvalidParams; OutError.Message = TEXT("transition_id not found"); return nullptr; }
                Target = &Transition->Conditions;
            }
            else
            {
                FGuid StateGuid; FGuid::Parse(StateId, StateGuid);
                UStateTreeState* State = FindStateById(Ed, StateGuid);
                if (!State) { OutError.Code = MCPProtocol::FMCPError::InvalidParams; OutError.Message = TEXT("state_id not found"); return nullptr; }
                State->Modify();
                Target = &State->EnterConditions;
            }

            const FGuid Id = AddNodeToArray(*Target, Ed, Struct);
            Tree->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("node_id"), Id.ToString());
            return Result;
        });

    // statetree.set_node_property — sets a property on a node's instance data by reflection.
    // Works for any evaluator/task/condition node found by id. Params: tree_path, node_id,
    // property (name), value (Unreal import text, e.g. "true" or "(TagName=\"Ability.Monster.Melee\")").
    // Returns { ok }.
    Registry.Register(TEXT("statetree.set_node_property"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UStateTree* Tree = LoadTree(Params, OutError); if (!Tree) return nullptr;
            UStateTreeEditorData* Ed = GetEditorData(Tree, OutError); if (!Ed) return nullptr;

            FString NodeId, PropName, Value;
            if (!RequireString(Params, TEXT("node_id"), NodeId, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("property"), PropName, OutError)) return nullptr;
            if (!Params->TryGetStringField(TEXT("value"), Value))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams; OutError.Message = TEXT("value is required"); return nullptr;
            }

            FGuid Guid; FGuid::Parse(NodeId, Guid);
            FStateTreeEditorNode* Node = FindNodeById(Ed, Guid);
            if (!Node) { OutError.Code = MCPProtocol::FMCPError::InvalidParams; OutError.Message = TEXT("node_id not found"); return nullptr; }

            const FStateTreeDataView View = Node->GetInstance();
            const UStruct* Struct = View.GetStruct();
            void* Memory = View.GetMutableMemory();
            if (!Struct || !Memory) { OutError.Code = MCPProtocol::FMCPError::InternalError; OutError.Message = TEXT("node has no instance data"); return nullptr; }

            FProperty* Prop = Struct->FindPropertyByName(FName(*PropName));
            if (!Prop) { OutError.Code = MCPProtocol::FMCPError::InvalidParams; OutError.Message = FString::Printf(TEXT("property not found: %s"), *PropName); return nullptr; }

            void* Addr = Prop->ContainerPtrToValuePtr<void>(Memory);
            Ed->Modify();
            const TCHAR* Result = Prop->ImportText_Direct(*Value, Addr, nullptr, PPF_None);
            if (Result == nullptr) { OutError.Code = MCPProtocol::FMCPError::InvalidParams; OutError.Message = FString::Printf(TEXT("failed to import value '%s' for %s"), *Value, *PropName); return nullptr; }
            Tree->MarkPackageDirty();

            auto Out = MakeShared<FJsonObject>();
            Out->SetBoolField(TEXT("ok"), true);
            return Out;
        });

    // statetree.compile — compiles the StateTree from its editor data. Params: tree_path.
    Registry.Register(TEXT("statetree.compile"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UStateTree* Tree = LoadTree(Params, OutError); if (!Tree) return nullptr;

            FStateTreeCompilerLog Log;
            const bool bOk = UStateTreeEditingSubsystem::CompileStateTree(Tree, Log);
            Tree->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("compiled"), bOk);
            return Result;
        });
}
