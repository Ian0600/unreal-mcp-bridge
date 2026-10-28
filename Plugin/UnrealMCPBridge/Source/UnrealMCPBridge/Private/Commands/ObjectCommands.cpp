#include "Commands/ObjectCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "Commands/CommandReflectionHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "UObject/PropertyPortFlags.h"

namespace
{
    using UnrealMCPBridge::Json::RequireString;
    // Path resolution and the import/export rules live in CommandReflectionHelpers.h so that
    // level.* (which writes the same actor properties, with dry-run diffs) cannot drift from them.
    using UnrealMCPBridge::Reflection::ResolveObject;
    using UnrealMCPBridge::Reflection::FResolvedProperty;
    using UnrealMCPBridge::Reflection::ResolvePropertyPath;
    using UnrealMCPBridge::Reflection::ExportValue;
    using UnrealMCPBridge::Reflection::TypeName;
}

void FObjectCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    // object.list_properties — every reflected property on an object, with its current value.
    // Params: object_path (string, required), include_values (bool, optional, default true).
    // Returns { object_path, object_class, properties: [{name, type, class, value, flags}] }.
    Registry.Register(TEXT("object.list_properties"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString Path;
            if (!RequireString(Params, TEXT("object_path"), Path, OutError)) return nullptr;

            UObject* Obj = ResolveObject(Path);
            if (!Obj)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("Could not resolve object: %s"), *Path);
                return nullptr;
            }

            bool bIncludeValues = true;
            Params->TryGetBoolField(TEXT("include_values"), bIncludeValues);

            TArray<TSharedPtr<FJsonValue>> PropArray;
            for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
            {
                FProperty* Prop = *It;
                auto Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"),  Prop->GetName());
                Entry->SetStringField(TEXT("type"),  TypeName(Prop));
                Entry->SetStringField(TEXT("class"), Prop->GetClass()->GetName());

                // Which of these the editor UI would have shown, so a caller can tell at a glance
                // whether they are reaching for something Python/the details panel could not.
                TArray<TSharedPtr<FJsonValue>> Flags;
                if (Prop->HasAnyPropertyFlags(CPF_Edit))               Flags.Add(MakeShared<FJsonValueString>(TEXT("Edit")));
                if (Prop->HasAnyPropertyFlags(CPF_EditConst))          Flags.Add(MakeShared<FJsonValueString>(TEXT("EditConst")));
                if (Prop->HasAnyPropertyFlags(CPF_BlueprintVisible))   Flags.Add(MakeShared<FJsonValueString>(TEXT("BlueprintVisible")));
                if (Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly))  Flags.Add(MakeShared<FJsonValueString>(TEXT("BlueprintReadOnly")));
                if (Prop->HasAnyPropertyFlags(CPF_Transient))          Flags.Add(MakeShared<FJsonValueString>(TEXT("Transient")));
                if (Prop->HasAnyPropertyFlags(CPF_InstancedReference)) Flags.Add(MakeShared<FJsonValueString>(TEXT("Instanced")));
                Entry->SetArrayField(TEXT("flags"), Flags);

                if (bIncludeValues)
                {
                    const void* Addr = Prop->ContainerPtrToValuePtr<void>(Obj);
                    Entry->SetStringField(TEXT("value"), ExportValue(Prop, Addr, Obj));
                }
                PropArray.Add(MakeShared<FJsonValueObject>(Entry));
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("object_path"),  Obj->GetPathName());
            Result->SetStringField(TEXT("object_class"), Obj->GetClass()->GetName());
            Result->SetArrayField(TEXT("properties"), PropArray);
            return Result;
        });

    // object.get_property — reads one property (or a nested member / array element) as text.
    // Params: object_path, property_name (supports "A.B", "A[2]", "A[2].B").
    // Returns { object_path, property_name, type, value }.
    Registry.Register(TEXT("object.get_property"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString Path, PropertyName;
            if (!RequireString(Params, TEXT("object_path"), Path, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("property_name"), PropertyName, OutError)) return nullptr;

            UObject* Obj = ResolveObject(Path);
            if (!Obj)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("Could not resolve object: %s"), *Path);
                return nullptr;
            }

            FResolvedProperty Resolved;
            FString ResolveError;
            if (!ResolvePropertyPath(Obj, PropertyName, Resolved, ResolveError))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = ResolveError;
                return nullptr;
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("object_path"),   Obj->GetPathName());
            Result->SetStringField(TEXT("property_name"), PropertyName);
            Result->SetStringField(TEXT("type"),          TypeName(Resolved.Property));
            Result->SetStringField(TEXT("value"),         ExportValue(Resolved.Property, Resolved.Address, Obj));
            return Result;
        });

    // object.set_property — writes one property (or nested member / array element) from text.
    // Params: object_path, property_name, value (Unreal's ImportText format).
    // Returns { ok, old_value, new_value }.
    //
    // The value format is exactly what object.get_property returned, which is the reliable way
    // to author one: read it, edit the part you want, write it back. Structs are "(X=1,Y=2)",
    // arrays are "(Elem1,Elem2)", object references are their full path.
    //
    // This does NOT rebuild anything downstream. A camera rig needs camerarig.build afterwards,
    // a Blueprint needs blueprint.compile, and nothing is persisted until asset.save.
    Registry.Register(TEXT("object.set_property"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString Path, PropertyName, Value;
            if (!RequireString(Params, TEXT("object_path"), Path, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("property_name"), PropertyName, OutError)) return nullptr;
            // Not RequireString: an empty string is a legitimate value for FString/FName/FText
            // properties, and rejecting it would make those unclearable.
            if (!Params->TryGetStringField(TEXT("value"), Value))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("value is required");
                return nullptr;
            }

            UObject* Obj = ResolveObject(Path);
            if (!Obj)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("Could not resolve object: %s"), *Path);
                return nullptr;
            }

            FString OldValue, NewValue, SetError;
            if (!UnrealMCPBridge::Reflection::SetPropertyFromText(Obj, PropertyName, Value, OldValue, NewValue, SetError))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = SetError;
                return nullptr;
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            Result->SetStringField(TEXT("object_path"), Obj->GetPathName());
            Result->SetStringField(TEXT("old_value"), OldValue);
            Result->SetStringField(TEXT("new_value"), NewValue);
            return Result;
        });

    // object.set_properties — several property paths on one object at once.
    // Params: object_path (string, required), values ({path: text}, required — paths as in
    // object.set_property, e.g. "PatternRules[Spore].Cooldown"), dry_run (bool, default false).
    // Every value is parsed before anything is written, so one bad entry writes nothing.
    // Returns { object_path, dry_run, changes: [{path, old_value, new_value, changed}] }.
    Registry.Register(TEXT("object.set_properties"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString Path;
            if (!RequireString(Params, TEXT("object_path"), Path, OutError)) return nullptr;
            const TSharedPtr<FJsonObject>* Values = nullptr;
            if (!Params->TryGetObjectField(TEXT("values"), Values) || !Values || (*Values)->Values.Num() == 0)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("values ({\"Path.To.Property\": \"text\", ...}) is required");
                return nullptr;
            }
            bool bDryRun = false;
            Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

            UObject* Obj = ResolveObject(Path);
            if (!Obj)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("Could not resolve object: %s"), *Path);
                return nullptr;
            }

            // Pass 1: resolve and parse everything.
            struct FPlanned { FString PropPath, Text, Old, New; };
            TArray<FPlanned> Plan;
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Values)->Values)
            {
                FPlanned Item;
                Item.PropPath = Pair.Key;
                if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(Item.Text))
                {
                    // Numbers and bools arrive as JSON scalars; ImportText wants their text.
                    double Number = 0.0;
                    bool bBool = false;
                    if (Pair.Value.IsValid() && Pair.Value->TryGetNumber(Number)) { Item.Text = FString::SanitizeFloat(Number); }
                    else if (Pair.Value.IsValid() && Pair.Value->TryGetBool(bBool)) { Item.Text = bBool ? TEXT("True") : TEXT("False"); }
                    else
                    {
                        OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                        OutError.Message = FString::Printf(TEXT("%s: value must be text, a number or a bool"), *Item.PropPath);
                        return nullptr;
                    }
                }
                FResolvedProperty Resolved;
                FString Error;
                if (!ResolvePropertyPath(Obj, Item.PropPath, Resolved, Error)
                    || !UnrealMCPBridge::Reflection::PreviewImport(Resolved.Property, Item.Text, Resolved.Owner, Item.New, Error))
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = FString::Printf(TEXT("%s: %s (nothing was written)"), *Item.PropPath, *Error);
                    return nullptr;
                }
                Item.Old = ExportValue(Resolved.Property, Resolved.Address, Resolved.Owner);
                Plan.Add(MoveTemp(Item));
            }

            // Pass 2: write (unless dry run).
            TArray<TSharedPtr<FJsonValue>> Changes;
            for (FPlanned& Item : Plan)
            {
                if (!bDryRun)
                {
                    FString Error;
                    if (!UnrealMCPBridge::Reflection::SetPropertyFromText(Obj, Item.PropPath, Item.Text, Item.Old, Item.New, Error))
                    {
                        OutError.Code = MCPProtocol::FMCPError::InternalError;
                        OutError.Message = FString::Printf(TEXT("%s: %s (entries before it were written)"), *Item.PropPath, *Error);
                        return nullptr;
                    }
                }
                auto Change = MakeShared<FJsonObject>();
                Change->SetStringField(TEXT("path"), Item.PropPath);
                Change->SetStringField(TEXT("old_value"), Item.Old);
                Change->SetStringField(TEXT("new_value"), Item.New);
                Change->SetBoolField(TEXT("changed"), Item.Old != Item.New);
                Changes.Add(MakeShared<FJsonValueObject>(Change));
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("object_path"), Obj->GetPathName());
            Result->SetBoolField(TEXT("dry_run"), bDryRun);
            Result->SetArrayField(TEXT("changes"), Changes);
            return Result;
        });

    // object.diff_from_defaults — the properties whose value differs from the class default.
    // Params: object_path (string, required), against ("class" default = the object's own class
    // default object, so a Blueprint class's defaults count as defaults; "native" = the nearest C++
    // class's defaults, i.e. what the code would give), edit_only (bool, default true = only
    // properties the details panel shows).
    // Returns { object_path, against, against_class, differences: [{name, type, value, default}] }.
    Registry.Register(TEXT("object.diff_from_defaults"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString Path;
            if (!RequireString(Params, TEXT("object_path"), Path, OutError)) return nullptr;
            FString Against = TEXT("class");
            Params->TryGetStringField(TEXT("against"), Against);
            bool bEditOnly = true;
            Params->TryGetBoolField(TEXT("edit_only"), bEditOnly);

            UObject* Obj = ResolveObject(Path);
            if (!Obj)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("Could not resolve object: %s"), *Path);
                return nullptr;
            }

            UClass* DefaultsClass = Obj->GetClass();
            if (Against.Equals(TEXT("native"), ESearchCase::IgnoreCase))
            {
                while (DefaultsClass && !DefaultsClass->HasAnyClassFlags(CLASS_Native))
                {
                    DefaultsClass = DefaultsClass->GetSuperClass();
                }
            }
            else if (!Against.Equals(TEXT("class"), ESearchCase::IgnoreCase))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("against must be \"class\" or \"native\"");
                return nullptr;
            }
            const UObject* Defaults = DefaultsClass ? DefaultsClass->GetDefaultObject() : nullptr;
            if (!Defaults || Defaults == Obj)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("No default object to compare with (is this the class default object itself?)");
                return nullptr;
            }

            TArray<TSharedPtr<FJsonValue>> Differences;
            // Only properties the defaults class has — a Blueprint's own variables have no native default.
            for (TFieldIterator<FProperty> It(DefaultsClass); It; ++It)
            {
                FProperty* Prop = *It;
                if (bEditOnly && !Prop->HasAnyPropertyFlags(CPF_Edit))
                {
                    continue;
                }
                if (Prop->HasAnyPropertyFlags(CPF_Transient))
                {
                    continue;
                }
                const void* Value = Prop->ContainerPtrToValuePtr<void>(Obj);
                const void* Default = Prop->ContainerPtrToValuePtr<void>(Defaults);
                if (Prop->Identical(Value, Default, PPF_None))
                {
                    continue;
                }
                auto Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"), Prop->GetName());
                Entry->SetStringField(TEXT("type"), TypeName(Prop));
                Entry->SetStringField(TEXT("value"), ExportValue(Prop, Value, Obj));
                Entry->SetStringField(TEXT("default"), ExportValue(Prop, Default, const_cast<UObject*>(Defaults)));
                Differences.Add(MakeShared<FJsonValueObject>(Entry));
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("object_path"), Obj->GetPathName());
            Result->SetStringField(TEXT("against"), Against.ToLower());
            Result->SetStringField(TEXT("against_class"), DefaultsClass->GetPathName());
            Result->SetArrayField(TEXT("differences"), Differences);
            return Result;
        });
}
