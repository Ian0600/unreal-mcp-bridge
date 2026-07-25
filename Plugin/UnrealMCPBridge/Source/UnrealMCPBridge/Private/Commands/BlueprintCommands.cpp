#include "Commands/BlueprintCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "MCPGraphEditLibrary.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/Interface.h"

namespace
{
    // Loads the Blueprint named by the "blueprint_path" param, or sets OutError and returns null.
    UBlueprint* LoadBP(const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        FString Path;
        if (!Params.IsValid() || !Params->TryGetStringField(TEXT("blueprint_path"), Path) || Path.IsEmpty())
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = TEXT("blueprint_path is required and must be non-empty");
            return nullptr;
        }
        UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Path);
        if (!BP)
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("Could not load Blueprint: %s"), *Path);
        }
        return BP;
    }

    bool ReqStr(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FString& Out, MCPProtocol::FMCPError& OutError)
    {
        if (!Params->TryGetStringField(Field, Out) || Out.IsEmpty())
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("%s is required and must be non-empty"), Field);
            return false;
        }
        return true;
    }

    int32 GetIntOr(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, int32 Fallback)
    {
        double Val = 0.0;
        return Params->TryGetNumberField(Field, Val) ? static_cast<int32>(Val) : Fallback;
    }

    TSharedPtr<FJsonObject> OkResult()
    {
        auto R = MakeShared<FJsonObject>();
        R->SetBoolField(TEXT("ok"), true);
        return R;
    }

    // For the graph-node adders that return a node name ("" = failure).
    TSharedPtr<FJsonObject> NodeNameResult(const FString& NodeName, MCPProtocol::FMCPError& OutError)
    {
        if (NodeName.IsEmpty())
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = TEXT("operation failed (check blueprint_path/graph_name and args; see editor log)");
            return nullptr;
        }
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("node_name"), NodeName);
        return R;
    }

    // Human-readable type string from a pin type (mirrors blueprint.list_variables), with a trailing
    // "[]" for array containers.
    FString PinTypeToString(const FEdGraphPinType& PinType)
    {
        FString TypeStr;
        if (PinType.PinSubCategoryObject.IsValid())      { TypeStr = PinType.PinSubCategoryObject->GetName(); }
        else if (!PinType.PinSubCategory.IsNone())       { TypeStr = PinType.PinSubCategory.ToString(); }
        else                                             { TypeStr = PinType.PinCategory.ToString(); }
        if (PinType.ContainerType == EPinContainerType::Array) { TypeStr += TEXT("[]"); }
        return TypeStr;
    }
}

void FBlueprintCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    // blueprint.create — creates a new Blueprint asset under the given package path
    Registry.Register(TEXT("blueprint.create"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            if (!Params.IsValid())
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("params object is required");
                return nullptr;
            }

            FString PackagePath;
            FString AssetName;
            FString ParentClassPath;

            if (!Params->TryGetStringField(TEXT("package_path"), PackagePath) || PackagePath.IsEmpty())
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("package_path is required and must be non-empty");
                return nullptr;
            }
            if (!Params->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("asset_name is required and must be non-empty");
                return nullptr;
            }
            // blueprint_type (optional, default "normal"): normal | const | interface.
            FString BlueprintTypeStr = TEXT("normal");
            Params->TryGetStringField(TEXT("blueprint_type"), BlueprintTypeStr);
            const FString BT = BlueprintTypeStr.ToLower();

            EBlueprintType BPType = BPTYPE_Normal;
            UClass* ParentClass = nullptr;

            if (BT == TEXT("interface"))
            {
                // Blueprint Interfaces always derive from UInterface; parent_class_path is ignored.
                BPType = BPTYPE_Interface;
                ParentClass = UInterface::StaticClass();
            }
            else
            {
                if (BT == TEXT("const")) { BPType = BPTYPE_Const; }
                if (!Params->TryGetStringField(TEXT("parent_class_path"), ParentClassPath) || ParentClassPath.IsEmpty())
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = TEXT("parent_class_path is required (except for blueprint_type=interface)");
                    return nullptr;
                }
                ParentClass = LoadObject<UClass>(nullptr, *ParentClassPath);
                if (!ParentClass)
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = FString::Printf(TEXT("Could not load parent class: %s"), *ParentClassPath);
                    return nullptr;
                }
            }

            // Build the full package name e.g. /Game/Blueprints/BP_MyActor
            const FString FullPackageName = PackagePath / AssetName;
            UPackage* Package = CreatePackage(*FullPackageName);
            if (!Package)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = FString::Printf(TEXT("Could not create package: %s"), *FullPackageName);
                return nullptr;
            }
            Package->FullyLoad();

            UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
                ParentClass, Package, FName(*AssetName), BPType);

            if (!NewBP)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("FKismetEditorUtilities::CreateBlueprint returned null");
                return nullptr;
            }

            // Notify asset registry of the new asset
            FAssetRegistryModule::AssetCreated(NewBP);
            Package->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("blueprint_path"), NewBP->GetPathName());
            return Result;
        });

    // blueprint.compile — compiles a Blueprint asset by object path
    Registry.Register(TEXT("blueprint.compile"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
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

            UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
            if (!BP)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("Could not load Blueprint: %s"), *BlueprintPath);
                return nullptr;
            }

            FKismetEditorUtilities::CompileBlueprint(BP);

            const bool  bSuccess     = (BP->Status != BS_Error);
            const int32 ErrorCount   = bSuccess ? 0 : 1;
            // Warning count is not exposed by the public Blueprint status API without KismetCompiler internals.
            const int32 WarningCount = 0;

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("success"), bSuccess);
            Result->SetNumberField(TEXT("error_count"), static_cast<double>(ErrorCount));
            Result->SetNumberField(TEXT("warning_count"), static_cast<double>(WarningCount));
            return Result;
        });

    // blueprint.list_variables — lists NewVariables from a Blueprint
    Registry.Register(TEXT("blueprint.list_variables"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
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

            UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
            if (!BP)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("Could not load Blueprint: %s"), *BlueprintPath);
                return nullptr;
            }

            TArray<TSharedPtr<FJsonValue>> VarArray;
            for (const FBPVariableDescription& VarDesc : BP->NewVariables)
            {
                // Build a human-readable type string from the pin type
                FString TypeStr;
                const FEdGraphPinType& PinType = VarDesc.VarType;
                if (PinType.PinSubCategoryObject.IsValid())
                {
                    TypeStr = PinType.PinSubCategoryObject->GetName();
                }
                else if (!PinType.PinSubCategory.IsNone())
                {
                    TypeStr = PinType.PinSubCategory.ToString();
                }
                else
                {
                    TypeStr = PinType.PinCategory.ToString();
                }

                VarArray.Add(MakeShared<FJsonValueObject>(
                    UnrealMCPBridge::Json::MakeVariableJson(
                        VarDesc.VarName.ToString(),
                        TypeStr,
                        VarDesc.Category.ToString())));
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("variables"), VarArray);
            return Result;
        });

    // blueprint.get_graph_nodes — dumps nodes & pin connections from BP's ubergraph / function / macro graphs.
    // Params: blueprint_path (string, required), graph_name (string, optional — filters by graph name).
    Registry.Register(TEXT("blueprint.get_graph_nodes"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
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

            FString GraphNameFilter;
            Params->TryGetStringField(TEXT("graph_name"), GraphNameFilter);

            UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
            if (!BP)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("Could not load Blueprint: %s"), *BlueprintPath);
                return nullptr;
            }

            auto BuildPinJson = [](UEdGraphPin* Pin) -> TSharedPtr<FJsonObject>
            {
                TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
                PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
                PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
                PinObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
                if (!Pin->PinType.PinSubCategory.IsNone())
                {
                    PinObj->SetStringField(TEXT("subtype"), Pin->PinType.PinSubCategory.ToString());
                }
                if (Pin->PinType.PinSubCategoryObject.IsValid())
                {
                    PinObj->SetStringField(TEXT("subtype_object"), Pin->PinType.PinSubCategoryObject->GetName());
                }
                if (!Pin->DefaultValue.IsEmpty())
                {
                    PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
                }

                TArray<TSharedPtr<FJsonValue>> Linked;
                for (UEdGraphPin* L : Pin->LinkedTo)
                {
                    if (!L) continue;
                    UEdGraphNode* LNode = L->GetOwningNode();
                    const FString Ref = FString::Printf(TEXT("%s:%s"),
                        LNode ? *LNode->GetName() : TEXT(""),
                        *L->PinName.ToString());
                    Linked.Add(MakeShared<FJsonValueString>(Ref));
                }
                PinObj->SetArrayField(TEXT("connected_to"), Linked);
                return PinObj;
            };

            auto BuildNodeJson = [&BuildPinJson](UEdGraphNode* Node) -> TSharedPtr<FJsonObject>
            {
                TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
                NodeObj->SetStringField(TEXT("name"), Node->GetName());
                NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
                NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
                NodeObj->SetNumberField(TEXT("pos_x"), Node->NodePosX);
                NodeObj->SetNumberField(TEXT("pos_y"), Node->NodePosY);

                TArray<TSharedPtr<FJsonValue>> Pins;
                for (UEdGraphPin* Pin : Node->Pins)
                {
                    if (!Pin) continue;
                    Pins.Add(MakeShared<FJsonValueObject>(BuildPinJson(Pin)));
                }
                NodeObj->SetArrayField(TEXT("pins"), Pins);
                return NodeObj;
            };

            auto BuildGraphJson = [&BuildNodeJson](UEdGraph* Graph, const FString& Type) -> TSharedPtr<FJsonObject>
            {
                TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
                GraphObj->SetStringField(TEXT("name"), Graph->GetName());
                GraphObj->SetStringField(TEXT("type"), Type);

                TArray<TSharedPtr<FJsonValue>> Nodes;
                for (UEdGraphNode* Node : Graph->Nodes)
                {
                    if (!Node) continue;
                    Nodes.Add(MakeShared<FJsonValueObject>(BuildNodeJson(Node)));
                }
                GraphObj->SetArrayField(TEXT("nodes"), Nodes);
                return GraphObj;
            };

            TArray<TSharedPtr<FJsonValue>> AllGraphs;
            auto AddGraphs = [&](const TArray<UEdGraph*>& Graphs, const FString& Type)
            {
                for (UEdGraph* G : Graphs)
                {
                    if (!G) continue;
                    if (!GraphNameFilter.IsEmpty() && G->GetName() != GraphNameFilter) continue;
                    AllGraphs.Add(MakeShared<FJsonValueObject>(BuildGraphJson(G, Type)));
                }
            };

            AddGraphs(BP->UbergraphPages, TEXT("ubergraph"));
            AddGraphs(BP->FunctionGraphs, TEXT("function"));
            AddGraphs(BP->MacroGraphs, TEXT("macro"));

            // Not found among top-level graphs — for AnimBlueprints, GraphNameFilter may name a
            // nested graph (state machine inner graph, state bound graph, transition rule graph).
            if (AllGraphs.Num() == 0 && !GraphNameFilter.IsEmpty())
            {
                if (UEdGraph* Nested = UMCPGraphEditLibrary::FindGraphByName(BP, GraphNameFilter))
                {
                    AllGraphs.Add(MakeShared<FJsonValueObject>(BuildGraphJson(Nested, TEXT("nested"))));
                }
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("graphs"), AllGraphs);
            return Result;
        });

    // ---- authoring: structure ----

    // blueprint.add_variable — Params: blueprint_path, var_name, type, [sub_type_path], [is_array], [default_value].
    Registry.Register(TEXT("blueprint.add_variable"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBlueprint* BP = LoadBP(Params, OutError); if (!BP) return nullptr;
            FString VarName, TypeString;
            if (!ReqStr(Params, TEXT("var_name"), VarName, OutError)) return nullptr;
            if (!ReqStr(Params, TEXT("type"), TypeString, OutError)) return nullptr;
            FString SubType, DefaultValue; bool bIsArray = false;
            Params->TryGetStringField(TEXT("sub_type_path"), SubType);
            Params->TryGetStringField(TEXT("default_value"), DefaultValue);
            Params->TryGetBoolField(TEXT("is_array"), bIsArray);
            if (!UMCPGraphEditLibrary::AddMemberVariable(BP, VarName, TypeString, SubType, bIsArray, DefaultValue))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("add_variable failed (unrecognized type '%s'?)"), *TypeString);
                return nullptr;
            }
            return OkResult();
        });

    // blueprint.add_function — Params: blueprint_path, function_name. Returns { graph_name }.
    Registry.Register(TEXT("blueprint.add_function"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBlueprint* BP = LoadBP(Params, OutError); if (!BP) return nullptr;
            FString FunctionName;
            if (!ReqStr(Params, TEXT("function_name"), FunctionName, OutError)) return nullptr;
            const FString GraphName = UMCPGraphEditLibrary::AddFunctionGraph(BP, FunctionName);
            if (GraphName.IsEmpty())
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("add_function failed");
                return nullptr;
            }
            auto R = MakeShared<FJsonObject>();
            R->SetStringField(TEXT("graph_name"), GraphName);
            return R;
        });

    // blueprint.add_component — Params: blueprint_path, component_class_path, component_name, [parent_component].
    // Returns { component_name }.
    Registry.Register(TEXT("blueprint.add_component"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBlueprint* BP = LoadBP(Params, OutError); if (!BP) return nullptr;
            FString ClassPath, CompName;
            if (!ReqStr(Params, TEXT("component_class_path"), ClassPath, OutError)) return nullptr;
            if (!ReqStr(Params, TEXT("component_name"), CompName, OutError)) return nullptr;
            FString Parent;
            Params->TryGetStringField(TEXT("parent_component"), Parent);
            const FString Made = UMCPGraphEditLibrary::AddComponent(BP, ClassPath, CompName, Parent);
            if (Made.IsEmpty())
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("add_component failed (is '%s' a UActorComponent, and does the BP have a construction script?)"), *ClassPath);
                return nullptr;
            }
            auto R = MakeShared<FJsonObject>();
            R->SetStringField(TEXT("component_name"), Made);
            return R;
        });

    // blueprint.set_class_default — Params: blueprint_path, property, value (Unreal import text).
    Registry.Register(TEXT("blueprint.set_class_default"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBlueprint* BP = LoadBP(Params, OutError); if (!BP) return nullptr;
            FString PropName, Value;
            if (!ReqStr(Params, TEXT("property"), PropName, OutError)) return nullptr;
            if (!Params->TryGetStringField(TEXT("value"), Value))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams; OutError.Message = TEXT("value is required"); return nullptr;
            }
            if (!UMCPGraphEditLibrary::SetClassDefaultProperty(BP, PropName, Value))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("set_class_default failed (property '%s' not found or value invalid)"), *PropName);
                return nullptr;
            }
            return OkResult();
        });

    // ---- authoring: graph nodes ----

    // blueprint.add_node — generic K2 node by class. Params: blueprint_path, graph_name, node_class_path, [pos_x], [pos_y].
    Registry.Register(TEXT("blueprint.add_node"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBlueprint* BP = LoadBP(Params, OutError); if (!BP) return nullptr;
            FString GraphName, NodeClassPath;
            if (!ReqStr(Params, TEXT("graph_name"), GraphName, OutError)) return nullptr;
            if (!ReqStr(Params, TEXT("node_class_path"), NodeClassPath, OutError)) return nullptr;
            return NodeNameResult(UMCPGraphEditLibrary::AddK2Node(BP, GraphName, NodeClassPath,
                GetIntOr(Params, TEXT("pos_x"), 0), GetIntOr(Params, TEXT("pos_y"), 0)), OutError);
        });

    // blueprint.add_call_function — Params: blueprint_path, graph_name, function_class_path, function_name, [pos_x], [pos_y].
    Registry.Register(TEXT("blueprint.add_call_function"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBlueprint* BP = LoadBP(Params, OutError); if (!BP) return nullptr;
            FString GraphName, FnClassPath, FnName;
            if (!ReqStr(Params, TEXT("graph_name"), GraphName, OutError)) return nullptr;
            if (!ReqStr(Params, TEXT("function_class_path"), FnClassPath, OutError)) return nullptr;
            if (!ReqStr(Params, TEXT("function_name"), FnName, OutError)) return nullptr;
            return NodeNameResult(UMCPGraphEditLibrary::AddCallFunctionNode(BP, GraphName, FnClassPath, FnName,
                GetIntOr(Params, TEXT("pos_x"), 0), GetIntOr(Params, TEXT("pos_y"), 0)), OutError);
        });

    // blueprint.add_branch — K2Node_IfThenElse. Params: blueprint_path, graph_name, [pos_x], [pos_y].
    Registry.Register(TEXT("blueprint.add_branch"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBlueprint* BP = LoadBP(Params, OutError); if (!BP) return nullptr;
            FString GraphName;
            if (!ReqStr(Params, TEXT("graph_name"), GraphName, OutError)) return nullptr;
            return NodeNameResult(UMCPGraphEditLibrary::AddIfThenElseNode(BP, GraphName,
                GetIntOr(Params, TEXT("pos_x"), 0), GetIntOr(Params, TEXT("pos_y"), 0)), OutError);
        });

    // blueprint.add_variable_get — Params: blueprint_path, graph_name, property, [pos_x], [pos_y].
    Registry.Register(TEXT("blueprint.add_variable_get"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBlueprint* BP = LoadBP(Params, OutError); if (!BP) return nullptr;
            FString GraphName, PropName;
            if (!ReqStr(Params, TEXT("graph_name"), GraphName, OutError)) return nullptr;
            if (!ReqStr(Params, TEXT("property"), PropName, OutError)) return nullptr;
            return NodeNameResult(UMCPGraphEditLibrary::AddVariableGetNode(BP, GraphName, PropName,
                GetIntOr(Params, TEXT("pos_x"), 0), GetIntOr(Params, TEXT("pos_y"), 0)), OutError);
        });

    // blueprint.add_variable_set — Params: blueprint_path, graph_name, property, [pos_x], [pos_y].
    Registry.Register(TEXT("blueprint.add_variable_set"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBlueprint* BP = LoadBP(Params, OutError); if (!BP) return nullptr;
            FString GraphName, PropName;
            if (!ReqStr(Params, TEXT("graph_name"), GraphName, OutError)) return nullptr;
            if (!ReqStr(Params, TEXT("property"), PropName, OutError)) return nullptr;
            return NodeNameResult(UMCPGraphEditLibrary::AddVariableSetNode(BP, GraphName, PropName,
                GetIntOr(Params, TEXT("pos_x"), 0), GetIntOr(Params, TEXT("pos_y"), 0)), OutError);
        });

    // blueprint.set_pin_default — Params: blueprint_path, graph_name, node_name, pin_name, value.
    Registry.Register(TEXT("blueprint.set_pin_default"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBlueprint* BP = LoadBP(Params, OutError); if (!BP) return nullptr;
            FString GraphName, NodeName, PinName, Value;
            if (!ReqStr(Params, TEXT("graph_name"), GraphName, OutError)) return nullptr;
            if (!ReqStr(Params, TEXT("node_name"), NodeName, OutError)) return nullptr;
            if (!ReqStr(Params, TEXT("pin_name"), PinName, OutError)) return nullptr;
            if (!Params->TryGetStringField(TEXT("value"), Value))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams; OutError.Message = TEXT("value is required"); return nullptr;
            }
            if (!UMCPGraphEditLibrary::SetPinDefaultValue(BP, GraphName, NodeName, PinName, Value))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("set_pin_default failed (graph/node/pin not found)");
                return nullptr;
            }
            return OkResult();
        });

    // blueprint.connect_pins — Params: blueprint_path, graph_name, src_node, src_pin, dst_node, dst_pin.
    Registry.Register(TEXT("blueprint.connect_pins"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBlueprint* BP = LoadBP(Params, OutError); if (!BP) return nullptr;
            FString GraphName, SrcNode, SrcPin, DstNode, DstPin;
            if (!ReqStr(Params, TEXT("graph_name"), GraphName, OutError)) return nullptr;
            if (!ReqStr(Params, TEXT("src_node"), SrcNode, OutError)) return nullptr;
            if (!ReqStr(Params, TEXT("src_pin"), SrcPin, OutError)) return nullptr;
            if (!ReqStr(Params, TEXT("dst_node"), DstNode, OutError)) return nullptr;
            if (!ReqStr(Params, TEXT("dst_pin"), DstPin, OutError)) return nullptr;
            if (!UMCPGraphEditLibrary::ConnectPins(BP, GraphName, SrcNode, SrcPin, DstNode, DstPin))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("connect_pins refused (see editor log for the schema's reason)");
                return nullptr;
            }
            return OkResult();
        });

    // blueprint.disconnect_pins — Params: blueprint_path, graph_name, node_a, pin_a, node_b, pin_b.
    Registry.Register(TEXT("blueprint.disconnect_pins"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBlueprint* BP = LoadBP(Params, OutError); if (!BP) return nullptr;
            FString GraphName, NodeA, PinA, NodeB, PinB;
            if (!ReqStr(Params, TEXT("graph_name"), GraphName, OutError)) return nullptr;
            if (!ReqStr(Params, TEXT("node_a"), NodeA, OutError)) return nullptr;
            if (!ReqStr(Params, TEXT("pin_a"), PinA, OutError)) return nullptr;
            if (!ReqStr(Params, TEXT("node_b"), NodeB, OutError)) return nullptr;
            if (!ReqStr(Params, TEXT("pin_b"), PinB, OutError)) return nullptr;
            if (!UMCPGraphEditLibrary::DisconnectPinLink(BP, GraphName, NodeA, PinA, NodeB, PinB))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("disconnect_pins failed (link not found)");
                return nullptr;
            }
            return OkResult();
        });

    // blueprint.remove_node — Params: blueprint_path, graph_name, node_name.
    Registry.Register(TEXT("blueprint.remove_node"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBlueprint* BP = LoadBP(Params, OutError); if (!BP) return nullptr;
            FString GraphName, NodeName;
            if (!ReqStr(Params, TEXT("graph_name"), GraphName, OutError)) return nullptr;
            if (!ReqStr(Params, TEXT("node_name"), NodeName, OutError)) return nullptr;
            if (!UMCPGraphEditLibrary::RemoveNode(BP, GraphName, NodeName))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("remove_node failed (graph/node not found)");
                return nullptr;
            }
            return OkResult();
        });

    // ---- authoring: interfaces & function signatures ----

    // blueprint.implement_interface — Params: blueprint_path, interface_class_path.
    Registry.Register(TEXT("blueprint.implement_interface"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBlueprint* BP = LoadBP(Params, OutError); if (!BP) return nullptr;
            FString IfacePath;
            if (!ReqStr(Params, TEXT("interface_class_path"), IfacePath, OutError)) return nullptr;
            if (!UMCPGraphEditLibrary::ImplementInterface(BP, IfacePath))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("implement_interface failed (could not load or add '%s')"), *IfacePath);
                return nullptr;
            }
            return OkResult();
        });

    // blueprint.add_function_param — Params: blueprint_path, function_graph, param_name, type,
    // [sub_type_path], [is_array], [is_output]. Returns { pin_name }.
    Registry.Register(TEXT("blueprint.add_function_param"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBlueprint* BP = LoadBP(Params, OutError); if (!BP) return nullptr;
            FString FuncGraph, ParamName, TypeString;
            if (!ReqStr(Params, TEXT("function_graph"), FuncGraph, OutError)) return nullptr;
            if (!ReqStr(Params, TEXT("param_name"), ParamName, OutError)) return nullptr;
            if (!ReqStr(Params, TEXT("type"), TypeString, OutError)) return nullptr;
            FString SubType; bool bIsArray = false, bIsOutput = false;
            Params->TryGetStringField(TEXT("sub_type_path"), SubType);
            Params->TryGetBoolField(TEXT("is_array"), bIsArray);
            Params->TryGetBoolField(TEXT("is_output"), bIsOutput);
            const FString PinName = UMCPGraphEditLibrary::AddFunctionParam(BP, FuncGraph, ParamName, TypeString, SubType, bIsArray, bIsOutput);
            if (PinName.IsEmpty())
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("add_function_param failed (check function_graph name and type)");
                return nullptr;
            }
            auto R = MakeShared<FJsonObject>();
            R->SetStringField(TEXT("pin_name"), PinName);
            return R;
        });

    // blueprint.list_functions — graph inventory: name, graph_type (ubergraph/function/macro), node_count,
    // and for function graphs the resolved input/output signature. Params: blueprint_path, [graph_name].
    Registry.Register(TEXT("blueprint.list_functions"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UBlueprint* BP = LoadBP(Params, OutError); if (!BP) return nullptr;
            FString GraphNameFilter;
            Params->TryGetStringField(TEXT("graph_name"), GraphNameFilter);

            TArray<TSharedPtr<FJsonValue>> Graphs;
            auto AddGraph = [&](UEdGraph* G, const TCHAR* Type)
            {
                if (!G) return;
                if (!GraphNameFilter.IsEmpty() && G->GetName() != GraphNameFilter) return;

                TSharedPtr<FJsonObject> GObj = MakeShared<FJsonObject>();
                GObj->SetStringField(TEXT("name"), G->GetName());
                GObj->SetStringField(TEXT("graph_type"), Type);
                GObj->SetNumberField(TEXT("node_count"), G->Nodes.Num());

                if (FCString::Strcmp(Type, TEXT("function")) == 0)
                {
                    TArray<TSharedPtr<FJsonValue>> Inputs, Outputs;
                    auto AddParam = [](TArray<TSharedPtr<FJsonValue>>& Out, UEdGraphPin* P)
                    {
                        TSharedPtr<FJsonObject> Param = MakeShared<FJsonObject>();
                        Param->SetStringField(TEXT("name"), P->PinName.ToString());
                        Param->SetStringField(TEXT("type"), PinTypeToString(P->PinType));
                        Out.Add(MakeShared<FJsonValueObject>(Param));
                    };
                    for (UEdGraphNode* N : G->Nodes)
                    {
                        // Function INPUT params are OUTPUT pins on the entry node; OUTPUT params are INPUT
                        // pins on the result node. Skip exec pins.
                        if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(N))
                        {
                            for (UEdGraphPin* P : Entry->Pins)
                            {
                                if (P && P->Direction == EGPD_Output && P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) { AddParam(Inputs, P); }
                            }
                        }
                        else if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(N))
                        {
                            for (UEdGraphPin* P : ResultNode->Pins)
                            {
                                if (P && P->Direction == EGPD_Input && P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) { AddParam(Outputs, P); }
                            }
                        }
                    }
                    GObj->SetArrayField(TEXT("inputs"), Inputs);
                    GObj->SetArrayField(TEXT("outputs"), Outputs);
                }
                Graphs.Add(MakeShared<FJsonValueObject>(GObj));
            };

            for (UEdGraph* G : BP->UbergraphPages) { AddGraph(G, TEXT("ubergraph")); }
            for (UEdGraph* G : BP->FunctionGraphs) { AddGraph(G, TEXT("function")); }
            for (UEdGraph* G : BP->MacroGraphs)    { AddGraph(G, TEXT("macro")); }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("graphs"), Graphs);
            return Result;
        });
}
