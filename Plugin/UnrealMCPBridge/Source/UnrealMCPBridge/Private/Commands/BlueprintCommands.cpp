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
            if (!Params->TryGetStringField(TEXT("parent_class_path"), ParentClassPath) || ParentClassPath.IsEmpty())
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("parent_class_path is required and must be non-empty");
                return nullptr;
            }

            UClass* ParentClass = LoadObject<UClass>(nullptr, *ParentClassPath);
            if (!ParentClass)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("Could not load parent class: %s"), *ParentClassPath);
                return nullptr;
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
                ParentClass, Package, FName(*AssetName), BPTYPE_Normal);

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

    // blueprint.list_functions — inventory of a Blueprint's graphs: name, graph_type, node_count,
    // and for function graphs the resolved input/output signature.
    // Params: blueprint_path (required), graph_name (optional filter).
    //
    // get_graph_nodes answers the same question by dumping every node with every pin and every
    // link, which on a real character Blueprint is orders of magnitude more than "what is in
    // here" needs. This is the cheap first look that says which graph is worth dumping.
    Registry.Register(TEXT("blueprint.list_functions"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString BlueprintPath;
            if (!UnrealMCPBridge::Json::RequireString(Params, TEXT("blueprint_path"), BlueprintPath, OutError))
            {
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

            // Same display rule as blueprint.list_variables, so the two agree on type names.
            auto TypeString = [](const FEdGraphPinType& PinType) -> FString
            {
                if (PinType.PinSubCategoryObject.IsValid()) return PinType.PinSubCategoryObject->GetName();
                if (!PinType.PinSubCategory.IsNone())       return PinType.PinSubCategory.ToString();
                return PinType.PinCategory.ToString();
            };

            // A function's signature lives on its entry/result nodes rather than on the graph, and
            // the directions read backwards: the entry node's OUTPUT pins are the function's inputs,
            // the result node's INPUT pins are its outputs. Exec pins are control flow, not part of
            // the signature, so they are dropped.
            auto CollectParams = [&TypeString](UEdGraphNode* Node, EEdGraphPinDirection Direction)
            {
                TArray<TSharedPtr<FJsonValue>> Params2;
                if (!Node) return Params2;
                for (UEdGraphPin* Pin : Node->Pins)
                {
                    if (!Pin || Pin->Direction != Direction) continue;
                    if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
                    auto PinObj = MakeShared<FJsonObject>();
                    PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
                    PinObj->SetStringField(TEXT("type"), TypeString(Pin->PinType));
                    Params2.Add(MakeShared<FJsonValueObject>(PinObj));
                }
                return Params2;
            };

            TArray<TSharedPtr<FJsonValue>> Graphs;
            // Generic over the container so the interface graphs below (a different array type on
            // FBPInterfaceDescription) go through the same code.
            auto AddGraphs = [&](const auto& InGraphs, const FString& Type)
            {
                for (UEdGraph* G : InGraphs)
                {
                    if (!G) continue;
                    if (!GraphNameFilter.IsEmpty() && G->GetName() != GraphNameFilter) continue;

                    auto GraphObj = MakeShared<FJsonObject>();
                    GraphObj->SetStringField(TEXT("name"), G->GetName());
                    GraphObj->SetStringField(TEXT("graph_type"), Type);
                    GraphObj->SetNumberField(TEXT("node_count"), G->Nodes.Num());

                    UK2Node_FunctionEntry*  Entry  = nullptr;
                    UK2Node_FunctionResult* Result = nullptr;
                    for (UEdGraphNode* N : G->Nodes)
                    {
                        if (!Entry)  Entry  = Cast<UK2Node_FunctionEntry>(N);
                        if (!Result) Result = Cast<UK2Node_FunctionResult>(N);
                    }
                    // Only emitted when the graph actually has them — an event graph has neither,
                    // and an empty array there would read as "takes nothing, returns nothing".
                    if (Entry)  GraphObj->SetArrayField(TEXT("inputs"),  CollectParams(Entry, EGPD_Output));
                    if (Result) GraphObj->SetArrayField(TEXT("outputs"), CollectParams(Result, EGPD_Input));

                    Graphs.Add(MakeShared<FJsonValueObject>(GraphObj));
                }
            };

            AddGraphs(BP->UbergraphPages, TEXT("ubergraph"));
            AddGraphs(BP->FunctionGraphs, TEXT("function"));
            AddGraphs(BP->MacroGraphs,    TEXT("macro"));

            // Implemented interface functions live on ImplementedInterfaces, not FunctionGraphs, so
            // walking the three top-level arrays alone reports them as absent.
            for (const FBPInterfaceDescription& Interface : BP->ImplementedInterfaces)
            {
                AddGraphs(Interface.Graphs, TEXT("interface"));
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("graphs"), Graphs);
            return Result;
        });
}
