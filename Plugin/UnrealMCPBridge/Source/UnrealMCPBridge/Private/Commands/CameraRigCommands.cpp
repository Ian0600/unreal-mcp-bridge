#include "Commands/CameraRigCommands.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "Core/CameraRigAsset.h"
#include "Core/CameraNode.h"
#include "UObject/UObjectHash.h"

namespace
{
    UCameraRigAsset* LoadRig(const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        FString Path;
        if (!Params.IsValid() || !Params->TryGetStringField(TEXT("rig_path"), Path) || Path.IsEmpty())
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = TEXT("rig_path is required and must be non-empty");
            return nullptr;
        }
        UCameraRigAsset* Rig = LoadObject<UCameraRigAsset>(nullptr, *Path);
        if (!Rig)
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("Could not load CameraRigAsset: %s"), *Path);
        }
        return Rig;
    }

    bool RigRequireString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FString& Out, MCPProtocol::FMCPError& OutError)
    {
        if (!Params->TryGetStringField(Field, Out) || Out.IsEmpty())
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("%s is required and must be non-empty"), Field);
            return false;
        }
        return true;
    }

    // Collects every UCameraNode instanced under the rig (connected or loose), in discovery order.
    void CollectNodes(UCameraRigAsset* Rig, TArray<UCameraNode*>& OutNodes)
    {
        ForEachObjectWithOuter(Rig, [&OutNodes](UObject* Obj)
        {
            if (UCameraNode* Node = Cast<UCameraNode>(Obj))
            {
                OutNodes.Add(Node);
            }
        }, /*bIncludeNestedObjects=*/true);
    }

    // Matches a node's class by short name, case-insensitive, with or without the leading 'U'
    // (e.g. "BoomArmCameraNode" or "UBoomArmCameraNode").
    bool ClassNameMatches(const UCameraNode* Node, const FString& Wanted)
    {
        const FString ClassName = Node->GetClass()->GetName();
        return ClassName.Equals(Wanted, ESearchCase::IgnoreCase)
            || ClassName.Equals(Wanted.RightChop(Wanted.StartsWith(TEXT("U")) ? 1 : 0), ESearchCase::IgnoreCase)
            || (TEXT("U") + ClassName).Equals(Wanted, ESearchCase::IgnoreCase);
    }
}

void FCameraRigCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    // camerarig.get_info — lists the rig's camera nodes. Params: rig_path.
    // Returns { root_node_class, nodes: [{class, name}] }.
    Registry.Register(TEXT("camerarig.get_info"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UCameraRigAsset* Rig = LoadRig(Params, OutError); if (!Rig) return nullptr;

            TArray<UCameraNode*> Nodes;
            CollectNodes(Rig, Nodes);

            TArray<TSharedPtr<FJsonValue>> NodeArray;
            for (const UCameraNode* Node : Nodes)
            {
                auto Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("class"), Node->GetClass()->GetName());
                Entry->SetStringField(TEXT("name"), Node->GetName());
                NodeArray.Add(MakeShared<FJsonValueObject>(Entry));
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("root_node_class"),
                Rig->RootNode ? Rig->RootNode->GetClass()->GetName() : TEXT("None"));
            Result->SetArrayField(TEXT("nodes"), NodeArray);
            return Result;
        });

    // camerarig.set_node_property — sets a property on a node (found by class) via reflection.
    // Params: rig_path, node_class (short class name), property_name, value (ImportText string),
    //         node_index (optional, default 0 — which match to edit). Returns { ok }.
    // Example: node_class="BoomArmCameraNode", property_name="BoomOffset",
    //          value="(Value=(X=-200.0,Y=60.0,Z=60.0))".
    Registry.Register(TEXT("camerarig.set_node_property"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UCameraRigAsset* Rig = LoadRig(Params, OutError); if (!Rig) return nullptr;

            FString NodeClass, PropertyName, Value;
            if (!RigRequireString(Params, TEXT("node_class"), NodeClass, OutError)) return nullptr;
            if (!RigRequireString(Params, TEXT("property_name"), PropertyName, OutError)) return nullptr;
            if (!RigRequireString(Params, TEXT("value"), Value, OutError)) return nullptr;
            double IndexD = 0; Params->TryGetNumberField(TEXT("node_index"), IndexD);
            const int32 WantIndex = static_cast<int32>(IndexD);

            TArray<UCameraNode*> Nodes;
            CollectNodes(Rig, Nodes);

            UCameraNode* Target = nullptr;
            int32 MatchIndex = 0;
            for (UCameraNode* Node : Nodes)
            {
                if (ClassNameMatches(Node, NodeClass))
                {
                    if (MatchIndex == WantIndex) { Target = Node; break; }
                    ++MatchIndex;
                }
            }
            if (!Target)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("No node of class '%s' (index %d) in rig"), *NodeClass, WantIndex);
                return nullptr;
            }

            FProperty* Prop = FindFProperty<FProperty>(Target->GetClass(), *PropertyName);
            if (!Prop)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("Property '%s' not found on %s"), *PropertyName, *Target->GetClass()->GetName());
                return nullptr;
            }

            Target->Modify();
            void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Target);
            const TCHAR* Result = Prop->ImportText_Direct(*Value, ValuePtr, Target, PPF_None);
            if (Result == nullptr)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("Failed to parse value '%s' for property '%s'"), *Value, *PropertyName);
                return nullptr;
            }

            Rig->DirtyBuildStatus();
            Rig->MarkPackageDirty();

            auto Out = MakeShared<FJsonObject>();
            Out->SetBoolField(TEXT("ok"), true);
            Out->SetStringField(TEXT("node"), Target->GetName());
            return Out;
        });

    // camerarig.build — rebuilds the rig (validate + allocation info) and marks it dirty.
    // Params: rig_path. Returns { ok }.
    Registry.Register(TEXT("camerarig.build"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UCameraRigAsset* Rig = LoadRig(Params, OutError); if (!Rig) return nullptr;

            Rig->BuildCameraRig();
            Rig->MarkPackageDirty();

            auto Out = MakeShared<FJsonObject>();
            Out->SetBoolField(TEXT("ok"), true);
            return Out;
        });
}
