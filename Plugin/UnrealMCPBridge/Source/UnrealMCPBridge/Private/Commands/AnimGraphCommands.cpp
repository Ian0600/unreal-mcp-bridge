#include "Commands/AnimGraphCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "MCPGraphEditLibrary.h"
#include "Dom/JsonObject.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "ScopedTransaction.h"

namespace
{
    using UnrealMCPBridge::Json::RequireString;

    // Loads and casts blueprint_path to UAnimBlueprint, or sets OutError and returns null.
    UAnimBlueprint* LoadRequiredAnimBlueprint(const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        if (!Params.IsValid())
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = TEXT("params object is required");
            return nullptr;
        }

        FString BlueprintPath;
        if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = TEXT("blueprint_path is required and must be non-empty");
            return nullptr;
        }

        UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *BlueprintPath);
        if (!AnimBP)
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("Could not load AnimBlueprint (or not an AnimBlueprint): %s"), *BlueprintPath);
            return nullptr;
        }
        return AnimBP;
    }

    int32 GetIntFieldOrDefault(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32 Default)
    {
        double Value = Default;
        Params->TryGetNumberField(FieldName, Value);
        return static_cast<int32>(Value);
    }
}

void FAnimGraphCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    // animgraph.add_state_machine — adds a State Machine node to an AnimGraph.
    // Params: blueprint_path, graph_name (default "AnimGraph"), pos_x, pos_y.
    Registry.Register(TEXT("animgraph.add_state_machine"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimBlueprint* AnimBP = LoadRequiredAnimBlueprint(Params, OutError);
            if (!AnimBP) return nullptr;

            FString GraphName = Params->HasField(TEXT("graph_name")) ? Params->GetStringField(TEXT("graph_name")) : TEXT("AnimGraph");
            const int32 PosX = GetIntFieldOrDefault(Params, TEXT("pos_x"), 0);
            const int32 PosY = GetIntFieldOrDefault(Params, TEXT("pos_y"), 0);

            const FString NodeName = UMCPGraphEditLibrary::AddAnimStateMachineNode(AnimBP, GraphName, PosX, PosY);
            if (NodeName.IsEmpty())
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("Failed to add state machine node (see UnrealMCPBridge log for details)");
                return nullptr;
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("node_name"), NodeName);
            return Result;
        });

    // animgraph.add_state — adds a state to a state machine's inner graph.
    // Params: blueprint_path, graph_name (default "AnimGraph"), state_machine_node, state_name, pos_x, pos_y.
    Registry.Register(TEXT("animgraph.add_state"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimBlueprint* AnimBP = LoadRequiredAnimBlueprint(Params, OutError);
            if (!AnimBP) return nullptr;

            FString StateMachineNode, StateName;
            if (!RequireString(Params, TEXT("state_machine_node"), StateMachineNode, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("state_name"), StateName, OutError)) return nullptr;

            FString GraphName = Params->HasField(TEXT("graph_name")) ? Params->GetStringField(TEXT("graph_name")) : TEXT("AnimGraph");
            const int32 PosX = GetIntFieldOrDefault(Params, TEXT("pos_x"), 0);
            const int32 PosY = GetIntFieldOrDefault(Params, TEXT("pos_y"), 0);

            const FString NodeName = UMCPGraphEditLibrary::AddAnimState(AnimBP, GraphName, StateMachineNode, StateName, PosX, PosY);
            if (NodeName.IsEmpty())
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("Failed to add state (see UnrealMCPBridge log for details)");
                return nullptr;
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("node_name"), NodeName);
            return Result;
        });

    // animgraph.add_blend_space_player — adds a BlendSpacePlayer node inside a state, wired to its pose sink.
    // Params: blueprint_path, graph_name (default "AnimGraph"), state_machine_node, state_name,
    //         blend_space_path, pos_x, pos_y.
    Registry.Register(TEXT("animgraph.add_blend_space_player"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimBlueprint* AnimBP = LoadRequiredAnimBlueprint(Params, OutError);
            if (!AnimBP) return nullptr;

            FString StateMachineNode, StateName, BlendSpacePath;
            if (!RequireString(Params, TEXT("state_machine_node"), StateMachineNode, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("state_name"), StateName, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("blend_space_path"), BlendSpacePath, OutError)) return nullptr;

            FString GraphName = Params->HasField(TEXT("graph_name")) ? Params->GetStringField(TEXT("graph_name")) : TEXT("AnimGraph");
            const int32 PosX = GetIntFieldOrDefault(Params, TEXT("pos_x"), 0);
            const int32 PosY = GetIntFieldOrDefault(Params, TEXT("pos_y"), 0);

            const FString NodeName = UMCPGraphEditLibrary::AddBlendSpacePlayerToState(
                AnimBP, GraphName, StateMachineNode, StateName, BlendSpacePath, PosX, PosY);
            if (NodeName.IsEmpty())
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("Failed to add blend space player (see UnrealMCPBridge log for details)");
                return nullptr;
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("node_name"), NodeName);
            return Result;
        });

    // animgraph.add_transition — adds a transition between two states.
    // Params: blueprint_path, graph_name (default "AnimGraph"), state_machine_node, from_state, to_state,
    //         condition_property (optional, e.g. "bIsInAir"), negate_condition (optional bool).
    Registry.Register(TEXT("animgraph.add_transition"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimBlueprint* AnimBP = LoadRequiredAnimBlueprint(Params, OutError);
            if (!AnimBP) return nullptr;

            FString StateMachineNode, FromState, ToState;
            if (!RequireString(Params, TEXT("state_machine_node"), StateMachineNode, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("from_state"), FromState, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("to_state"), ToState, OutError)) return nullptr;

            FString GraphName = Params->HasField(TEXT("graph_name")) ? Params->GetStringField(TEXT("graph_name")) : TEXT("AnimGraph");
            FString ConditionProperty;
            Params->TryGetStringField(TEXT("condition_property"), ConditionProperty);
            bool bNegate = false;
            Params->TryGetBoolField(TEXT("negate_condition"), bNegate);

            const FString NodeName = UMCPGraphEditLibrary::AddAnimTransition(
                AnimBP, GraphName, StateMachineNode, FromState, ToState, ConditionProperty, bNegate);
            if (NodeName.IsEmpty())
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("Failed to add transition (see UnrealMCPBridge log for details)");
                return nullptr;
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("node_name"), NodeName);
            return Result;
        });

    // animgraph.add_variable_get — adds a Get <property> node to a (possibly nested) graph.
    // Params: blueprint_path, graph_name (state bound graphs resolve by state name), property_name,
    //         pos_x, pos_y. Its output pin is named after the property. Returns { node_name }.
    Registry.Register(TEXT("animgraph.add_variable_get"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimBlueprint* AnimBP = LoadRequiredAnimBlueprint(Params, OutError);
            if (!AnimBP) return nullptr;

            FString GraphName, PropertyName;
            if (!RequireString(Params, TEXT("graph_name"), GraphName, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("property_name"), PropertyName, OutError)) return nullptr;

            const int32 PosX = GetIntFieldOrDefault(Params, TEXT("pos_x"), 0);
            const int32 PosY = GetIntFieldOrDefault(Params, TEXT("pos_y"), 0);

            const FString NodeName = UMCPGraphEditLibrary::AddVariableGetNode(AnimBP, GraphName, PropertyName, PosX, PosY);
            if (NodeName.IsEmpty())
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("Failed to add variable get node (see UnrealMCPBridge log for details)");
                return nullptr;
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("node_name"), NodeName);
            return Result;
        });

    // animgraph.connect_pins — wires src node's out pin to dst node's in pin within one graph.
    // Params: blueprint_path, graph_name, src_node, src_pin, dst_node, dst_pin. Returns { connected }.
    Registry.Register(TEXT("animgraph.connect_pins"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimBlueprint* AnimBP = LoadRequiredAnimBlueprint(Params, OutError);
            if (!AnimBP) return nullptr;

            FString GraphName, SrcNode, SrcPin, DstNode, DstPin;
            if (!RequireString(Params, TEXT("graph_name"), GraphName, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("src_node"), SrcNode, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("src_pin"), SrcPin, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("dst_node"), DstNode, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("dst_pin"), DstPin, OutError)) return nullptr;

            const bool bOk = UMCPGraphEditLibrary::ConnectPins(AnimBP, GraphName, SrcNode, SrcPin, DstNode, DstPin);
            if (!bOk)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("Failed to connect pins (check graph/node/pin names; see UnrealMCPBridge log)");
                return nullptr;
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("connected"), true);
            return Result;
        });

    // animgraph.disconnect_pins — removes a specific link between two pins. Params: blueprint_path,
    // graph_name, node_a, pin_a, node_b, pin_b. Returns { disconnected }.
    Registry.Register(TEXT("animgraph.disconnect_pins"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimBlueprint* AnimBP = LoadRequiredAnimBlueprint(Params, OutError);
            if (!AnimBP) return nullptr;

            FString GraphName, NodeA, PinA, NodeB, PinB;
            if (!RequireString(Params, TEXT("graph_name"), GraphName, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("node_a"), NodeA, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("pin_a"), PinA, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("node_b"), NodeB, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("pin_b"), PinB, OutError)) return nullptr;

            const bool bOk = UMCPGraphEditLibrary::DisconnectPinLink(AnimBP, GraphName, NodeA, PinA, NodeB, PinB);
            if (!bOk)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("Failed to disconnect pins (check graph/node/pin names; see UnrealMCPBridge log)");
                return nullptr;
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("disconnected"), true);
            return Result;
        });

    // animgraph.add_sequence_player — adds a SequencePlayer node inside a state's bound graph,
    // sets its Sequence + loop, wires it to the pose sink. Params: blueprint_path, graph_name
    // (default "AnimGraph"), state_machine_node, state_name, sequence_path, loop (default true),
    // pos_x, pos_y. Returns { node_name }.
    Registry.Register(TEXT("animgraph.add_sequence_player"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimBlueprint* AnimBP = LoadRequiredAnimBlueprint(Params, OutError);
            if (!AnimBP) return nullptr;

            FString StateMachineNode, StateName, SequencePath;
            if (!RequireString(Params, TEXT("state_machine_node"), StateMachineNode, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("state_name"), StateName, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("sequence_path"), SequencePath, OutError)) return nullptr;

            FString GraphName = Params->HasField(TEXT("graph_name")) ? Params->GetStringField(TEXT("graph_name")) : TEXT("AnimGraph");
            bool bLoop = true;
            Params->TryGetBoolField(TEXT("loop"), bLoop);
            const int32 PosX = GetIntFieldOrDefault(Params, TEXT("pos_x"), 0);
            const int32 PosY = GetIntFieldOrDefault(Params, TEXT("pos_y"), 0);

            const FString NodeName = UMCPGraphEditLibrary::AddSequencePlayerToState(
                AnimBP, GraphName, StateMachineNode, StateName, SequencePath, bLoop, PosX, PosY);
            if (NodeName.IsEmpty())
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("Failed to add sequence player (see UnrealMCPBridge log for details)");
                return nullptr;
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("node_name"), NodeName);
            return Result;
        });

    // animgraph.add_slot — adds a Slot node (montage layering point) to the top-level AnimGraph.
    // Params: blueprint_path, graph_name (default "AnimGraph"), slot_name, pos_x, pos_y.
    // Pins: "Source" (in) / "Pose" (out) — wire with connect_pins. Returns { node_name }.
    Registry.Register(TEXT("animgraph.add_slot"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimBlueprint* AnimBP = LoadRequiredAnimBlueprint(Params, OutError);
            if (!AnimBP) return nullptr;

            FString SlotName;
            if (!RequireString(Params, TEXT("slot_name"), SlotName, OutError)) return nullptr;

            FString GraphName = Params->HasField(TEXT("graph_name")) ? Params->GetStringField(TEXT("graph_name")) : TEXT("AnimGraph");
            const int32 PosX = GetIntFieldOrDefault(Params, TEXT("pos_x"), 0);
            const int32 PosY = GetIntFieldOrDefault(Params, TEXT("pos_y"), 0);

            const FString NodeName = UMCPGraphEditLibrary::AddAnimSlotNode(AnimBP, GraphName, SlotName, PosX, PosY);
            if (NodeName.IsEmpty())
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("Failed to add slot node (see UnrealMCPBridge log for details)");
                return nullptr;
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("node_name"), NodeName);
            return Result;
        });

    // animgraph.remove_node — removes a node from the named graph. Params: blueprint_path,
    // graph_name, node_name. Returns { removed }.
    Registry.Register(TEXT("animgraph.remove_node"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimBlueprint* AnimBP = LoadRequiredAnimBlueprint(Params, OutError);
            if (!AnimBP) return nullptr;

            FString GraphName, NodeName;
            if (!RequireString(Params, TEXT("graph_name"), GraphName, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("node_name"), NodeName, OutError)) return nullptr;

            const bool bOk = UMCPGraphEditLibrary::RemoveNode(AnimBP, GraphName, NodeName);
            if (!bOk)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("Failed to remove node (check graph/node names; see UnrealMCPBridge log)");
                return nullptr;
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("removed"), true);
            return Result;
        });

    // animgraph.set_preview_mesh — the AnimBP's preview mesh, what Preview Scene → "Apply To Asset"
    // stores. Params: blueprint_path, mesh_path (empty string clears it). Returns { ok, previous, mesh }.
    // Python has no setter for it. The mesh has to be on a skeleton compatible with the AnimBP's target
    // skeleton — the editor refuses others too, and a mismatched preview shows the reference pose.
    // Does not save — use asset.save.
    Registry.Register(TEXT("animgraph.set_preview_mesh"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimBlueprint* AnimBP = LoadRequiredAnimBlueprint(Params, OutError);
            if (!AnimBP) return nullptr;

            FString MeshPath;
            if (!Params->TryGetStringField(TEXT("mesh_path"), MeshPath))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("mesh_path is required (empty string clears the preview mesh)");
                return nullptr;
            }

            USkeletalMesh* Mesh = nullptr;
            if (!MeshPath.IsEmpty())
            {
                Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
                if (!Mesh)
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = FString::Printf(TEXT("Could not load skeletal mesh: %s"), *MeshPath);
                    return nullptr;
                }
                const USkeleton* Target = AnimBP->TargetSkeleton;
                if (Target && (!Mesh->GetSkeleton() || !Target->IsCompatibleForEditor(Mesh->GetSkeleton())))
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = FString::Printf(TEXT("%s uses skeleton %s, not compatible with the AnimBP's %s"),
                        *Mesh->GetName(), *GetNameSafe(Mesh->GetSkeleton()), *Target->GetName());
                    return nullptr;
                }
            }

            const USkeletalMesh* Previous = AnimBP->GetPreviewMesh();
            const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Set AnimBP Preview Mesh")));
            AnimBP->Modify();
            AnimBP->SetPreviewMesh(Mesh, /*bMarkAsDirty=*/true);

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            Result->SetStringField(TEXT("previous"), Previous ? Previous->GetPathName() : FString());
            Result->SetStringField(TEXT("mesh"), Mesh ? Mesh->GetPathName() : FString());
            return Result;
        });
}
