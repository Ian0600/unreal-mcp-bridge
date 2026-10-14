#include "Commands/WidgetCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/NamedSlot.h"
#include "Components/NamedSlotInterface.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Extensions/UIComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Templates/WidgetTemplateClass.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditorUtils.h"
#include "WidgetBlueprintOperationUtils.h"
#include "Commands/InspectorInvoke.h"

// Named rather than anonymous namespace on purpose: UBT's unity build merges command .cpp
// files into one translation unit, where same-named helpers in two anonymous namespaces
// collide. See the note in CommandJsonHelpers.h.
namespace UnrealMCPBridge::WidgetCmd
{
    using MCPProtocol::FMCPError;
    using UnrealMCPBridge::Json::RequireString;

    void Fail(FMCPError& OutError, int32 Code, const FString& Message)
    {
        OutError.Code = Code;
        OutError.Message = Message;
    }

    TSharedPtr<FJsonObject> Ok()
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField(TEXT("ok"), true);
        return Out;
    }

    // ---------------------------------------------------------------------------------
    // Resolution helpers
    // ---------------------------------------------------------------------------------

    UWidgetBlueprint* LoadWidgetBlueprint(const TSharedPtr<FJsonObject>& Params, FMCPError& OutError)
    {
        FString Path;
        if (!RequireString(Params, TEXT("widget_path"), Path, OutError))
        {
            return nullptr;
        }
        UWidgetBlueprint* Blueprint = LoadObject<UWidgetBlueprint>(nullptr, *Path);
        if (!Blueprint)
        {
            Fail(OutError, FMCPError::InvalidParams,
                FString::Printf(TEXT("Could not load WidgetBlueprint: %s"), *Path));
            return nullptr;
        }
        if (!Blueprint->WidgetTree)
        {
            Fail(OutError, FMCPError::InternalError,
                FString::Printf(TEXT("WidgetBlueprint has no widget tree: %s"), *Path));
            return nullptr;
        }
        return Blueprint;
    }

    UWidget* FindWidget(UWidgetBlueprint& Blueprint, const FString& WidgetName, FMCPError& OutError)
    {
        UWidget* Widget = Blueprint.WidgetTree->FindWidget(FName(*WidgetName));
        if (!Widget)
        {
            Fail(OutError, FMCPError::InvalidParams,
                FString::Printf(TEXT("widget not found in tree: %s"), *WidgetName));
        }
        return Widget;
    }

    // Reads widget_name and resolves it in one step — the shape almost every command needs.
    UWidget* ResolveWidgetParam(const TSharedPtr<FJsonObject>& Params, UWidgetBlueprint& Blueprint,
        const TCHAR* Field, FMCPError& OutError)
    {
        FString Name;
        if (!RequireString(Params, Field, Name, OutError))
        {
            return nullptr;
        }
        return FindWidget(Blueprint, Name, OutError);
    }

    /**
     * Resolves a widget class from either a native class path ("/Script/UMG.Button") or a
     * Widget Blueprint asset path ("/Game/UI/WBP_Card.WBP_Card"), returning the class that
     * can actually be instantiated. Blueprint paths resolve through GeneratedClass, and the
     * "_C" suffix form is accepted too since that is what class pickers report.
     */
    UClass* ResolveWidgetClass(const FString& ClassPath, FMCPError& OutError)
    {
        if (UClass* Direct = LoadObject<UClass>(nullptr, *ClassPath))
        {
            return Direct;
        }
        if (UBlueprint* AsBlueprint = LoadObject<UBlueprint>(nullptr, *ClassPath))
        {
            if (AsBlueprint->GeneratedClass)
            {
                return AsBlueprint->GeneratedClass;
            }
        }
        if (UClass* WithSuffix = LoadObject<UClass>(nullptr, *(ClassPath + TEXT("_C"))))
        {
            return WithSuffix;
        }
        Fail(OutError, FMCPError::InvalidParams,
            FString::Printf(TEXT("could not resolve class: %s"), *ClassPath));
        return nullptr;
    }

    UClass* ResolveWidgetClassParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FMCPError& OutError)
    {
        FString ClassPath;
        if (!RequireString(Params, Field, ClassPath, OutError))
        {
            return nullptr;
        }
        UClass* Class = ResolveWidgetClass(ClassPath, OutError);
        if (Class && !Class->IsChildOf(UWidget::StaticClass()))
        {
            Fail(OutError, FMCPError::InvalidParams,
                FString::Printf(TEXT("%s is not a UWidget subclass"), *ClassPath));
            return nullptr;
        }
        return Class;
    }

    // ---------------------------------------------------------------------------------
    // Reflection
    // ---------------------------------------------------------------------------------

    /**
     * Generic reflection-based property write. Values arrive as the same text form
     * UnrealEd's property system uses, so structs go in as "(X=10,Y=20)" and enums as
     * their entry name — the identical strings list_properties reports back.
     */
    bool SetPropertyByReflection(UObject* Object, const FString& PropertyName, const FString& Value, FMCPError& OutError)
    {
        FProperty* Property = Object->GetClass()->FindPropertyByName(FName(*PropertyName));
        if (!Property)
        {
            Fail(OutError, FMCPError::InvalidParams,
                FString::Printf(TEXT("property not found on %s: %s"), *Object->GetClass()->GetName(), *PropertyName));
            return false;
        }

        void* Address = Property->ContainerPtrToValuePtr<void>(Object);
        Object->Modify();
        if (Property->ImportText_Direct(*Value, Address, Object, PPF_None) == nullptr)
        {
            Fail(OutError, FMCPError::InvalidParams,
                FString::Printf(TEXT("failed to import value '%s' for property %s"), *Value, *PropertyName));
            return false;
        }

        FPropertyChangedEvent Event(Property);
        Object->PostEditChangeProperty(Event);
        Object->MarkPackageDirty();
        return true;
    }

    // Dumps every editable property on an object as name/type/value triples. Deliberately
    // symmetric with SetPropertyByReflection: the reported value string is accepted back.
    TArray<TSharedPtr<FJsonValue>> DescribeProperties(UObject* Object, bool bEditableOnly)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        if (!Object)
        {
            return Out;
        }

        for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
        {
            FProperty* Property = *It;
            if (bEditableOnly && !Property->HasAnyPropertyFlags(CPF_Edit))
            {
                continue;
            }
            if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient))
            {
                continue;
            }

            FString ValueText;
            Property->ExportText_InContainer(0, ValueText, Object, Object, Object, PPF_None);

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), Property->GetName());
            Entry->SetStringField(TEXT("type"), Property->GetCPPType());
            Entry->SetStringField(TEXT("value"), ValueText);
            Entry->SetStringField(TEXT("category"), Property->GetMetaData(TEXT("Category")));
            Entry->SetBoolField(TEXT("read_only"), Property->HasAnyPropertyFlags(CPF_EditConst));
            Out.Add(MakeShared<FJsonValueObject>(Entry));
        }
        return Out;
    }

    // ---------------------------------------------------------------------------------
    // JSON shaping
    // ---------------------------------------------------------------------------------

    TSharedPtr<FJsonObject> MakeWidgetJson(const UWidget* Widget)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Widget->GetName());
        Obj->SetStringField(TEXT("class"), Widget->GetClass()->GetPathName());
        Obj->SetStringField(TEXT("label"), Widget->GetLabelText().ToString());
        Obj->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);
        Obj->SetBoolField(TEXT("is_panel"), Widget->IsA<UPanelWidget>());

        if (const UPanelSlot* Slot = Widget->Slot)
        {
            Obj->SetStringField(TEXT("slot_class"), Slot->GetClass()->GetPathName());
        }
        if (const UPanelWidget* Parent = Widget->GetParent())
        {
            Obj->SetStringField(TEXT("parent"), Parent->GetName());
        }
        return Obj;
    }

    /**
     * Narrows an object to a named slot host, or returns an empty interface.
     *
     * Only three things in UMG host named slots: UUserWidget (a nested widget blueprint,
     * offering the slots its own tree exposes), UExpandableArea, and UWidgetTree — the tree
     * being how a blueprint fills slots declared by its parent class. UNamedSlot is
     * deliberately not among them: it *declares* a slot for consumers of this blueprint and
     * is an ordinary content widget locally, so its own content is a plain child.
     */
    TScriptInterface<INamedSlotInterface> AsNamedSlotHost(UObject* Object)
    {
        TScriptInterface<INamedSlotInterface> Interface;
        if (Object && Object->GetClass()->ImplementsInterface(UNamedSlotInterface::StaticClass()))
        {
            Interface.SetObject(Object);
            Interface.SetInterface(Cast<INamedSlotInterface>(Object));
        }
        return Interface;
    }

    // Explains the above where a caller is most likely to hit it: pointing a named slot
    // command at the UNamedSlot widget itself is the natural first guess.
    FString DescribeNotASlotHost(const UObject& Object)
    {
        if (Object.IsA<UNamedSlot>())
        {
            return FString::Printf(
                TEXT("'%s' is a NamedSlot widget: it declares a slot for blueprints that consume this one, ")
                TEXT("it does not host slot content itself. Put content in it with widget.add or widget.move, ")
                TEXT("and fill it from the consuming blueprint by naming that nested user widget as host_widget."),
                *Object.GetName());
        }
        return FString::Printf(
            TEXT("'%s' does not host named slots — only nested user widgets, ExpandableArea, ")
            TEXT("and the blueprint itself (omit host_widget) do."),
            *Object.GetName());
    }

    void MarkStructurallyModified(UWidgetBlueprint& Blueprint)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
    }

    // Both operation-utils and editor-utils report failures as FText; funnel them into the
    // JSON-RPC error the same way everywhere.
    bool Check(bool bSucceeded, const FText& ErrorText, const TCHAR* What, FMCPError& OutError)
    {
        if (bSucceeded)
        {
            return true;
        }
        const FString Detail = ErrorText.IsEmpty() ? FString(TEXT("operation refused")) : ErrorText.ToString();
        Fail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("%s failed: %s"), What, *Detail));
        return false;
    }
}

void FWidgetCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    // ---- BEGIN commands forwarded to BlueprintInspector (generated from its function headers) ----
    Registry.Register(TEXT("widget.mvvm_set_binding_mode"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BPIMVVMToolset"), TEXT("SetBindingMode"), { TEXT("widget_path"), TEXT("binding_id"), TEXT("mode") }));
    Registry.Register(TEXT("widget.mvvm_set_binding_execution_mode"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BPIMVVMToolset"), TEXT("SetBindingExecutionMode"), { TEXT("widget_path"), TEXT("binding_id"), TEXT("execution_mode") }));
    Registry.Register(TEXT("widget.mvvm_set_binding_enabled"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BPIMVVMToolset"), TEXT("SetBindingEnabled"), { TEXT("widget_path"), TEXT("binding_id"), TEXT("enabled"), TEXT("compile") }));
    Registry.Register(TEXT("widget.mvvm_update_binding_paths"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BPIMVVMToolset"), TEXT("UpdateBindingPaths"), { TEXT("widget_path"), TEXT("binding_id"), TEXT("source_path"), TEXT("destination_path") }));
    Registry.Register(TEXT("widget.mvvm_set_conversion_function_argument"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BPIMVVMToolset"), TEXT("SetConversionFunctionArgument"), { TEXT("widget_path"), TEXT("binding_id"), TEXT("argument_name"), TEXT("value_or_path"), TEXT("source_to_destination") }));
    Registry.Register(TEXT("widget.mvvm_remove_view_model"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BPIMVVMToolset"), TEXT("RemoveViewModelFromWidget"), { TEXT("widget_path"), TEXT("view_model_name"), TEXT("remove_referencing_bindings") }));
    Registry.Register(TEXT("widget.mvvm_set_view_model_creation_type"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BPIMVVMToolset"), TEXT("SetViewModelCreationType"), { TEXT("widget_path"), TEXT("view_model_name"), TEXT("creation_type"), TEXT("argument") }));
    Registry.Register(TEXT("widget.mvvm_set_view_model_options"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BPIMVVMToolset"), TEXT("SetViewModelOptions"), { TEXT("widget_path"), TEXT("view_model_name"), TEXT("expose_instance_in_editor"), TEXT("create_setter_function"), TEXT("optional") }));
    Registry.Register(TEXT("widget.mvvm_rename_view_model"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BPIMVVMToolset"), TEXT("RenameViewModel"), { TEXT("widget_path"), TEXT("view_model_name"), TEXT("new_name") }));
    Registry.Register(TEXT("widget.mvvm_remove_view_model_property"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BPIMVVMToolset"), TEXT("RemoveViewModelProperty"), { TEXT("view_model_path"), TEXT("property_name") }));
    Registry.Register(TEXT("widget.mvvm_set_view_model_property_default"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BPIMVVMToolset"), TEXT("SetViewModelPropertyDefault"), { TEXT("view_model_path"), TEXT("property_name"), TEXT("value_text") }));
    Registry.Register(TEXT("widget.mvvm_add_view_event"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BPIMVVMToolset"), TEXT("AddViewEvent"), { TEXT("widget_path"), TEXT("event_path"), TEXT("destination_function_path") }));
    Registry.Register(TEXT("widget.mvvm_set_view_event_argument"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BPIMVVMToolset"), TEXT("SetViewEventArgument"), { TEXT("widget_path"), TEXT("event_id"), TEXT("argument_name"), TEXT("value_or_path") }));
    Registry.Register(TEXT("widget.mvvm_remove_view_event"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BPIMVVMToolset"), TEXT("RemoveViewEvent"), { TEXT("widget_path"), TEXT("event_id") }));
    Registry.Register(TEXT("widget.mvvm_add_view_condition"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BPIMVVMToolset"), TEXT("AddViewCondition"), { TEXT("widget_path"), TEXT("condition_path"), TEXT("operation"), TEXT("value"), TEXT("max_value"), TEXT("destination_function_path") }));
    Registry.Register(TEXT("widget.mvvm_remove_view_condition"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BPIMVVMToolset"), TEXT("RemoveViewCondition"), { TEXT("widget_path"), TEXT("condition_id") }));
    Registry.Register(TEXT("widget.mvvm_dump"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BPIMVVMToolset"), TEXT("DumpWidgetMVVM"), { TEXT("widget_path") }));
    Registry.Register(TEXT("widget.mvvm_compile"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BPIMVVMToolset"), TEXT("CompileWidgetMVVM"), { TEXT("widget_path") }));
    // ---- END forwarded commands ----

    using namespace UnrealMCPBridge::WidgetCmd;
    using MCPProtocol::FMCPError;

    // =====================================================================================
    // BLUEPRINT LEVEL
    // =====================================================================================

    // widget.create — new Widget Blueprint. Params: package_path, asset_name,
    // parent_class (default UUserWidget), root_widget_class (optional).
    Registry.Register(TEXT("widget.create"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString PackagePath, AssetName;
            if (!RequireString(Params, TEXT("package_path"), PackagePath, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("asset_name"), AssetName, OutError)) return nullptr;

            UClass* ParentClass = UUserWidget::StaticClass();
            FString ParentClassPath;
            if (Params->TryGetStringField(TEXT("parent_class"), ParentClassPath) && !ParentClassPath.IsEmpty())
            {
                ParentClass = ResolveWidgetClass(ParentClassPath, OutError);
                if (!ParentClass) return nullptr;
                if (!ParentClass->IsChildOf(UUserWidget::StaticClass()))
                {
                    Fail(OutError, FMCPError::InvalidParams,
                        FString::Printf(TEXT("parent_class must derive from UserWidget: %s"), *ParentClassPath));
                    return nullptr;
                }
            }

            UClass* RootWidgetClass = nullptr;
            FString RootClassPath;
            if (Params->TryGetStringField(TEXT("root_widget_class"), RootClassPath) && !RootClassPath.IsEmpty())
            {
                RootWidgetClass = ResolveWidgetClass(RootClassPath, OutError);
                if (!RootWidgetClass) return nullptr;
                if (!RootWidgetClass->IsChildOf(UWidget::StaticClass()))
                {
                    Fail(OutError, FMCPError::InvalidParams,
                        FString::Printf(TEXT("root_widget_class must derive from Widget: %s"), *RootClassPath));
                    return nullptr;
                }
            }

            const FString FullPackageName = PackagePath / AssetName;
            UPackage* Package = CreatePackage(*FullPackageName);
            if (!Package)
            {
                Fail(OutError, FMCPError::InternalError,
                    FString::Printf(TEXT("could not create package: %s"), *FullPackageName));
                return nullptr;
            }

            UWidgetBlueprint* Blueprint = FWidgetBlueprintOperationUtils::CreateWidgetBlueprint(
                Package, FName(*AssetName), BPTYPE_Normal, ParentClass, RootWidgetClass, TEXT("MCPBridge"));
            if (!Blueprint)
            {
                Fail(OutError, FMCPError::InternalError, TEXT("CreateWidgetBlueprint returned null"));
                return nullptr;
            }

            FAssetRegistryModule::AssetCreated(Blueprint);

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("object_path"), Blueprint->GetPathName());
            Result->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
            if (Blueprint->WidgetTree && Blueprint->WidgetTree->RootWidget)
            {
                Result->SetStringField(TEXT("root_widget"), Blueprint->WidgetTree->RootWidget->GetName());
            }
            return Result;
        });

    // widget.get_info — asset-level summary. Params: widget_path.
    Registry.Register(TEXT("widget.get_info"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            TArray<UWidget*> AllWidgets;
            Blueprint->WidgetTree->GetAllWidgets(AllWidgets);

            TArray<FName> InheritedSlots = Blueprint->GetInheritedAvailableNamedSlots();
            TArray<TSharedPtr<FJsonValue>> InheritedSlotsJson;
            for (const FName& SlotName : InheritedSlots)
            {
                InheritedSlotsJson.Add(MakeShared<FJsonValueString>(SlotName.ToString()));
            }

            // Slots this blueprint offers to whoever consumes it: one per UNamedSlot in the
            // tree. Read from the tree rather than the generated class's InstanceNamedSlots so
            // the answer is right before the next compile.
            TArray<TSharedPtr<FJsonValue>> ExposedSlotsJson;
            for (const UWidget* Widget : AllWidgets)
            {
                if (Widget && Widget->IsA<UNamedSlot>())
                {
                    ExposedSlotsJson.Add(MakeShared<FJsonValueString>(Widget->GetName()));
                }
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("widget_blueprint"), Blueprint->GetPathName());
            Result->SetStringField(TEXT("parent_class"),
                Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString());
            Result->SetStringField(TEXT("palette_category"), Blueprint->PaletteCategory);
            Result->SetNumberField(TEXT("widget_count"), AllWidgets.Num());
            Result->SetNumberField(TEXT("animation_count"), Blueprint->Animations.Num());
            Result->SetNumberField(TEXT("binding_count"), Blueprint->Bindings.Num());
            Result->SetBoolField(TEXT("property_bindings_allowed"), Blueprint->ArePropertyBindingsAllowed());
            Result->SetBoolField(TEXT("can_call_initialized_without_player_context"),
                Blueprint->bCanCallInitializedWithoutPlayerContext);
            Result->SetArrayField(TEXT("inherited_named_slots"), InheritedSlotsJson);
            Result->SetArrayField(TEXT("exposed_named_slots"), ExposedSlotsJson);
            if (Blueprint->WidgetTree->RootWidget)
            {
                Result->SetStringField(TEXT("root_widget"), Blueprint->WidgetTree->RootWidget->GetName());
                Result->SetStringField(TEXT("root_widget_class"),
                    Blueprint->WidgetTree->RootWidget->GetClass()->GetPathName());
            }
            return Result;
        });

    // widget.set_property — reflection write on the blueprint asset itself or its CDO.
    // Params: widget_path, property, value, target ("blueprint" | "cdo", default blueprint).
    Registry.Register(TEXT("widget.set_property"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            FString PropertyName, Value;
            if (!RequireString(Params, TEXT("property"), PropertyName, OutError)) return nullptr;
            if (!Params->TryGetStringField(TEXT("value"), Value))
            {
                Fail(OutError, FMCPError::InvalidParams, TEXT("value is required"));
                return nullptr;
            }

            FString Target = TEXT("blueprint");
            Params->TryGetStringField(TEXT("target"), Target);

            UObject* Object = Blueprint;
            if (Target.Equals(TEXT("cdo"), ESearchCase::IgnoreCase))
            {
                Object = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
                if (!Object)
                {
                    Fail(OutError, FMCPError::InvalidParams,
                        TEXT("blueprint has no generated class — compile it before writing CDO properties"));
                    return nullptr;
                }
            }

            if (!SetPropertyByReflection(Object, PropertyName, Value, OutError)) return nullptr;
            MarkStructurallyModified(*Blueprint);
            return Ok();
        });

    // widget.list_blueprint_properties — editable properties on the asset or its CDO.
    // Params: widget_path, target ("blueprint" | "cdo").
    Registry.Register(TEXT("widget.list_blueprint_properties"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            FString Target = TEXT("blueprint");
            Params->TryGetStringField(TEXT("target"), Target);

            UObject* Object = Blueprint;
            if (Target.Equals(TEXT("cdo"), ESearchCase::IgnoreCase))
            {
                Object = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
                if (!Object)
                {
                    Fail(OutError, FMCPError::InvalidParams,
                        TEXT("blueprint has no generated class — compile it first"));
                    return nullptr;
                }
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("target_class"), Object->GetClass()->GetPathName());
            Result->SetArrayField(TEXT("properties"), DescribeProperties(Object, /*bEditableOnly*/ true));
            return Result;
        });

    // widget.set_desired_focus — widget focused when the user widget is activated.
    // Params: widget_path, widget_name (empty clears).
    Registry.Register(TEXT("widget.set_desired_focus"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            FString WidgetName;
            Params->TryGetStringField(TEXT("widget_name"), WidgetName);

            if (!WidgetName.IsEmpty() && !FindWidget(*Blueprint, WidgetName, OutError))
            {
                return nullptr;
            }

            FWidgetBlueprintEditorUtils::SetDesiredFocus(Blueprint, FName(*WidgetName));
            MarkStructurallyModified(*Blueprint);
            return Ok();
        });

    // widget.compile — compiles and reports the resulting status. Params: widget_path.
    Registry.Register(TEXT("widget.compile"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            FKismetEditorUtilities::CompileBlueprint(Blueprint);

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("compiled"), Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
            Result->SetBoolField(TEXT("has_warnings"), Blueprint->Status == BS_UpToDateWithWarnings);
            Result->SetNumberField(TEXT("status"), static_cast<int32>(Blueprint->Status));
            return Result;
        });

    // widget.validate — circular references, inherited name conflicts, slate widget leaks.
    // Params: widget_path.
    Registry.Register(TEXT("widget.validate"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            TArray<TSharedPtr<FJsonValue>> Issues;

            TValueOrError<void, UWidget*> Circular = Blueprint->HasCircularReferences();
            if (Circular.HasError())
            {
                const UWidget* Offender = Circular.GetError();
                Issues.Add(MakeShared<FJsonValueString>(FString::Printf(
                    TEXT("circular reference through widget '%s'"),
                    Offender ? *Offender->GetName() : TEXT("<unknown>"))));
            }

            TValueOrError<void, TSet<UWidget*>> Conflicts = Blueprint->HasConflictingWidgetNamesFromInheritance();
            if (Conflicts.HasError())
            {
                for (const UWidget* Widget : Conflicts.GetError())
                {
                    Issues.Add(MakeShared<FJsonValueString>(FString::Printf(
                        TEXT("widget name '%s' conflicts with an inherited widget"),
                        Widget ? *Widget->GetName() : TEXT("<unknown>"))));
                }
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("valid"), Issues.Num() == 0);
            Result->SetArrayField(TEXT("issues"), Issues);
            return Result;
        });

    // widget.palette_list — widget classes that can be placed. Params: include_blueprints
    // (default true), name_filter (substring, optional).
    Registry.Register(TEXT("widget.palette_list"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            bool bIncludeBlueprints = true;
            if (Params.IsValid())
            {
                Params->TryGetBoolField(TEXT("include_blueprints"), bIncludeBlueprints);
            }
            FString NameFilter;
            if (Params.IsValid())
            {
                Params->TryGetStringField(TEXT("name_filter"), NameFilter);
            }

            auto PassesFilter = [&NameFilter](const FString& Name)
            {
                return NameFilter.IsEmpty() || Name.Contains(NameFilter);
            };

            TArray<TSharedPtr<FJsonValue>> Entries;

            for (TObjectIterator<UClass> It; It; ++It)
            {
                UClass* Class = *It;
                if (!Class->IsChildOf(UWidget::StaticClass()) || Class == UWidget::StaticClass())
                {
                    continue;
                }
                if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
                {
                    continue;
                }
                const bool bIsBlueprintClass = Class->ClassGeneratedBy != nullptr;
                if (bIsBlueprintClass && !bIncludeBlueprints)
                {
                    continue;
                }
                if (!PassesFilter(Class->GetName()))
                {
                    continue;
                }

                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"), Class->GetName());
                Entry->SetStringField(TEXT("class"), Class->GetPathName());
                Entry->SetStringField(TEXT("category"),
                    FWidgetBlueprintEditorUtils::GetPaletteCategory(TSubclassOf<UWidget>(Class)).ToString());
                Entry->SetBoolField(TEXT("is_panel"), Class->IsChildOf(UPanelWidget::StaticClass()));
                Entry->SetBoolField(TEXT("is_user_widget"), Class->IsChildOf(UUserWidget::StaticClass()));
                Entry->SetBoolField(TEXT("is_blueprint"), bIsBlueprintClass);
                Entries.Add(MakeShared<FJsonValueObject>(Entry));
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("count"), Entries.Num());
            Result->SetArrayField(TEXT("classes"), Entries);
            return Result;
        });

    // =====================================================================================
    // WIDGET TREE
    // =====================================================================================

    // widget.tree_list — hierarchy walk. Params: widget_path, start_widget (optional),
    // max_depth (-1 = unlimited).
    Registry.Register(TEXT("widget.tree_list"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            const UWidget* StartWidget = nullptr;
            FString StartName;
            if (Params->TryGetStringField(TEXT("start_widget"), StartName) && !StartName.IsEmpty())
            {
                StartWidget = FindWidget(*Blueprint, StartName, OutError);
                if (!StartWidget) return nullptr;
            }

            int32 MaxDepth = -1;
            if (double DepthValue; Params->TryGetNumberField(TEXT("max_depth"), DepthValue))
            {
                MaxDepth = static_cast<int32>(DepthValue);
            }

            TArray<TSharedPtr<FJsonValue>> Entries;
            FWidgetBlueprintOperationUtils::WalkWidgetTree(Blueprint, StartWidget, MaxDepth,
                [&Entries](const UWidget* Widget, int32 Depth, FName SlotName)
                {
                    if (!Widget)
                    {
                        return;
                    }
                    TSharedPtr<FJsonObject> Entry = MakeWidgetJson(Widget);
                    Entry->SetNumberField(TEXT("depth"), Depth);
                    if (!SlotName.IsNone())
                    {
                        Entry->SetStringField(TEXT("named_slot"), SlotName.ToString());
                    }
                    if (const UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
                    {
                        Entry->SetNumberField(TEXT("child_count"), Panel->GetChildrenCount());
                    }
                    Entries.Add(MakeShared<FJsonValueObject>(Entry));
                });

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("tree_depth"),
                FWidgetBlueprintOperationUtils::ComputeWidgetTreeDepth(Blueprint, StartWidget));
            Result->SetNumberField(TEXT("count"), Entries.Num());
            Result->SetArrayField(TEXT("widgets"), Entries);
            return Result;
        });

    // widget.get_widget — one widget's identity, parent, slot and children.
    // Params: widget_path, widget_name.
    Registry.Register(TEXT("widget.get_widget"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidget* Widget = ResolveWidgetParam(Params, *Blueprint, TEXT("widget_name"), OutError);
            if (!Widget) return nullptr;

            TSharedPtr<FJsonObject> Result = MakeWidgetJson(Widget);
            Result->SetBoolField(TEXT("is_root"), Blueprint->WidgetTree->RootWidget == Widget);

            if (const UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
            {
                TArray<TSharedPtr<FJsonValue>> Children;
                for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
                {
                    if (const UWidget* Child = Panel->GetChildAt(Index))
                    {
                        TSharedPtr<FJsonObject> ChildJson = MakeShared<FJsonObject>();
                        ChildJson->SetNumberField(TEXT("index"), Index);
                        ChildJson->SetStringField(TEXT("name"), Child->GetName());
                        ChildJson->SetStringField(TEXT("class"), Child->GetClass()->GetPathName());
                        Children.Add(MakeShared<FJsonValueObject>(ChildJson));
                    }
                }
                Result->SetArrayField(TEXT("children"), Children);
                Result->SetStringField(TEXT("slot_class_for_children"),
                    Panel->GetSlotClass() ? Panel->GetSlotClass()->GetPathName() : FString());
                Result->SetBoolField(TEXT("can_have_multiple_children"), Panel->CanHaveMultipleChildren());
            }

            if (TScriptInterface<INamedSlotInterface> Host = AsNamedSlotHost(Widget))
            {
                TArray<FName> SlotNames;
                Host->GetSlotNames(SlotNames);
                TArray<TSharedPtr<FJsonValue>> SlotsJson;
                for (const FName& SlotName : SlotNames)
                {
                    TSharedPtr<FJsonObject> SlotJson = MakeShared<FJsonObject>();
                    SlotJson->SetStringField(TEXT("name"), SlotName.ToString());
                    const UWidget* Content = Host->GetContentForSlot(SlotName);
                    SlotJson->SetStringField(TEXT("content"), Content ? Content->GetName() : FString());
                    SlotsJson.Add(MakeShared<FJsonValueObject>(SlotJson));
                }
                Result->SetArrayField(TEXT("named_slots"), SlotsJson);
            }

            return Result;
        });

    // widget.add — instantiate a widget class into the tree. Params: widget_path,
    // widget_class, parent_widget (optional — root when the tree is empty), child_index
    // (-1 appends), widget_name (optional rename after insertion).
    Registry.Register(TEXT("widget.add"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UClass* WidgetClass = ResolveWidgetClassParam(Params, TEXT("widget_class"), OutError);
            if (!WidgetClass) return nullptr;

            UWidget* ParentWidget = nullptr;
            FString ParentName;
            if (Params->TryGetStringField(TEXT("parent_widget"), ParentName) && !ParentName.IsEmpty())
            {
                ParentWidget = FindWidget(*Blueprint, ParentName, OutError);
                if (!ParentWidget) return nullptr;
            }

            int32 ChildIndex = -1;
            if (double IndexValue; Params->TryGetNumberField(TEXT("child_index"), IndexValue))
            {
                ChildIndex = static_cast<int32>(IndexValue);
            }

            FText ErrorText;
            if (!Check(FWidgetBlueprintOperationUtils::CanAddToParent(Blueprint, ParentWidget, ErrorText),
                ErrorText, TEXT("widget.add"), OutError))
            {
                return nullptr;
            }

            // FWidgetTemplateClass is the same path the palette uses: it resolves name
            // collisions against inherited widget trees and calls CreatedFromPalette, which
            // several widgets rely on for their initial designer state.
            // Braced init, not parentheses: the parenthesised form parses as a function
            // declaration taking a TSubclassOf, not a variable.
            FWidgetTemplateClass WidgetTemplate{ TSubclassOf<UWidget>(WidgetClass) };
            UWidget* NewWidget = WidgetTemplate.Create(Blueprint->WidgetTree);
            if (!NewWidget)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("could not construct widget of class %s (abstract or deprecated?)"),
                        *WidgetClass->GetPathName()));
                return nullptr;
            }

            if (!FWidgetBlueprintOperationUtils::AddWidget(Blueprint, NewWidget, ParentWidget, ChildIndex, ErrorText))
            {
                FWidgetBlueprintOperationUtils::RemoveTransientWidgetFromTree(Blueprint, NewWidget);
                Check(false, ErrorText, TEXT("widget.add"), OutError);
                return nullptr;
            }

            FString DesiredName;
            if (Params->TryGetStringField(TEXT("widget_name"), DesiredName) && !DesiredName.IsEmpty())
            {
                FText RenameError;
                if (FWidgetBlueprintOperationUtils::VerifyWidgetRename(Blueprint, NewWidget, FText::FromString(DesiredName), RenameError))
                {
                    FWidgetBlueprintOperationUtils::RenameWidget(Blueprint, NewWidget, DesiredName);
                }
            }

            MarkStructurallyModified(*Blueprint);

            TSharedPtr<FJsonObject> Result = MakeWidgetJson(NewWidget);
            Result->SetBoolField(TEXT("is_root"), Blueprint->WidgetTree->RootWidget == NewWidget);
            return Result;
        });

    // widget.remove — delete a widget and its subtree. Params: widget_path, widget_name.
    Registry.Register(TEXT("widget.remove"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidget* Widget = ResolveWidgetParam(Params, *Blueprint, TEXT("widget_name"), OutError);
            if (!Widget) return nullptr;

            FText ErrorText;
            if (!Check(FWidgetBlueprintOperationUtils::RemoveWidget(Blueprint, Widget, ErrorText),
                ErrorText, TEXT("widget.remove"), OutError))
            {
                return nullptr;
            }
            return Ok();
        });

    // widget.rename — rename a widget and retarget graph references.
    // Params: widget_path, widget_name, new_name.
    Registry.Register(TEXT("widget.rename"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidget* Widget = ResolveWidgetParam(Params, *Blueprint, TEXT("widget_name"), OutError);
            if (!Widget) return nullptr;

            FString NewName;
            if (!RequireString(Params, TEXT("new_name"), NewName, OutError)) return nullptr;

            FText ErrorText;
            if (!Check(FWidgetBlueprintOperationUtils::VerifyWidgetRename(Blueprint, Widget, FText::FromString(NewName), ErrorText),
                ErrorText, TEXT("widget.rename"), OutError))
            {
                return nullptr;
            }
            if (!FWidgetBlueprintOperationUtils::RenameWidget(Blueprint, Widget, NewName))
            {
                Fail(OutError, FMCPError::InternalError, TEXT("widget.rename failed"));
                return nullptr;
            }

            TSharedPtr<FJsonObject> Result = Ok();
            Result->SetStringField(TEXT("name"), Widget->GetName());
            return Result;
        });

    // widget.move — reparent or reorder. Params: widget_path, widget_name, new_parent,
    // child_index (-1 appends).
    Registry.Register(TEXT("widget.move"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidget* Widget = ResolveWidgetParam(Params, *Blueprint, TEXT("widget_name"), OutError);
            if (!Widget) return nullptr;

            UWidget* NewParent = ResolveWidgetParam(Params, *Blueprint, TEXT("new_parent"), OutError);
            if (!NewParent) return nullptr;

            UPanelWidget* NewParentPanel = Cast<UPanelWidget>(NewParent);
            if (!NewParentPanel)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("new_parent '%s' is not a panel widget"), *NewParent->GetName()));
                return nullptr;
            }

            int32 ChildIndex = -1;
            if (double IndexValue; Params->TryGetNumberField(TEXT("child_index"), IndexValue))
            {
                ChildIndex = static_cast<int32>(IndexValue);
            }

            FText ErrorText;
            if (!Check(FWidgetBlueprintOperationUtils::MoveWidget(Blueprint, Widget, NewParentPanel, ChildIndex, ErrorText),
                ErrorText, TEXT("widget.move"), OutError))
            {
                return nullptr;
            }
            return Ok();
        });

    // widget.replace — swap a widget for a different class, keeping references where the
    // new class has matching members. Params: widget_path, widget_name, widget_class.
    Registry.Register(TEXT("widget.replace"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidget* Widget = ResolveWidgetParam(Params, *Blueprint, TEXT("widget_name"), OutError);
            if (!Widget) return nullptr;

            UClass* WidgetClass = ResolveWidgetClassParam(Params, TEXT("widget_class"), OutError);
            if (!WidgetClass) return nullptr;

            FText ErrorText;
            if (!Check(FWidgetBlueprintOperationUtils::ReplaceWidgetWithTemplate(
                    Blueprint, Widget, TSubclassOf<UWidget>(WidgetClass), ErrorText),
                ErrorText, TEXT("widget.replace"), OutError))
            {
                return nullptr;
            }
            return Ok();
        });

    // widget.replace_with_child — collapse a single-child panel into its child.
    // Params: widget_path, widget_name.
    Registry.Register(TEXT("widget.replace_with_child"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidget* Widget = ResolveWidgetParam(Params, *Blueprint, TEXT("widget_name"), OutError);
            if (!Widget) return nullptr;

            FText ErrorText;
            if (!Check(FWidgetBlueprintOperationUtils::ReplaceWidgetWithChild(Blueprint, Widget, ErrorText),
                ErrorText, TEXT("widget.replace_with_child"), OutError))
            {
                return nullptr;
            }
            return Ok();
        });

    // widget.replace_with_named_slot — collapse a named slot host into that slot's content.
    // Params: widget_path, widget_name, slot_name.
    Registry.Register(TEXT("widget.replace_with_named_slot"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidget* Widget = ResolveWidgetParam(Params, *Blueprint, TEXT("widget_name"), OutError);
            if (!Widget) return nullptr;

            FString SlotName;
            if (!RequireString(Params, TEXT("slot_name"), SlotName, OutError)) return nullptr;

            FText ErrorText;
            if (!Check(FWidgetBlueprintOperationUtils::ReplaceWidgetWithNamedSlot(
                    Blueprint, Widget, FName(*SlotName),
                    FWidgetBlueprintEditorUtils::EDeleteWidgetWarningType::DeleteSilently, ErrorText),
                ErrorText, TEXT("widget.replace_with_named_slot"), OutError))
            {
                return nullptr;
            }
            return Ok();
        });

    // widget.wrap — put widgets inside a newly created panel. Params: widget_path,
    // widget_names (array), widget_class (the wrapper panel class).
    Registry.Register(TEXT("widget.wrap"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            const TArray<TSharedPtr<FJsonValue>>* NamesJson = nullptr;
            if (!Params->TryGetArrayField(TEXT("widget_names"), NamesJson) || NamesJson->Num() == 0)
            {
                Fail(OutError, FMCPError::InvalidParams, TEXT("widget_names is required and must be a non-empty array"));
                return nullptr;
            }

            TArray<UWidget*> Widgets;
            for (const TSharedPtr<FJsonValue>& Value : *NamesJson)
            {
                UWidget* Widget = FindWidget(*Blueprint, Value->AsString(), OutError);
                if (!Widget) return nullptr;
                Widgets.Add(Widget);
            }

            UClass* WrapperClass = ResolveWidgetClassParam(Params, TEXT("widget_class"), OutError);
            if (!WrapperClass) return nullptr;
            if (!WrapperClass->IsChildOf(UPanelWidget::StaticClass()))
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("widget_class must be a panel widget: %s"), *WrapperClass->GetPathName()));
                return nullptr;
            }

            TArray<UWidget*> Wrappers = FWidgetBlueprintOperationUtils::WrapWidgets(Blueprint, Widgets, WrapperClass);
            if (Wrappers.Num() == 0)
            {
                Fail(OutError, FMCPError::InvalidParams, TEXT("widget.wrap produced no wrapper widgets"));
                return nullptr;
            }

            TArray<TSharedPtr<FJsonValue>> WrappersJson;
            for (const UWidget* Wrapper : Wrappers)
            {
                if (Wrapper)
                {
                    WrappersJson.Add(MakeShared<FJsonValueObject>(MakeWidgetJson(Wrapper)));
                }
            }

            TSharedPtr<FJsonObject> Result = Ok();
            Result->SetArrayField(TEXT("wrappers"), WrappersJson);
            return Result;
        });

    // widget.set_is_variable — expose (or hide) a widget as a blueprint variable.
    // Params: widget_path, widget_name, is_variable.
    Registry.Register(TEXT("widget.set_is_variable"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidget* Widget = ResolveWidgetParam(Params, *Blueprint, TEXT("widget_name"), OutError);
            if (!Widget) return nullptr;

            bool bIsVariable = true;
            if (!Params->TryGetBoolField(TEXT("is_variable"), bIsVariable))
            {
                Fail(OutError, FMCPError::InvalidParams, TEXT("is_variable is required"));
                return nullptr;
            }

            FWidgetBlueprintOperationUtils::ToggleWidgetAsVariable(Blueprint, Widget, bIsVariable);

            TSharedPtr<FJsonObject> Result = Ok();
            Result->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);
            return Result;
        });

    // widget.set_root — make an existing widget the tree root. Params: widget_path,
    // widget_name (empty clears the root).
    Registry.Register(TEXT("widget.set_root"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            FString WidgetName;
            Params->TryGetStringField(TEXT("widget_name"), WidgetName);

            UWidget* Widget = nullptr;
            if (!WidgetName.IsEmpty())
            {
                Widget = FindWidget(*Blueprint, WidgetName, OutError);
                if (!Widget) return nullptr;

                if (Widget->GetParent() != nullptr)
                {
                    Fail(OutError, FMCPError::InvalidParams,
                        FString::Printf(TEXT("widget '%s' still has a parent — move it out first"), *WidgetName));
                    return nullptr;
                }
            }

            Blueprint->WidgetTree->SetFlags(RF_Transactional);
            Blueprint->WidgetTree->Modify();
            Blueprint->WidgetTree->RootWidget = Widget;
            MarkStructurallyModified(*Blueprint);
            return Ok();
        });

    // widget.duplicate — copy a widget subtree back into the same blueprint via the
    // clipboard text format. Params: widget_path, widget_name, parent_widget (optional —
    // defaults to the source widget's parent).
    Registry.Register(TEXT("widget.duplicate"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidget* Source = ResolveWidgetParam(Params, *Blueprint, TEXT("widget_name"), OutError);
            if (!Source) return nullptr;

            UPanelWidget* TargetParent = Source->GetParent();
            FString ParentName;
            if (Params->TryGetStringField(TEXT("parent_widget"), ParentName) && !ParentName.IsEmpty())
            {
                UWidget* Candidate = FindWidget(*Blueprint, ParentName, OutError);
                if (!Candidate) return nullptr;
                TargetParent = Cast<UPanelWidget>(Candidate);
                if (!TargetParent)
                {
                    Fail(OutError, FMCPError::InvalidParams,
                        FString::Printf(TEXT("parent_widget '%s' is not a panel widget"), *ParentName));
                    return nullptr;
                }
            }
            if (!TargetParent)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    TEXT("no target parent — the source widget is the root, so pass parent_widget explicitly"));
                return nullptr;
            }

            // Export/import round-trip is how the editor's own duplicate works: it clones the
            // whole subtree with slot data intact and assigns fresh unique names.
            FString ExportedText;
            FWidgetBlueprintEditorUtils::ExportWidgetsToText({ Source }, ExportedText);

            TSet<UWidget*> ImportedWidgets;
            TMap<FName, UWidgetSlotPair*> SlotData;
            FWidgetBlueprintEditorUtils::ImportWidgetsFromText(Blueprint, ExportedText, ImportedWidgets, SlotData);

            TArray<TSharedPtr<FJsonValue>> Created;
            for (UWidget* Imported : ImportedWidgets)
            {
                // Only the subtree roots need parenting; children arrive already attached.
                if (Imported->GetParent() != nullptr)
                {
                    continue;
                }
                FText ErrorText;
                if (!FWidgetBlueprintOperationUtils::AddWidget(Blueprint, Imported, TargetParent, -1, ErrorText))
                {
                    FWidgetBlueprintOperationUtils::RemoveTransientWidgetFromTree(Blueprint, Imported);
                    Check(false, ErrorText, TEXT("widget.duplicate"), OutError);
                    return nullptr;
                }
                Created.Add(MakeShared<FJsonValueObject>(MakeWidgetJson(Imported)));
            }

            MarkStructurallyModified(*Blueprint);

            TSharedPtr<FJsonObject> Result = Ok();
            Result->SetArrayField(TEXT("widgets"), Created);
            return Result;
        });

    // =====================================================================================
    // WIDGET AND SLOT PROPERTIES
    // =====================================================================================

    // widget.list_properties — editable properties on one widget. Params: widget_path,
    // widget_name, editable_only (default true).
    Registry.Register(TEXT("widget.list_properties"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidget* Widget = ResolveWidgetParam(Params, *Blueprint, TEXT("widget_name"), OutError);
            if (!Widget) return nullptr;

            bool bEditableOnly = true;
            Params->TryGetBoolField(TEXT("editable_only"), bEditableOnly);

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("widget"), Widget->GetName());
            Result->SetStringField(TEXT("class"), Widget->GetClass()->GetPathName());
            Result->SetArrayField(TEXT("properties"), DescribeProperties(Widget, bEditableOnly));
            return Result;
        });

    // widget.set_widget_property — reflection write on a widget. Params: widget_path,
    // widget_name, property, value.
    Registry.Register(TEXT("widget.set_widget_property"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidget* Widget = ResolveWidgetParam(Params, *Blueprint, TEXT("widget_name"), OutError);
            if (!Widget) return nullptr;

            FString PropertyName, Value;
            if (!RequireString(Params, TEXT("property"), PropertyName, OutError)) return nullptr;
            if (!Params->TryGetStringField(TEXT("value"), Value))
            {
                Fail(OutError, FMCPError::InvalidParams, TEXT("value is required"));
                return nullptr;
            }

            Widget->SetFlags(RF_Transactional);
            if (!SetPropertyByReflection(Widget, PropertyName, Value, OutError)) return nullptr;
            MarkStructurallyModified(*Blueprint);
            return Ok();
        });

    // widget.list_slot_properties — layout properties of a widget's slot in its parent
    // panel. Params: widget_path, widget_name.
    Registry.Register(TEXT("widget.list_slot_properties"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidget* Widget = ResolveWidgetParam(Params, *Blueprint, TEXT("widget_name"), OutError);
            if (!Widget) return nullptr;

            if (!Widget->Slot)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("widget '%s' has no slot — it is the root or lives in a named slot"),
                        *Widget->GetName()));
                return nullptr;
            }

            bool bEditableOnly = true;
            Params->TryGetBoolField(TEXT("editable_only"), bEditableOnly);

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("widget"), Widget->GetName());
            Result->SetStringField(TEXT("slot_class"), Widget->Slot->GetClass()->GetPathName());
            Result->SetArrayField(TEXT("properties"), DescribeProperties(Widget->Slot, bEditableOnly));
            return Result;
        });

    // widget.set_slot_property — reflection write on a widget's slot. Params: widget_path,
    // widget_name, property, value.
    Registry.Register(TEXT("widget.set_slot_property"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidget* Widget = ResolveWidgetParam(Params, *Blueprint, TEXT("widget_name"), OutError);
            if (!Widget) return nullptr;

            if (!Widget->Slot)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("widget '%s' has no slot — it is the root or lives in a named slot"),
                        *Widget->GetName()));
                return nullptr;
            }

            FString PropertyName, Value;
            if (!RequireString(Params, TEXT("property"), PropertyName, OutError)) return nullptr;
            if (!Params->TryGetStringField(TEXT("value"), Value))
            {
                Fail(OutError, FMCPError::InvalidParams, TEXT("value is required"));
                return nullptr;
            }

            Widget->Slot->SetFlags(RF_Transactional);
            if (!SetPropertyByReflection(Widget->Slot, PropertyName, Value, OutError)) return nullptr;

            // Slot edits only reach the live Slate layout through the panel that owns them.
            if (UPanelWidget* Parent = Widget->GetParent())
            {
                Parent->Modify();
            }
            MarkStructurallyModified(*Blueprint);
            return Ok();
        });

    // =====================================================================================
    // NAMED SLOTS
    // =====================================================================================

    // widget.named_slot_list — named slots on a host widget, or on the widget tree itself
    // when host_widget is omitted. Params: widget_path, host_widget (optional).
    Registry.Register(TEXT("widget.named_slot_list"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UObject* HostObject = Blueprint->WidgetTree;
            FString HostName;
            if (Params->TryGetStringField(TEXT("host_widget"), HostName) && !HostName.IsEmpty())
            {
                HostObject = FindWidget(*Blueprint, HostName, OutError);
                if (!HostObject) return nullptr;
            }

            TScriptInterface<INamedSlotInterface> Host = AsNamedSlotHost(HostObject);
            if (!Host)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    DescribeNotASlotHost(*HostObject));
                return nullptr;
            }

            TArray<FName> SlotNames;
            Host->GetSlotNames(SlotNames);

            TArray<TSharedPtr<FJsonValue>> Entries;
            for (const FName& SlotName : SlotNames)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"), SlotName.ToString());
                const UWidget* Content = Host->GetContentForSlot(SlotName);
                Entry->SetStringField(TEXT("content"), Content ? Content->GetName() : FString());
                Entry->SetStringField(TEXT("content_class"),
                    Content ? Content->GetClass()->GetPathName() : FString());
                Entries.Add(MakeShared<FJsonValueObject>(Entry));
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("host"), HostObject->GetName());
            Result->SetArrayField(TEXT("slots"), Entries);
            return Result;
        });

    // widget.named_slot_set_content — place an existing widget into a named slot.
    // Params: widget_path, slot_name, content_widget, host_widget (optional).
    Registry.Register(TEXT("widget.named_slot_set_content"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            FString SlotName;
            if (!RequireString(Params, TEXT("slot_name"), SlotName, OutError)) return nullptr;

            UWidget* Content = ResolveWidgetParam(Params, *Blueprint, TEXT("content_widget"), OutError);
            if (!Content) return nullptr;

            UObject* HostObject = Blueprint->WidgetTree;
            FString HostName;
            if (Params->TryGetStringField(TEXT("host_widget"), HostName) && !HostName.IsEmpty())
            {
                HostObject = FindWidget(*Blueprint, HostName, OutError);
                if (!HostObject) return nullptr;
            }

            TScriptInterface<INamedSlotInterface> Host = AsNamedSlotHost(HostObject);
            if (!Host)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    DescribeNotASlotHost(*HostObject));
                return nullptr;
            }

            TArray<FName> SlotNames;
            Host->GetSlotNames(SlotNames);
            if (!SlotNames.Contains(FName(*SlotName)))
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("'%s' has no named slot '%s'"), *HostObject->GetName(), *SlotName));
                return nullptr;
            }

            // A widget can only live in one place: detach it from a panel parent first, or the
            // tree ends up with the same widget reachable through two routes.
            if (UPanelWidget* CurrentParent = Content->GetParent())
            {
                CurrentParent->Modify();
                CurrentParent->RemoveChild(Content);
            }

            HostObject->SetFlags(RF_Transactional);
            HostObject->Modify();
            Host->SetContentForSlot(FName(*SlotName), Content);
            MarkStructurallyModified(*Blueprint);
            return Ok();
        });

    // widget.named_slot_clear — empty a named slot. Params: widget_path, slot_name,
    // host_widget (optional), delete_content (default false — the widget is left orphaned
    // in the tree otherwise).
    Registry.Register(TEXT("widget.named_slot_clear"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            FString SlotName;
            if (!RequireString(Params, TEXT("slot_name"), SlotName, OutError)) return nullptr;

            UObject* HostObject = Blueprint->WidgetTree;
            FString HostName;
            if (Params->TryGetStringField(TEXT("host_widget"), HostName) && !HostName.IsEmpty())
            {
                HostObject = FindWidget(*Blueprint, HostName, OutError);
                if (!HostObject) return nullptr;
            }

            TScriptInterface<INamedSlotInterface> Host = AsNamedSlotHost(HostObject);
            if (!Host)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    DescribeNotASlotHost(*HostObject));
                return nullptr;
            }

            bool bDeleteContent = false;
            Params->TryGetBoolField(TEXT("delete_content"), bDeleteContent);

            UWidget* Content = Host->GetContentForSlot(FName(*SlotName));

            HostObject->SetFlags(RF_Transactional);
            HostObject->Modify();
            Host->SetContentForSlot(FName(*SlotName), nullptr);

            if (bDeleteContent && Content)
            {
                FText ErrorText;
                FWidgetBlueprintOperationUtils::RemoveWidget(Blueprint, Content, ErrorText);
            }

            MarkStructurallyModified(*Blueprint);

            TSharedPtr<FJsonObject> Result = Ok();
            Result->SetStringField(TEXT("previous_content"), Content ? Content->GetName() : FString());
            return Result;
        });

    // =====================================================================================
    // PROPERTY BINDINGS AND EVENTS
    // =====================================================================================

    // widget.binding_list — property bindings declared on the blueprint. Params: widget_path.
    Registry.Register(TEXT("widget.binding_list"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            TArray<TSharedPtr<FJsonValue>> Entries;
            for (const FDelegateEditorBinding& Binding : Blueprint->Bindings)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("widget_name"), Binding.ObjectName);
                Entry->SetStringField(TEXT("property"), Binding.PropertyName.ToString());
                Entry->SetStringField(TEXT("kind"),
                    Binding.Kind == EBindingKind::Function ? TEXT("function") : TEXT("property"));
                Entry->SetStringField(TEXT("function_name"), Binding.FunctionName.ToString());
                Entry->SetStringField(TEXT("source_property"), Binding.SourceProperty.ToString());
                Entry->SetBoolField(TEXT("target_exists"),
                    const_cast<FDelegateEditorBinding&>(Binding).DoesBindingTargetExist(Blueprint));
                Entries.Add(MakeShared<FJsonValueObject>(Entry));
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("count"), Entries.Num());
            Result->SetArrayField(TEXT("bindings"), Entries);
            return Result;
        });

    // widget.binding_add — bind a widget property to a blueprint function or variable.
    // Params: widget_path, widget_name, property, kind ("function" | "property"),
    // source (function name or variable name on the user widget).
    Registry.Register(TEXT("widget.binding_add"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidget* Widget = ResolveWidgetParam(Params, *Blueprint, TEXT("widget_name"), OutError);
            if (!Widget) return nullptr;

            FString PropertyName, Source;
            if (!RequireString(Params, TEXT("property"), PropertyName, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("source"), Source, OutError)) return nullptr;

            FString Kind = TEXT("function");
            Params->TryGetStringField(TEXT("kind"), Kind);
            const bool bIsFunction = !Kind.Equals(TEXT("property"), ESearchCase::IgnoreCase);

            if (!Widget->GetClass()->FindPropertyByName(FName(*PropertyName)))
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("widget '%s' has no property '%s'"), *Widget->GetName(), *PropertyName));
                return nullptr;
            }

            // The skeleton class is what the editor resolves bindings against — it exists as
            // soon as a member is declared, whereas the generated class only catches up on a
            // full compile. Fall back to the generated class for a freshly loaded asset.
            UClass* SkeletonClass = Blueprint->SkeletonGeneratedClass.Get();
            if (!SkeletonClass)
            {
                SkeletonClass = Blueprint->GeneratedClass.Get();
            }
            if (!SkeletonClass)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    TEXT("blueprint has no generated class — compile it before adding bindings"));
                return nullptr;
            }

            FDelegateEditorBinding Binding;
            Binding.ObjectName = Widget->GetName();
            Binding.PropertyName = FName(*PropertyName);

            if (bIsFunction)
            {
                UFunction* Function = SkeletonClass->FindFunctionByName(FName(*Source));
                if (!Function)
                {
                    Fail(OutError, FMCPError::InvalidParams,
                        FString::Printf(TEXT("no function '%s' on %s"), *Source, *SkeletonClass->GetName()));
                    return nullptr;
                }
                Binding.FunctionName = Function->GetFName();
                Binding.Kind = EBindingKind::Function;
                UBlueprint::GetGuidFromClassByFieldName<UFunction>(
                    Function->GetOwnerClass(), Function->GetFName(), Binding.MemberGuid);
            }
            else
            {
                FProperty* SourceProperty = SkeletonClass->FindPropertyByName(FName(*Source));
                if (!SourceProperty)
                {
                    Fail(OutError, FMCPError::InvalidParams,
                        FString::Printf(TEXT("no property '%s' on %s"), *Source, *SkeletonClass->GetName()));
                    return nullptr;
                }
                Binding.SourceProperty = SourceProperty->GetFName();
                Binding.Kind = EBindingKind::Property;
                UBlueprint::GetGuidFromClassByFieldName<FProperty>(
                    SkeletonClass, SourceProperty->GetFName(), Binding.MemberGuid);
            }

            Blueprint->Modify();
            // Remove-then-add rather than AddUnique alone: equality only compares widget and
            // property, so this is how an existing binding gets overwritten instead of ignored.
            Blueprint->Bindings.Remove(Binding);
            Blueprint->Bindings.AddUnique(Binding);
            MarkStructurallyModified(*Blueprint);
            return Ok();
        });

    // widget.binding_remove — drop a property binding. Params: widget_path, widget_name,
    // property.
    Registry.Register(TEXT("widget.binding_remove"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            FString WidgetName, PropertyName;
            if (!RequireString(Params, TEXT("widget_name"), WidgetName, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("property"), PropertyName, OutError)) return nullptr;

            FDelegateEditorBinding Binding;
            Binding.ObjectName = WidgetName;
            Binding.PropertyName = FName(*PropertyName);

            Blueprint->Modify();
            const int32 Removed = Blueprint->Bindings.Remove(Binding);
            if (Removed > 0)
            {
                MarkStructurallyModified(*Blueprint);
            }

            TSharedPtr<FJsonObject> Result = Ok();
            Result->SetNumberField(TEXT("removed"), Removed);
            return Result;
        });

    // widget.list_events — multicast delegate properties on a widget that an event graph
    // node can bind to. Params: widget_path, widget_name.
    Registry.Register(TEXT("widget.list_events"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidget* Widget = ResolveWidgetParam(Params, *Blueprint, TEXT("widget_name"), OutError);
            if (!Widget) return nullptr;

            TArray<TSharedPtr<FJsonValue>> Entries;
            for (TFieldIterator<FMulticastDelegateProperty> It(Widget->GetClass()); It; ++It)
            {
                FMulticastDelegateProperty* Delegate = *It;
                if (!Delegate->HasAnyPropertyFlags(CPF_BlueprintAssignable))
                {
                    continue;
                }
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"), Delegate->GetName());
                Entry->SetStringField(TEXT("category"), Delegate->GetMetaData(TEXT("Category")));
                Entry->SetStringField(TEXT("signature"),
                    Delegate->SignatureFunction ? Delegate->SignatureFunction->GetName() : FString());
                Entries.Add(MakeShared<FJsonValueObject>(Entry));
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("widget"), Widget->GetName());
            Result->SetStringField(TEXT("class"), Widget->GetClass()->GetPathName());
            Result->SetArrayField(TEXT("events"), Entries);
            return Result;
        });

    // widget.bind_event — create the event graph node for a widget's delegate, the same
    // thing the Details panel's event buttons do. Params: widget_path, widget_name,
    // event_name.
    Registry.Register(TEXT("widget.bind_event"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidget* Widget = ResolveWidgetParam(Params, *Blueprint, TEXT("widget_name"), OutError);
            if (!Widget) return nullptr;

            FString EventName;
            if (!RequireString(Params, TEXT("event_name"), EventName, OutError)) return nullptr;

            // The binding is made against the widget's blueprint variable, which only exists
            // once the widget is exposed as one.
            if (!Widget->bIsVariable)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("widget '%s' is not exposed as a variable — call widget.set_is_variable first"),
                        *Widget->GetName()));
                return nullptr;
            }

            FText ErrorText;
            if (!Check(FWidgetBlueprintOperationUtils::BindToEventProperty(
                    Blueprint, FName(*EventName), Widget->GetFName(), Widget->GetClass(),
                    /*bShouldJumpToNode*/ false, ErrorText),
                ErrorText, TEXT("widget.bind_event"), OutError))
            {
                return nullptr;
            }
            return Ok();
        });

    // =====================================================================================
    // UI COMPONENTS
    // =====================================================================================

    // widget.component_add — attach a UIComponent to a widget. Params: widget_path,
    // widget_name, component_class.
    Registry.Register(TEXT("widget.component_add"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            FString WidgetName, ComponentClassPath;
            if (!RequireString(Params, TEXT("widget_name"), WidgetName, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("component_class"), ComponentClassPath, OutError)) return nullptr;

            UClass* ComponentClass = ResolveWidgetClass(ComponentClassPath, OutError);
            if (!ComponentClass) return nullptr;
            if (!ComponentClass->IsChildOf(UUIComponent::StaticClass()))
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("%s is not a UIComponent subclass"), *ComponentClassPath));
                return nullptr;
            }

            FText ErrorText;
            UWidget* Target = FWidgetBlueprintOperationUtils::AddUIComponent(
                Blueprint, ComponentClass, FName(*WidgetName), ErrorText);
            if (!Check(Target != nullptr, ErrorText, TEXT("widget.component_add"), OutError))
            {
                return nullptr;
            }

            TSharedPtr<FJsonObject> Result = Ok();
            Result->SetStringField(TEXT("widget"), Target->GetName());
            return Result;
        });

    // widget.component_remove — detach a UIComponent. Params: widget_path, widget_name,
    // component_class.
    Registry.Register(TEXT("widget.component_remove"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            FString WidgetName, ComponentClassPath;
            if (!RequireString(Params, TEXT("widget_name"), WidgetName, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("component_class"), ComponentClassPath, OutError)) return nullptr;

            UClass* ComponentClass = ResolveWidgetClass(ComponentClassPath, OutError);
            if (!ComponentClass) return nullptr;

            FText ErrorText;
            if (!Check(FWidgetBlueprintOperationUtils::RemoveUIComponent(
                    Blueprint, ComponentClass, FName(*WidgetName), ErrorText),
                ErrorText, TEXT("widget.component_remove"), OutError))
            {
                return nullptr;
            }
            return Ok();
        });

    // widget.component_move — reorder a UIComponent relative to another on the same widget.
    // Params: widget_path, widget_name, component_class, relative_to_class, move_after
    // (default true).
    Registry.Register(TEXT("widget.component_move"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            FString WidgetName, ComponentClassPath, RelativeClassPath;
            if (!RequireString(Params, TEXT("widget_name"), WidgetName, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("component_class"), ComponentClassPath, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("relative_to_class"), RelativeClassPath, OutError)) return nullptr;

            UClass* ComponentClass = ResolveWidgetClass(ComponentClassPath, OutError);
            if (!ComponentClass) return nullptr;
            UClass* RelativeClass = ResolveWidgetClass(RelativeClassPath, OutError);
            if (!RelativeClass) return nullptr;

            bool bMoveAfter = true;
            Params->TryGetBoolField(TEXT("move_after"), bMoveAfter);

            FText ErrorText;
            if (!Check(FWidgetBlueprintOperationUtils::MoveUIComponent(
                    Blueprint, ComponentClass, RelativeClass, FName(*WidgetName), bMoveAfter, ErrorText),
                ErrorText, TEXT("widget.component_move"), OutError))
            {
                return nullptr;
            }
            return Ok();
        });

    // =====================================================================================
    // CLIPBOARD TEXT
    // =====================================================================================

    // widget.export_text — serialise widgets to the editor's clipboard text format.
    // Params: widget_path, widget_names (array).
    Registry.Register(TEXT("widget.export_text"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            const TArray<TSharedPtr<FJsonValue>>* NamesJson = nullptr;
            if (!Params->TryGetArrayField(TEXT("widget_names"), NamesJson) || NamesJson->Num() == 0)
            {
                Fail(OutError, FMCPError::InvalidParams, TEXT("widget_names is required and must be a non-empty array"));
                return nullptr;
            }

            TArray<UWidget*> Widgets;
            for (const TSharedPtr<FJsonValue>& Value : *NamesJson)
            {
                UWidget* Widget = FindWidget(*Blueprint, Value->AsString(), OutError);
                if (!Widget) return nullptr;
                Widgets.Add(Widget);
            }

            FString ExportedText;
            FWidgetBlueprintEditorUtils::ExportWidgetsToText(Widgets, ExportedText);

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("text"), ExportedText);
            return Result;
        });

    // widget.import_text — recreate widgets from clipboard text, optionally parenting the
    // imported roots. Params: widget_path, text, parent_widget (optional).
    Registry.Register(TEXT("widget.import_text"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            FString Text;
            if (!RequireString(Params, TEXT("text"), Text, OutError)) return nullptr;

            UPanelWidget* TargetParent = nullptr;
            FString ParentName;
            if (Params->TryGetStringField(TEXT("parent_widget"), ParentName) && !ParentName.IsEmpty())
            {
                UWidget* Candidate = FindWidget(*Blueprint, ParentName, OutError);
                if (!Candidate) return nullptr;
                TargetParent = Cast<UPanelWidget>(Candidate);
                if (!TargetParent)
                {
                    Fail(OutError, FMCPError::InvalidParams,
                        FString::Printf(TEXT("parent_widget '%s' is not a panel widget"), *ParentName));
                    return nullptr;
                }
            }

            TSet<UWidget*> ImportedWidgets;
            TMap<FName, UWidgetSlotPair*> SlotData;
            FWidgetBlueprintEditorUtils::ImportWidgetsFromText(Blueprint, Text, ImportedWidgets, SlotData);

            TArray<TSharedPtr<FJsonValue>> Created;
            for (UWidget* Imported : ImportedWidgets)
            {
                if (Imported->GetParent() != nullptr)
                {
                    continue;
                }
                if (TargetParent)
                {
                    FText ErrorText;
                    if (!FWidgetBlueprintOperationUtils::AddWidget(Blueprint, Imported, TargetParent, -1, ErrorText))
                    {
                        FWidgetBlueprintOperationUtils::RemoveTransientWidgetFromTree(Blueprint, Imported);
                        Check(false, ErrorText, TEXT("widget.import_text"), OutError);
                        return nullptr;
                    }
                }
                Created.Add(MakeShared<FJsonValueObject>(MakeWidgetJson(Imported)));
            }

            MarkStructurallyModified(*Blueprint);

            TSharedPtr<FJsonObject> Result = Ok();
            Result->SetNumberField(TEXT("imported_count"), ImportedWidgets.Num());
            Result->SetArrayField(TEXT("roots"), Created);
            return Result;
        });
}
