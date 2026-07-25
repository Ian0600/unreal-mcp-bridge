#include "Commands/StateTreeCommands.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "StateTree.h"
#include "StateTreeState.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeTypes.h"
// UE 5.3 authoring APIs (the 5.5+ StateTreeEditingSubsystem and PropertyBindingUtils module do not exist here)
#include "StateTreeCompiler.h"                 // FStateTreeCompiler
#include "StateTreeNodeBase.h"                 // FStateTreeNodeBase::GetInstanceDataType
#include "StateTreePropertyBindings.h"         // FStateTreePropertyPath
#include "StateTreeDelegates.h"                // UE::StateTree::Delegates::OnPostCompile
#include "Serialization/ArchiveObjectCrc32.h"  // FArchiveObjectCrc32 (editor-data hash)

namespace
{
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

    bool RequireString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FString& Out, MCPProtocol::FMCPError& OutError)
    {
        if (!Params->TryGetStringField(Field, Out) || Out.IsEmpty())
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("%s is required and must be non-empty"), Field);
            return false;
        }
        return true;
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

    // Adds an editor node (task/eval/condition) of the given struct into an array, returns its new GUID.
    // UE 5.3: FStateTreeEditorNode has no unified InitializeAs(Outer, Struct). Initialize the node struct
    // (FInstancedStruct Node) directly, then allocate its instance data from the node's declared type
    // (GetInstanceDataType is virtual on FStateTreeNodeBase). Mirrors UStateTreeEditorData::AddEvaluator<T>.
    FGuid AddNodeToArray(TArray<FStateTreeEditorNode>& Array, const UScriptStruct* Struct)
    {
        FStateTreeEditorNode& Node = Array.AddDefaulted_GetRef();
        Node.ID = FGuid::NewGuid();
        Node.Node.InitializeAs(Struct);
        if (const FStateTreeNodeBase* NodePtr = Node.Node.GetPtr<FStateTreeNodeBase>())
        {
            if (const UScriptStruct* InstanceType = Cast<const UScriptStruct>(NodePtr->GetInstanceDataType()))
            {
                Node.Instance.InitializeAs(InstanceType);
            }
        }
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

            const FGuid Id = AddNodeToArray(Ed->Evaluators, Struct);
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

            const FGuid Id = AddNodeToArray(State->Tasks, Struct);
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

            FStateTreePropertyPath Source; Source.SetStructID(SrcGuid);
            FStateTreePropertyPath Target; Target.SetStructID(TgtGuid);
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

            const FGuid Id = AddNodeToArray(*Target, Struct);
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

            // UE 5.3: no UStateTreeEditingSubsystem::CompileStateTree. Drive FStateTreeCompiler directly,
            // mirroring FStateTreeEditor::Compile / UStateTreeCompileAllCommandlet: hash editor data, compile,
            // then on success stamp the hash and broadcast OnPostCompile; on failure reset compiled data.
            uint32 EditorDataHash = 0;
            if (Tree->EditorData)
            {
                FArchiveObjectCrc32 Crc;
                EditorDataHash = Crc.Crc32(Tree->EditorData, 0);
            }

            FStateTreeCompilerLog Log;
            FStateTreeCompiler Compiler(Log);
            const bool bOk = Compiler.Compile(*Tree);
            if (bOk)
            {
                Tree->LastCompiledEditorDataHash = EditorDataHash;
                UE::StateTree::Delegates::OnPostCompile.Broadcast(*Tree);
            }
            else
            {
                Tree->ResetCompiled();
                Tree->LastCompiledEditorDataHash = 0;
            }
            Tree->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("compiled"), bOk);
            return Result;
        });
}
