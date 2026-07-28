#include "Commands/CameraVariableCommands.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "Core/CameraVariableCollection.h"
#include "Core/CameraVariableAssets.h"
#include "UObject/Package.h"
#include "AssetRegistry/AssetRegistryModule.h"

namespace
{
    UCameraVariableCollection* LoadCollection(const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        FString Path;
        if (!Params.IsValid() || !Params->TryGetStringField(TEXT("collection_path"), Path) || Path.IsEmpty())
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = TEXT("collection_path is required and must be non-empty");
            return nullptr;
        }
        UCameraVariableCollection* Coll = LoadObject<UCameraVariableCollection>(nullptr, *Path);
        if (!Coll)
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("Could not load CameraVariableCollection: %s"), *Path);
        }
        return Coll;
    }

    bool CamVarRequireString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FString& Out, MCPProtocol::FMCPError& OutError)
    {
        if (!Params->TryGetStringField(Field, Out) || Out.IsEmpty())
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("%s is required and must be non-empty"), Field);
            return false;
        }
        return true;
    }

    // Resolves a short variable type ("Vector3d", "Double", "Boolean", ...) to its concrete
    // UClass under /Script/GameplayCameras. Accepts an optional leading 'U' and/or trailing
    // "CameraVariable". Rejects non-CameraVariable or abstract classes.
    UClass* ResolveVarClass(const FString& TypeIn, MCPProtocol::FMCPError& OutError)
    {
        FString Bare = TypeIn;
        Bare.TrimStartAndEndInline();
        Bare.RemoveFromStart(TEXT("U"));
        if (!Bare.EndsWith(TEXT("CameraVariable")))
        {
            Bare += TEXT("CameraVariable");
        }
        const FString ClassPath = FString::Printf(TEXT("/Script/GameplayCameras.%s"), *Bare);
        UClass* Cls = FindObject<UClass>(nullptr, *ClassPath);
        if (!Cls)
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(
                TEXT("Unknown camera variable type '%s' (looked for %s). Valid: Boolean, Integer32, Float, Double, Vector2f/2d, Vector3f/3d, Vector4f/4d, Rotator3f/3d, Transform3f/3d."),
                *TypeIn, *ClassPath);
            return nullptr;
        }
        if (!Cls->IsChildOf(UCameraVariableAsset::StaticClass()) || Cls->HasAnyClassFlags(CLASS_Abstract))
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("'%s' is not a concrete CameraVariable type"), *TypeIn);
            return nullptr;
        }
        return Cls;
    }

    // The default-value property is "DefaultValue" on every type except Boolean ("bDefaultValue").
    FProperty* ValueProp(const UCameraVariableAsset* Var)
    {
        if (FProperty* P = FindFProperty<FProperty>(Var->GetClass(), TEXT("DefaultValue")))
        {
            return P;
        }
        return FindFProperty<FProperty>(Var->GetClass(), TEXT("bDefaultValue"));
    }

    // "Vector3dCameraVariable" -> "Vector3d"
    FString TypeFromClass(const UClass* Cls)
    {
        FString N = Cls->GetName();
        N.RemoveFromEnd(TEXT("CameraVariable"));
        return N;
    }

    // The variable's display label: DisplayName if set, else its object name.
    FString VarDisplayName(const UCameraVariableAsset* Var)
    {
        FString DN;
#if WITH_EDITORONLY_DATA
        DN = Var->DisplayName;
#endif
        if (DN.IsEmpty())
        {
            DN = Var->GetName();
        }
        return DN;
    }
}

void FCameraVariableCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    // camvar.create_collection — creates a new empty UCameraVariableCollection asset.
    // Params: package_path (content folder), name. Returns { ok, path }. Marks dirty (save separately).
    Registry.Register(TEXT("camvar.create_collection"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString PackagePath, Name;
            if (!Params.IsValid()) { OutError.Code = MCPProtocol::FMCPError::InvalidParams; OutError.Message = TEXT("params required"); return nullptr; }
            if (!CamVarRequireString(Params, TEXT("package_path"), PackagePath, OutError)) return nullptr;
            if (!CamVarRequireString(Params, TEXT("name"), Name, OutError)) return nullptr;

            const FString FullName = PackagePath / Name;
            if (FindPackage(nullptr, *FullName) || LoadObject<UCameraVariableCollection>(nullptr, *(FullName + TEXT(".") + Name)))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("Asset already exists: %s"), *FullName);
                return nullptr;
            }

            UPackage* Package = CreatePackage(*FullName);
            if (!Package)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = FString::Printf(TEXT("Could not create package: %s"), *FullName);
                return nullptr;
            }

            UCameraVariableCollection* Coll = NewObject<UCameraVariableCollection>(
                Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
            FAssetRegistryModule::AssetCreated(Coll);
            Coll->MarkPackageDirty();

            auto Out = MakeShared<FJsonObject>();
            Out->SetBoolField(TEXT("ok"), true);
            Out->SetStringField(TEXT("path"), Coll->GetPathName());
            return Out;
        });

    // camvar.add — adds a typed variable to a collection.
    // Params: collection_path, var_type (short type e.g. "Vector3d"), name (DisplayName),
    //         default_value (optional ImportText for the DefaultValue property).
    // Returns { ok, name, type, guid, object }.
    Registry.Register(TEXT("camvar.add"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UCameraVariableCollection* Coll = LoadCollection(Params, OutError); if (!Coll) return nullptr;

            FString VarType, Name;
            if (!CamVarRequireString(Params, TEXT("var_type"), VarType, OutError)) return nullptr;
            if (!CamVarRequireString(Params, TEXT("name"), Name, OutError)) return nullptr;

            UClass* VarClass = ResolveVarClass(VarType, OutError); if (!VarClass) return nullptr;

            Coll->Modify();
            UCameraVariableAsset* Var = NewObject<UCameraVariableAsset>(
                Coll, VarClass, NAME_None, RF_Public | RF_Transactional);   // Guid auto-set in PostInitProperties
            if (!Var)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("Failed to create camera variable");
                return nullptr;
            }
#if WITH_EDITORONLY_DATA
            Var->DisplayName = Name;
#endif
            FString DefaultVal;
            if (Params->TryGetStringField(TEXT("default_value"), DefaultVal) && !DefaultVal.IsEmpty())
            {
                FProperty* VP = ValueProp(Var);
                if (!VP)
                {
                    OutError.Code = MCPProtocol::FMCPError::InternalError;
                    OutError.Message = FString::Printf(TEXT("%s has no default-value property"), *VarClass->GetName());
                    return nullptr;
                }
                void* ValuePtr = VP->ContainerPtrToValuePtr<void>(Var);
                const TCHAR* Result = VP->ImportText_Direct(*DefaultVal, ValuePtr, Var, PPF_None);
                if (Result == nullptr)
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = FString::Printf(TEXT("Failed to parse default_value '%s' for %s"), *DefaultVal, *VarClass->GetName());
                    return nullptr;
                }
            }

            Coll->Variables.Add(Var);
            Coll->MarkPackageDirty();

            auto Out = MakeShared<FJsonObject>();
            Out->SetBoolField(TEXT("ok"), true);
            Out->SetStringField(TEXT("name"), Name);
            Out->SetStringField(TEXT("type"), TypeFromClass(VarClass));
            Out->SetStringField(TEXT("guid"), Var->GetGuid().ToString());
            Out->SetStringField(TEXT("object"), Var->GetName());
            return Out;
        });

    // camvar.list — lists variables in a collection.
    // Params: collection_path. Returns { variables: [{index, name, type, guid, default_value}] }.
    Registry.Register(TEXT("camvar.list"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UCameraVariableCollection* Coll = LoadCollection(Params, OutError); if (!Coll) return nullptr;

            TArray<TSharedPtr<FJsonValue>> Arr;
            for (int32 i = 0; i < Coll->Variables.Num(); ++i)
            {
                UCameraVariableAsset* Var = Coll->Variables[i];
                if (!Var) continue;

                auto E = MakeShared<FJsonObject>();
                E->SetNumberField(TEXT("index"), i);
                E->SetStringField(TEXT("name"), VarDisplayName(Var));
                E->SetStringField(TEXT("type"), TypeFromClass(Var->GetClass()));
                E->SetStringField(TEXT("guid"), Var->GetGuid().ToString());
                if (FProperty* VP = ValueProp(Var))
                {
                    FString Exported;
                    const void* ValuePtr = VP->ContainerPtrToValuePtr<void>(Var);
                    VP->ExportTextItem_Direct(Exported, ValuePtr, nullptr, Var, PPF_None);
                    E->SetStringField(TEXT("default_value"), Exported);
                }
                Arr.Add(MakeShared<FJsonValueObject>(E));
            }

            auto Out = MakeShared<FJsonObject>();
            Out->SetArrayField(TEXT("variables"), Arr);
            return Out;
        });

    // camvar.set_default — sets a variable's default value (matched by name) via ImportText.
    // Params: collection_path, name (DisplayName), value (ImportText). Returns { ok }.
    Registry.Register(TEXT("camvar.set_default"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UCameraVariableCollection* Coll = LoadCollection(Params, OutError); if (!Coll) return nullptr;

            FString Name, Value;
            if (!CamVarRequireString(Params, TEXT("name"), Name, OutError)) return nullptr;
            if (!CamVarRequireString(Params, TEXT("value"), Value, OutError)) return nullptr;

            UCameraVariableAsset* Target = nullptr;
            for (UCameraVariableAsset* Var : Coll->Variables)
            {
                if (Var && VarDisplayName(Var).Equals(Name, ESearchCase::IgnoreCase))
                {
                    Target = Var;
                    break;
                }
            }
            if (!Target)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("No variable named '%s' in collection"), *Name);
                return nullptr;
            }

            FProperty* VP = ValueProp(Target);
            if (!VP)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = FString::Printf(TEXT("%s has no default-value property"), *Target->GetClass()->GetName());
                return nullptr;
            }

            Target->Modify();
            void* ValuePtr = VP->ContainerPtrToValuePtr<void>(Target);
            const TCHAR* Result = VP->ImportText_Direct(*Value, ValuePtr, Target, PPF_None);
            if (Result == nullptr)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("Failed to parse value '%s'"), *Value);
                return nullptr;
            }

            Coll->MarkPackageDirty();

            auto Out = MakeShared<FJsonObject>();
            Out->SetBoolField(TEXT("ok"), true);
            return Out;
        });
}
