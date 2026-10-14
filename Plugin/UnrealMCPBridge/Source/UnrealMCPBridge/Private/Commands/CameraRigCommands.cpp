#include "Commands/CameraRigCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "Core/CameraRigAsset.h"
#include "Core/CameraNode.h"
#include "Core/CameraObjectInterfaceParameterDefinition.h"
#include "Core/ObjectTreeGraphRootObject.h"
#include "StructUtils/PropertyBag.h"
#include "StructUtils/StructView.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace
{
    using UnrealMCPBridge::Json::RequireString;

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

    // Collects every UCameraNode instanced under the rig (connected or loose), in discovery order.
    void CollectNodes(UCameraRigAsset* Rig, TArray<UCameraNode*>& OutNodes)
    {
        ForEachObjectWithOuter(Rig, [&OutNodes](UObject* Obj)
        {
            if (UCameraNode* Node = Cast<UCameraNode>(Obj))
            {
                OutNodes.Add(Node);
            }
        }, EGetObjectsFlags::IncludeNestedObjects);
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

    /**
     * Resolves the node a command should act on, from either node_name (exact subobject name,
     * e.g. "BoomArmCameraNode_0") or node_class + node_index.
     *
     * node_name is the precise form and the one to prefer: a rig routinely holds several nodes
     * of one class, and node_index then depends on discovery order, which shifts as soon as a
     * node is added or removed. camerarig.get_info reports both.
     */
    UCameraNode* RigResolveTargetNode(const TSharedPtr<FJsonObject>& Params, UCameraRigAsset* Rig, MCPProtocol::FMCPError& OutError)
    {
        TArray<UCameraNode*> Nodes;
        CollectNodes(Rig, Nodes);

        FString NodeName;
        if (Params->TryGetStringField(TEXT("node_name"), NodeName) && !NodeName.IsEmpty())
        {
            for (UCameraNode* Node : Nodes)
            {
                if (Node->GetName().Equals(NodeName, ESearchCase::IgnoreCase))
                {
                    return Node;
                }
            }
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("No node named '%s' in rig"), *NodeName);
            return nullptr;
        }

        FString NodeClass;
        if (!Params->TryGetStringField(TEXT("node_class"), NodeClass) || NodeClass.IsEmpty())
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = TEXT("either node_name or node_class is required");
            return nullptr;
        }

        double IndexD = 0; Params->TryGetNumberField(TEXT("node_index"), IndexD);
        const int32 WantIndex = static_cast<int32>(IndexD);

        int32 MatchIndex = 0;
        for (UCameraNode* Node : Nodes)
        {
            if (ClassNameMatches(Node, NodeClass))
            {
                if (MatchIndex == WantIndex) { return Node; }
                ++MatchIndex;
            }
        }
        OutError.Code = MCPProtocol::FMCPError::InvalidParams;
        OutError.Message = FString::Printf(TEXT("No node of class '%s' (index %d) in rig"), *NodeClass, WantIndex);
        return nullptr;
    }

    /** Finds a UCameraNode subclass by short class name, without FindFirstObject's ambiguity ensure. */
    UClass* RigFindNodeClass(const FString& ShortName)
    {
        const FString Wanted = ShortName.StartsWith(TEXT("U")) ? ShortName.RightChop(1) : ShortName;
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            if (!Class->IsChildOf(UCameraNode::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
            {
                continue;
            }
            if (Class->GetName().Equals(ShortName, ESearchCase::IgnoreCase)
                || Class->GetName().Equals(Wanted, ESearchCase::IgnoreCase))
            {
                return Class;
            }
        }
        return nullptr;
    }

    /**
     * Nulls every reference to Target found under one container, and drops it out of any object
     * array it appears in. Recurses through struct members and struct arrays.
     *
     * Reflection rather than the typed child API on purpose: a node points at other nodes through
     * more than its Children array — a Boom Arm's InputSlot, a blend's inner node, and so on. One
     * reflective sweep catches every such pointer without this file having to know each node class.
     */
    void RigClearRefsIn(void* ContainerAddr, UStruct* Struct, UObject* Owner, UCameraNode* Target, TArray<FString>& OutCleared, const FString& PathPrefix)
    {
        for (TFieldIterator<FProperty> It(Struct); It; ++It)
        {
            FProperty* Prop = *It;
            void* Addr = Prop->ContainerPtrToValuePtr<void>(ContainerAddr);
            const FString Path = PathPrefix.IsEmpty() ? Prop->GetName() : (PathPrefix + TEXT(".") + Prop->GetName());

            if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
            {
                if (ObjProp->GetObjectPropertyValue(Addr) == Target)
                {
                    Owner->Modify();
                    ObjProp->SetObjectPropertyValue(Addr, nullptr);
                    OutCleared.Add(FString::Printf(TEXT("%s.%s"), *Owner->GetName(), *Path));
                }
            }
            else if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
            {
                if (FObjectPropertyBase* InnerObjProp = CastField<FObjectPropertyBase>(ArrayProp->Inner))
                {
                    FScriptArrayHelper Helper(ArrayProp, Addr);
                    // Backwards so removals do not disturb the indices still to be visited.
                    for (int32 Index = Helper.Num() - 1; Index >= 0; --Index)
                    {
                        if (InnerObjProp->GetObjectPropertyValue(Helper.GetRawPtr(Index)) == Target)
                        {
                            Owner->Modify();
                            Helper.RemoveValues(Index, 1);
                            OutCleared.Add(FString::Printf(TEXT("%s.%s[%d]"), *Owner->GetName(), *Path, Index));
                        }
                    }
                }
                else if (FStructProperty* InnerStructProp = CastField<FStructProperty>(ArrayProp->Inner))
                {
                    FScriptArrayHelper Helper(ArrayProp, Addr);
                    for (int32 Index = 0; Index < Helper.Num(); ++Index)
                    {
                        RigClearRefsIn(Helper.GetRawPtr(Index), InnerStructProp->Struct, Owner, Target, OutCleared,
                            FString::Printf(TEXT("%s[%d]"), *Path, Index));
                    }
                }
            }
            else if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
            {
                RigClearRefsIn(Addr, StructProp->Struct, Owner, Target, OutCleared, Path);
            }
        }
    }

    /**
     * Lists the node-to-node links a node holds, as {property, index, node} entries.
     * Reported by property name (not just a flat child list) because rewiring goes back through
     * object.set_property / camerarig.set_node_children, and both address links that way.
     */
    TArray<TSharedPtr<FJsonValue>> RigCollectConnections(UCameraNode* Node)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (TFieldIterator<FProperty> It(Node->GetClass()); It; ++It)
        {
            FProperty* Prop = *It;
            void* Addr = Prop->ContainerPtrToValuePtr<void>(Node);

            if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
            {
                if (UCameraNode* Child = Cast<UCameraNode>(ObjProp->GetObjectPropertyValue(Addr)))
                {
                    auto Entry = MakeShared<FJsonObject>();
                    Entry->SetStringField(TEXT("property"), Prop->GetName());
                    Entry->SetNumberField(TEXT("index"), -1);
                    Entry->SetStringField(TEXT("node"), Child->GetName());
                    Out.Add(MakeShared<FJsonValueObject>(Entry));
                }
            }
            else if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
            {
                FObjectPropertyBase* InnerObjProp = CastField<FObjectPropertyBase>(ArrayProp->Inner);
                if (!InnerObjProp) { continue; }

                FScriptArrayHelper Helper(ArrayProp, Addr);
                for (int32 Index = 0; Index < Helper.Num(); ++Index)
                {
                    UObject* Value = InnerObjProp->GetObjectPropertyValue(Helper.GetRawPtr(Index));
                    auto Entry = MakeShared<FJsonObject>();
                    Entry->SetStringField(TEXT("property"), Prop->GetName());
                    Entry->SetNumberField(TEXT("index"), Index);
                    Entry->SetStringField(TEXT("node"), Value ? Value->GetName() : TEXT("None"));
                    Out.Add(MakeShared<FJsonValueObject>(Entry));
                }
            }
        }
        return Out;
    }

    /**
     * Stamps the rig's build state onto a result object.
     *
     * needs_build is deliberately narrower than "not Clean": CleanWithWarnings is built and
     * usable, and reporting it as needing a build would send callers into a rebuild loop over
     * warnings they cannot clear. build_status carries the exact state for when that matters.
     */
    void RigReportBuildStatus(const UCameraRigAsset* Rig, const TSharedPtr<FJsonObject>& Result)
    {
        const ECameraBuildStatus Status = Rig->GetBuildStatus();
        const TCHAR* StatusName = TEXT("Dirty");
        switch (Status)
        {
        case ECameraBuildStatus::Clean:             StatusName = TEXT("Clean"); break;
        case ECameraBuildStatus::CleanWithWarnings: StatusName = TEXT("CleanWithWarnings"); break;
        case ECameraBuildStatus::WithErrors:        StatusName = TEXT("WithErrors"); break;
        case ECameraBuildStatus::Dirty:             StatusName = TEXT("Dirty"); break;
        }
        Result->SetStringField(TEXT("build_status"), StatusName);
        Result->SetBoolField(TEXT("needs_build"),
            Status == ECameraBuildStatus::Dirty || Status == ECameraBuildStatus::WithErrors);
    }

    /** Human-readable name for a parameter's kind, for list_parameters output. */
    FString RigParameterTypeName(const FCameraObjectInterfaceParameterDefinition& Def)
    {
        return Def.ParameterType == ECameraObjectInterfaceParameterType::Blendable
            ? TEXT("Blendable") : TEXT("Data");
    }
}

void FCameraRigCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    // camerarig.get_info — lists the rig's camera nodes. Params: rig_path.
    // Returns { root_node_class, root_node, build_status, nodes: [{class, name, enabled, connections}] }.
    Registry.Register(TEXT("camerarig.get_info"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UCameraRigAsset* Rig = LoadRig(Params, OutError); if (!Rig) return nullptr;

            TArray<UCameraNode*> Nodes;
            CollectNodes(Rig, Nodes);

            TArray<TSharedPtr<FJsonValue>> NodeArray;
            for (UCameraNode* Node : Nodes)
            {
                auto Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("class"), Node->GetClass()->GetName());
                Entry->SetStringField(TEXT("name"), Node->GetName());
                Entry->SetBoolField(TEXT("enabled"), Node->bIsEnabled);
                Entry->SetArrayField(TEXT("connections"), RigCollectConnections(Node));
                NodeArray.Add(MakeShared<FJsonValueObject>(Entry));
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("root_node_class"),
                Rig->RootNode ? Rig->RootNode->GetClass()->GetName() : TEXT("None"));
            Result->SetStringField(TEXT("root_node"),
                Rig->RootNode ? Rig->RootNode->GetName() : TEXT("None"));
            RigReportBuildStatus(Rig, Result);
            Result->SetArrayField(TEXT("nodes"), NodeArray);
            return Result;
        });

    // camerarig.set_node_property — sets a property on a node (found by class) via reflection.
    // Params: rig_path, node_class (short class name), property_name, value (ImportText string),
    //         node_index (optional, default 0 — which match to edit). Returns { ok }.
    // Example: node_class="BoomArmCameraNode", property_name="BoomOffset",
    //          value="(Value=(X=-200.0,Y=60.0,Z=60.0))".
    //
    // ★ A node property wired to an interface parameter ignores what is written here: the
    // evaluator reads such properties from the variable table instead, seeded from the rig's
    // parameter defaults. camerarig.list_parameters shows which properties those are, and
    // camerarig.set_parameter_default is what actually moves them.
    Registry.Register(TEXT("camerarig.set_node_property"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UCameraRigAsset* Rig = LoadRig(Params, OutError); if (!Rig) return nullptr;

            FString PropertyName, Value;
            if (!RequireString(Params, TEXT("property_name"), PropertyName, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("value"), Value, OutError)) return nullptr;

            UCameraNode* Target = RigResolveTargetNode(Params, Rig, OutError);
            if (!Target) return nullptr;

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

    // camerarig.list_parameters — the rig's exposed interface parameters and their default values.
    // Params: rig_path.
    // Returns { parameters: [{name, type, variable_type, visible, value_type, value}] }.
    //
    // These defaults are what the rig actually evaluates with for any node property wired to a
    // parameter — the node's own field is dead in that case. This command is how you find out
    // which is which before editing anything.
    Registry.Register(TEXT("camerarig.list_parameters"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UCameraRigAsset* Rig = LoadRig(Params, OutError); if (!Rig) return nullptr;

            FInstancedPropertyBag& Bag = Rig->GetDefaultParameters();
            FStructView BagView = Bag.GetMutableValue();
            uint8* BagMemory = BagView.GetMemory();

            TArray<TSharedPtr<FJsonValue>> ParamArray;
            for (const FCameraObjectInterfaceParameterDefinition& Def : Rig->GetParameterDefinitions())
            {
                auto Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"), Def.ParameterName.ToString());
                Entry->SetStringField(TEXT("type"), RigParameterTypeName(Def));
                Entry->SetBoolField(TEXT("visible"), Def.bIsVisible);

                const FPropertyBagPropertyDesc* Desc = Bag.FindPropertyDescByName(Def.ParameterName);
                if (Desc && Desc->CachedProperty && BagMemory)
                {
                    const FProperty* Prop = Desc->CachedProperty;
                    const void* Addr = Prop->ContainerPtrToValuePtr<void>(BagMemory);
                    FString Text;
                    Prop->ExportText_Direct(Text, Addr, /*DefaultValue=*/nullptr, Rig, PPF_None);
                    Entry->SetStringField(TEXT("value_type"), Prop->GetCPPType());
                    Entry->SetStringField(TEXT("value"), Text);
                }
                else
                {
                    // A definition with no matching bag entry means the rig has not been built
                    // since the parameter changed. Say so rather than reporting an empty value.
                    Entry->SetStringField(TEXT("value_type"), TEXT("<unbuilt>"));
                    Entry->SetStringField(TEXT("value"), TEXT("<no default stored — run camerarig.build>"));
                }
                ParamArray.Add(MakeShared<FJsonValueObject>(Entry));
            }

            auto Result = MakeShared<FJsonObject>();
            RigReportBuildStatus(Rig, Result);
            Result->SetArrayField(TEXT("parameters"), ParamArray);
            return Result;
        });

    // camerarig.set_parameter_default — writes an exposed parameter's default value.
    // Params: rig_path, parameter_name, value (ImportText), member (optional).
    // Returns { ok, old_value, new_value }.
    //
    // A blendable parameter's stored value is a camera-parameter struct that wraps the payload
    // alongside its variable binding, e.g.
    //   (Value=(X=-144.0,Y=45.0,Z=40.0),VariableID=(Value=3629198531),Variable=None)
    // Retyping that whole thing risks clobbering the binding, so pass member="Value" and only
    // the payload — "(X=-144.0,Y=45.0,Z=40.0)" — to write the inner field and leave the rest be.
    Registry.Register(TEXT("camerarig.set_parameter_default"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UCameraRigAsset* Rig = LoadRig(Params, OutError); if (!Rig) return nullptr;

            FString ParameterName, Value;
            if (!RequireString(Params, TEXT("parameter_name"), ParameterName, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("value"), Value, OutError)) return nullptr;

            FString MemberName;
            Params->TryGetStringField(TEXT("member"), MemberName);

            FInstancedPropertyBag& Bag = Rig->GetDefaultParameters();
            const FPropertyBagPropertyDesc* Desc = Bag.FindPropertyDescByName(FName(*ParameterName));
            if (!Desc || !Desc->CachedProperty)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(
                    TEXT("No parameter '%s' in the rig's defaults. camerarig.list_parameters shows what is there; a freshly renamed parameter needs camerarig.build first."),
                    *ParameterName);
                return nullptr;
            }

            FStructView BagView = Bag.GetMutableValue();
            uint8* BagMemory = BagView.GetMemory();
            if (!BagMemory)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("The rig's default-parameter bag has no storage — run camerarig.build first");
                return nullptr;
            }

            FProperty* Prop = const_cast<FProperty*>(Desc->CachedProperty);
            void* Addr = Prop->ContainerPtrToValuePtr<void>(BagMemory);

            if (!MemberName.IsEmpty())
            {
                FStructProperty* StructProp = CastField<FStructProperty>(Prop);
                if (!StructProp)
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = FString::Printf(TEXT("Parameter '%s' is a %s, not a struct, so 'member' does not apply"),
                        *ParameterName, *Prop->GetCPPType());
                    return nullptr;
                }
                FProperty* MemberProp = FindFProperty<FProperty>(StructProp->Struct, *MemberName);
                if (!MemberProp)
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = FString::Printf(TEXT("Member '%s' not found on %s"), *MemberName, *StructProp->Struct->GetName());
                    return nullptr;
                }
                Addr = MemberProp->ContainerPtrToValuePtr<void>(Addr);
                Prop = MemberProp;
            }

            FString OldValue;
            Prop->ExportText_Direct(OldValue, Addr, /*DefaultValue=*/nullptr, Rig, PPF_None);

            Rig->Modify();
            const TCHAR* ImportResult = Prop->ImportText_Direct(*Value, Addr, Rig, PPF_None);
            if (ImportResult == nullptr)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("Failed to parse value '%s' for parameter '%s' (type %s)"),
                    *Value, *ParameterName, *Prop->GetCPPType());
                return nullptr;
            }

            FString NewValue;
            Prop->ExportText_Direct(NewValue, Addr, /*DefaultValue=*/nullptr, Rig, PPF_None);

            Rig->DirtyBuildStatus();
            Rig->MarkPackageDirty();

            auto Out = MakeShared<FJsonObject>();
            Out->SetBoolField(TEXT("ok"), true);
            Out->SetStringField(TEXT("parameter"), ParameterName);
            Out->SetStringField(TEXT("old_value"), OldValue);
            Out->SetStringField(TEXT("new_value"), NewValue);
            return Out;
        });

    // camerarig.add_node — creates a camera node under the rig.
    // Params: rig_path, node_class (short class name),
    //         parent_node (optional node name), parent_property (optional, default "Children"),
    //         index (optional insert position in the parent array, default = append).
    // Returns { ok, node, node_class }.
    //
    // A node with no parent is created "loose": it exists in the asset and shows in the rig
    // editor unconnected, exactly as if it had been dragged into the graph but not wired.
    Registry.Register(TEXT("camerarig.add_node"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UCameraRigAsset* Rig = LoadRig(Params, OutError); if (!Rig) return nullptr;

            FString NodeClassName;
            if (!RequireString(Params, TEXT("node_class"), NodeClassName, OutError)) return nullptr;

            UClass* NodeClass = RigFindNodeClass(NodeClassName);
            if (!NodeClass)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("No concrete UCameraNode subclass named '%s'"), *NodeClassName);
                return nullptr;
            }

            Rig->Modify();
            UCameraNode* NewNode = NewObject<UCameraNode>(Rig, NodeClass, NAME_None, RF_Transactional);
            if (!NewNode)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = FString::Printf(TEXT("NewObject failed for %s"), *NodeClassName);
                return nullptr;
            }

            // Register with the rig's node graph, which is what keeps a node alive while it is
            // not referenced by anything — without this a loose node is collected on the next GC.
            if (IObjectTreeGraphRootObject* GraphRoot = Cast<IObjectTreeGraphRootObject>(Rig))
            {
                GraphRoot->AddConnectableObject(UCameraRigAsset::NodeTreeGraphName, NewNode);
            }

            FString ParentName;
            if (Params->TryGetStringField(TEXT("parent_node"), ParentName) && !ParentName.IsEmpty())
            {
                TArray<UCameraNode*> Nodes;
                CollectNodes(Rig, Nodes);
                UCameraNode* Parent = nullptr;
                for (UCameraNode* Node : Nodes)
                {
                    if (Node->GetName().Equals(ParentName, ESearchCase::IgnoreCase)) { Parent = Node; break; }
                }
                if (!Parent)
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = FString::Printf(TEXT("No node named '%s' to parent under (the new node was created loose)"), *ParentName);
                    return nullptr;
                }

                FString ParentProperty = TEXT("Children");
                Params->TryGetStringField(TEXT("parent_property"), ParentProperty);

                FProperty* Prop = FindFProperty<FProperty>(Parent->GetClass(), *ParentProperty);
                FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
                if (!ArrayProp || !CastField<FObjectPropertyBase>(ArrayProp->Inner))
                {
                    // Single-pointer slots (a Boom Arm's InputSlot, say) are handled here too.
                    FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop);
                    if (!ObjProp)
                    {
                        OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                        OutError.Message = FString::Printf(TEXT("'%s' on %s is not a camera-node slot or array"),
                            *ParentProperty, *Parent->GetClass()->GetName());
                        return nullptr;
                    }
                    Parent->Modify();
                    ObjProp->SetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(Parent), NewNode);
                }
                else
                {
                    Parent->Modify();
                    FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(Parent));

                    double IndexD = -1; Params->TryGetNumberField(TEXT("index"), IndexD);
                    int32 InsertAt = static_cast<int32>(IndexD);
                    if (InsertAt < 0 || InsertAt > Helper.Num()) { InsertAt = Helper.Num(); }

                    Helper.InsertValues(InsertAt, 1);
                    CastField<FObjectPropertyBase>(ArrayProp->Inner)
                        ->SetObjectPropertyValue(Helper.GetRawPtr(InsertAt), NewNode);
                }
            }

            Rig->DirtyBuildStatus();
            Rig->MarkPackageDirty();

            auto Out = MakeShared<FJsonObject>();
            Out->SetBoolField(TEXT("ok"), true);
            Out->SetStringField(TEXT("node"), NewNode->GetName());
            Out->SetStringField(TEXT("node_class"), NewNode->GetClass()->GetName());
            return Out;
        });

    // camerarig.remove_node — deletes a camera node from the rig.
    // Params: rig_path, and either node_name or node_class (+ node_index).
    // Returns { ok, removed, cleared_references: [...] }.
    //
    // Deletes the way the rig editor does: every reference to the node is cleared (array entries
    // are removed, not left as null holes), the node is dropped from the rig's node-graph object
    // list, and the object itself is moved to the transient package so it cannot be saved back.
    // Clearing the references alone would leave it listed in the graph as a loose node.
    Registry.Register(TEXT("camerarig.remove_node"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UCameraRigAsset* Rig = LoadRig(Params, OutError); if (!Rig) return nullptr;

            UCameraNode* Target = RigResolveTargetNode(Params, Rig, OutError);
            if (!Target) return nullptr;

            const FString RemovedName = Target->GetName();
            const FString RemovedClass = Target->GetClass()->GetName();

            TArray<FString> Cleared;

            // Every other node in the rig, plus the rig itself (RootNode, AllNodeTreeObjects).
            TArray<UCameraNode*> Nodes;
            CollectNodes(Rig, Nodes);
            for (UCameraNode* Node : Nodes)
            {
                if (Node == Target) { continue; }
                RigClearRefsIn(Node, Node->GetClass(), Node, Target, Cleared, FString());
            }
            RigClearRefsIn(Rig, Rig->GetClass(), Rig, Target, Cleared, FString());

            // AllNodeTreeObjects is private to the rig, so the reflective sweep above already
            // emptied it of this node; this call is what tells the graph editor about it.
            if (IObjectTreeGraphRootObject* GraphRoot = Cast<IObjectTreeGraphRootObject>(Rig))
            {
                GraphRoot->RemoveConnectableObject(UCameraRigAsset::NodeTreeGraphName, Target);
            }

            // Out of the asset's package so a later save cannot resurrect it. Not MarkAsGarbage:
            // a stale pointer somewhere would then be a dangling one instead of merely orphaned.
            Target->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);

            Rig->DirtyBuildStatus();
            Rig->MarkPackageDirty();

            TArray<TSharedPtr<FJsonValue>> ClearedJson;
            for (const FString& Ref : Cleared)
            {
                ClearedJson.Add(MakeShared<FJsonValueString>(Ref));
            }

            auto Out = MakeShared<FJsonObject>();
            Out->SetBoolField(TEXT("ok"), true);
            Out->SetStringField(TEXT("removed"), RemovedName);
            Out->SetStringField(TEXT("removed_class"), RemovedClass);
            Out->SetArrayField(TEXT("cleared_references"), ClearedJson);
            return Out;
        });

    // camerarig.set_node_children — rewrites a node's child list by node name.
    // Params: rig_path, node_name (or node_class + node_index), children (array of node names,
    //         "None" for an empty slot), property (optional, default "Children").
    // Returns { ok, node, children }.
    //
    // The order of this array is the evaluation order, which is the thing that decides what a
    // rig actually does — a node that sets the camera location means one thing ahead of a boom
    // arm and something else entirely after it.
    Registry.Register(TEXT("camerarig.set_node_children"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UCameraRigAsset* Rig = LoadRig(Params, OutError); if (!Rig) return nullptr;

            UCameraNode* Target = RigResolveTargetNode(Params, Rig, OutError);
            if (!Target) return nullptr;

            const TArray<TSharedPtr<FJsonValue>>* ChildrenJson = nullptr;
            if (!Params->TryGetArrayField(TEXT("children"), ChildrenJson) || ChildrenJson == nullptr)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("children is required and must be an array of node names");
                return nullptr;
            }

            FString PropertyName = TEXT("Children");
            Params->TryGetStringField(TEXT("property"), PropertyName);

            FProperty* Prop = FindFProperty<FProperty>(Target->GetClass(), *PropertyName);
            FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
            FObjectPropertyBase* InnerObjProp = ArrayProp ? CastField<FObjectPropertyBase>(ArrayProp->Inner) : nullptr;
            if (!InnerObjProp)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("'%s' on %s is not an array of camera nodes"),
                    *PropertyName, *Target->GetClass()->GetName());
                return nullptr;
            }

            TArray<UCameraNode*> Nodes;
            CollectNodes(Rig, Nodes);

            // Resolve every name before touching the array, so a typo cannot leave it half written.
            TArray<UCameraNode*> Resolved;
            for (const TSharedPtr<FJsonValue>& Entry : *ChildrenJson)
            {
                const FString Name = Entry.IsValid() ? Entry->AsString() : FString();
                if (Name.IsEmpty() || Name.Equals(TEXT("None"), ESearchCase::IgnoreCase))
                {
                    Resolved.Add(nullptr);
                    continue;
                }
                UCameraNode* Found = nullptr;
                for (UCameraNode* Node : Nodes)
                {
                    if (Node->GetName().Equals(Name, ESearchCase::IgnoreCase)) { Found = Node; break; }
                }
                if (!Found)
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = FString::Printf(TEXT("No node named '%s' in rig — nothing was changed"), *Name);
                    return nullptr;
                }
                Resolved.Add(Found);
            }

            Target->Modify();
            FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(Target));
            Helper.Resize(Resolved.Num());
            for (int32 Index = 0; Index < Resolved.Num(); ++Index)
            {
                InnerObjProp->SetObjectPropertyValue(Helper.GetRawPtr(Index), Resolved[Index]);
            }

            Rig->DirtyBuildStatus();
            Rig->MarkPackageDirty();

            TArray<TSharedPtr<FJsonValue>> ResultChildren;
            for (UCameraNode* Node : Resolved)
            {
                ResultChildren.Add(MakeShared<FJsonValueString>(Node ? Node->GetName() : TEXT("None")));
            }

            auto Out = MakeShared<FJsonObject>();
            Out->SetBoolField(TEXT("ok"), true);
            Out->SetStringField(TEXT("node"), Target->GetName());
            Out->SetArrayField(TEXT("children"), ResultChildren);
            return Out;
        });
}
