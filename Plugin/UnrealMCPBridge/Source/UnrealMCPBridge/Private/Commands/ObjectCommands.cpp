#include "Commands/ObjectCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "UObject/PropertyPortFlags.h"

namespace
{
    using UnrealMCPBridge::Json::RequireString;

    // Anonymous-namespace helpers get an Obj prefix on purpose. UBT's unity build merges the
    // command .cpp files into one translation unit, where two files' identically named static
    // helpers become a redefinition error (see the note in CommandJsonHelpers.h).

    /**
     * Resolves an object path to a live UObject, loading the package if needed.
     *
     * Accepts the three shapes that show up in practice:
     *   /Game/Path/Asset.Asset                  — the asset itself
     *   /Game/Path/Asset.Asset:SubObject        — an instanced subobject (camera nodes, and such)
     *   /Game/Map.Map:PersistentLevel.Actor_0   — a live actor, PIE worlds included
     *
     * FindObject comes first so that already-loaded objects (and PIE-world objects, which no
     * loader can produce) resolve without touching the package loader. Only then do we try
     * LoadObject, and only then the split fallback: LoadObject can fail on a subobject path
     * whose package is not yet in memory, because it resolves the subobject before the outer
     * has been created. Loading the outer explicitly and searching inside it fixes that case.
     *
     * Note the deliberate absence of any short-name lookup. FindFirstObject would accept
     * "BoomArmCameraNode_0", but it fires an EnsureIfAmbiguous ensure when two assets contain
     * a subobject of that name — which halts a debugger inside whatever is executing the
     * request and reads as a crash of the bridge. Full paths only.
     */
    UObject* ObjResolveObject(const FString& Path)
    {
        if (Path.IsEmpty())
        {
            return nullptr;
        }

        if (UObject* Found = FindObject<UObject>(nullptr, *Path))
        {
            return Found;
        }
        if (UObject* Loaded = LoadObject<UObject>(nullptr, *Path))
        {
            return Loaded;
        }

        // Subobject path whose outer is not loaded yet: load the outer, then walk inward.
        FString OuterPath, SubPath;
        if (!Path.Split(TEXT(":"), &OuterPath, &SubPath))
        {
            return nullptr;
        }
        UObject* Outer = LoadObject<UObject>(nullptr, *OuterPath);
        if (!Outer)
        {
            return nullptr;
        }

        TArray<FString> Parts;
        SubPath.ParseIntoArray(Parts, TEXT("."), /*InCullEmpty=*/true);
        UObject* Current = Outer;
        for (const FString& Part : Parts)
        {
            Current = StaticFindObject(UObject::StaticClass(), Current, *Part);
            if (!Current)
            {
                return nullptr;
            }
        }
        return Current;
    }

    /** A property plus the address of the specific value it names — the result of walking a path. */
    struct FObjResolvedProperty
    {
        FProperty* Property = nullptr;
        void*      Address  = nullptr;
    };

    /**
     * Walks a dotted property path from a root object down to one value.
     *
     * Supports struct members, object hops, and array indexing, so all of these work:
     *   "BoomOffset"                  a plain property
     *   "RelativeLocation.X"          a struct member
     *   "Children[2]"                 one array element
     *   "Children[2].bIsEnabled"      a member of the object an array element points at
     *
     * The address returned is the value's own address, which is what ImportText_Direct and
     * ExportText_Direct both want — not the container's.
     */
    bool ObjResolvePropertyPath(UObject* Root, const FString& PropertyPath, FObjResolvedProperty& Out, FString& OutError)
    {
        TArray<FString> Tokens;
        PropertyPath.ParseIntoArray(Tokens, TEXT("."), /*InCullEmpty=*/true);
        if (Tokens.Num() == 0)
        {
            OutError = TEXT("property_name is empty");
            return false;
        }

        UStruct* CurrentStruct = Root->GetClass();
        void*    CurrentAddr   = Root;
        FProperty* CurrentProp = nullptr;

        for (int32 TokenIndex = 0; TokenIndex < Tokens.Num(); ++TokenIndex)
        {
            FString Token = Tokens[TokenIndex];

            // Peel a trailing [n] off the token, if present.
            int32 ArrayIndex = INDEX_NONE;
            int32 OpenBracket = INDEX_NONE;
            if (Token.FindChar(TEXT('['), OpenBracket))
            {
                const int32 CloseBracket = Token.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
                if (CloseBracket == INDEX_NONE || CloseBracket <= OpenBracket)
                {
                    OutError = FString::Printf(TEXT("Malformed array index in '%s'"), *Token);
                    return false;
                }
                ArrayIndex = FCString::Atoi(*Token.Mid(OpenBracket + 1, CloseBracket - OpenBracket - 1));
                Token = Token.Left(OpenBracket);
            }

            if (!CurrentStruct)
            {
                OutError = FString::Printf(TEXT("Cannot descend into '%s' — the previous step is not a struct or object"), *Token);
                return false;
            }

            FProperty* Prop = FindFProperty<FProperty>(CurrentStruct, *Token);
            if (!Prop)
            {
                OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *Token, *CurrentStruct->GetName());
                return false;
            }

            void* Addr = Prop->ContainerPtrToValuePtr<void>(CurrentAddr);

            if (ArrayIndex != INDEX_NONE)
            {
                FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
                if (!ArrayProp)
                {
                    OutError = FString::Printf(TEXT("Property '%s' is not an array, so [%d] is invalid"), *Token, ArrayIndex);
                    return false;
                }
                FScriptArrayHelper Helper(ArrayProp, Addr);
                if (!Helper.IsValidIndex(ArrayIndex))
                {
                    OutError = FString::Printf(TEXT("Index %d is out of range for '%s' (%d elements)"),
                        ArrayIndex, *Token, Helper.Num());
                    return false;
                }
                Addr = Helper.GetRawPtr(ArrayIndex);
                Prop = ArrayProp->Inner;
            }

            CurrentProp = Prop;
            CurrentAddr = Addr;

            const bool bMoreToWalk = (TokenIndex + 1 < Tokens.Num());
            if (bMoreToWalk)
            {
                if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
                {
                    CurrentStruct = StructProp->Struct;
                    // CurrentAddr already points at the struct instance.
                }
                else if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
                {
                    UObject* Inner = ObjProp->GetObjectPropertyValue(Addr);
                    if (!Inner)
                    {
                        OutError = FString::Printf(TEXT("'%s' is null, cannot walk past it"), *Token);
                        return false;
                    }
                    CurrentStruct = Inner->GetClass();
                    CurrentAddr   = Inner;
                }
                else
                {
                    OutError = FString::Printf(TEXT("'%s' is a %s — only structs and objects can be walked into"),
                        *Token, *Prop->GetClass()->GetName());
                    return false;
                }
            }
        }

        Out.Property = CurrentProp;
        Out.Address  = CurrentAddr;
        return true;
    }

    /** ExportText for one value. DefaultValue is null so the export is never delta-suppressed. */
    FString ObjExportValue(const FProperty* Prop, const void* Address, UObject* Owner)
    {
        FString Text;
        Prop->ExportText_Direct(Text, Address, /*DefaultValue=*/nullptr, Owner, PPF_None);
        return Text;
    }

    /** "FVector"/"UCameraNode*"-style type name, for humans reading list_properties output. */
    FString ObjTypeName(const FProperty* Prop)
    {
        return Prop->GetCPPType();
    }
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

            UObject* Obj = ObjResolveObject(Path);
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
                Entry->SetStringField(TEXT("type"),  ObjTypeName(Prop));
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
                    Entry->SetStringField(TEXT("value"), ObjExportValue(Prop, Addr, Obj));
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

            UObject* Obj = ObjResolveObject(Path);
            if (!Obj)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("Could not resolve object: %s"), *Path);
                return nullptr;
            }

            FObjResolvedProperty Resolved;
            FString ResolveError;
            if (!ObjResolvePropertyPath(Obj, PropertyName, Resolved, ResolveError))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = ResolveError;
                return nullptr;
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("object_path"),   Obj->GetPathName());
            Result->SetStringField(TEXT("property_name"), PropertyName);
            Result->SetStringField(TEXT("type"),          ObjTypeName(Resolved.Property));
            Result->SetStringField(TEXT("value"),         ObjExportValue(Resolved.Property, Resolved.Address, Obj));
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

            UObject* Obj = ObjResolveObject(Path);
            if (!Obj)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("Could not resolve object: %s"), *Path);
                return nullptr;
            }

            FObjResolvedProperty Resolved;
            FString ResolveError;
            if (!ObjResolvePropertyPath(Obj, PropertyName, Resolved, ResolveError))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = ResolveError;
                return nullptr;
            }

            const FString OldValue = ObjExportValue(Resolved.Property, Resolved.Address, Obj);

            Obj->Modify();
            Obj->PreEditChange(Resolved.Property);

            const TCHAR* ImportResult = Resolved.Property->ImportText_Direct(*Value, Resolved.Address, Obj, PPF_None);
            if (ImportResult == nullptr)
            {
                // ImportText leaves the value untouched when it cannot parse, so there is nothing
                // to roll back — but PreEditChange was already announced, so close the pair.
                FPropertyChangedEvent FailedEvent(Resolved.Property, EPropertyChangeType::ValueSet);
                Obj->PostEditChangeProperty(FailedEvent);

                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(
                    TEXT("Failed to parse value '%s' for property '%s' (type %s). Read the property first and match that format."),
                    *Value, *PropertyName, *ObjTypeName(Resolved.Property));
                return nullptr;
            }

            FPropertyChangedEvent ChangedEvent(Resolved.Property, EPropertyChangeType::ValueSet);
            Obj->PostEditChangeProperty(ChangedEvent);
            Obj->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            Result->SetStringField(TEXT("object_path"), Obj->GetPathName());
            Result->SetStringField(TEXT("old_value"), OldValue);
            Result->SetStringField(TEXT("new_value"), ObjExportValue(Resolved.Property, Resolved.Address, Obj));
            return Result;
        });
}
