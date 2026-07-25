#include "Commands/MaterialCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialExpressionComment.h"
#include "MaterialTypes.h" // UE 5.3: EMaterialParameterAssociation lives here (moved to Materials/MaterialParameters.h in 5.8)
#include "SceneTypes.h"

#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Engine/Texture.h"
#include "UObject/UnrealType.h"

// Bring FMCPError into scope for both the anonymous-namespace helpers below and the
// command lambdas in RegisterCommands (which reference FMCPError unqualified).
using namespace MCPProtocol;

namespace
{
    // ---------- generic param helpers ----------

    bool MtlRequireString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FString& Out, FMCPError& OutError)
    {
        if (!Params.IsValid() || !Params->TryGetStringField(Field, Out) || Out.IsEmpty())
        {
            OutError.Code = FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("%s is required and must be non-empty"), Field);
            return false;
        }
        return true;
    }

    void MtlFail(FMCPError& OutError, int32 Code, const FString& Msg)
    {
        OutError.Code = Code;
        OutError.Message = Msg;
    }

    // ---------- object loading ----------

    template <typename T>
    T* MtlLoadTyped(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FMCPError& OutError)
    {
        FString Path;
        if (!MtlRequireString(Params, Field, Path, OutError)) return nullptr;
        T* Obj = LoadObject<T>(nullptr, *Path);
        if (!Obj)
        {
            MtlFail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("Could not load %s: %s"), Field, *Path));
        }
        return Obj;
    }

    // Graph edits target either a UMaterial (material_path) or a UMaterialFunction (function_path).
    struct FGraphHost
    {
        UMaterial* Material = nullptr;
        UMaterialFunction* Function = nullptr;
        bool IsValid() const { return Material != nullptr || Function != nullptr; }
        UObject* AsObject() const { return Material ? static_cast<UObject*>(Material) : static_cast<UObject*>(Function); }
    };

    FGraphHost LoadGraphHost(const TSharedPtr<FJsonObject>& Params, FMCPError& OutError)
    {
        FGraphHost Host;
        FString MatPath, FuncPath;
        const bool bHasMat = Params.IsValid() && Params->TryGetStringField(TEXT("material_path"), MatPath) && !MatPath.IsEmpty();
        const bool bHasFunc = Params.IsValid() && Params->TryGetStringField(TEXT("function_path"), FuncPath) && !FuncPath.IsEmpty();
        if (bHasMat == bHasFunc)
        {
            MtlFail(OutError, FMCPError::InvalidParams, TEXT("provide exactly one of material_path or function_path"));
            return Host;
        }
        if (bHasMat)
        {
            Host.Material = LoadObject<UMaterial>(nullptr, *MatPath);
            if (!Host.Material) MtlFail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("Could not load material_path: %s"), *MatPath));
        }
        else
        {
            Host.Function = LoadObject<UMaterialFunction>(nullptr, *FuncPath);
            if (!Host.Function) MtlFail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("Could not load function_path: %s"), *FuncPath));
        }
        return Host;
    }

    TArray<UMaterialExpression*> MtlGetExpressions(const FGraphHost& Host)
    {
        // UE 5.3: UMaterialEditingLibrary has no array getters (added in 5.8). Read the
        // expression collection straight off the material/function instead.
        TArray<UMaterialExpression*> Out;
        if (Host.Material)
        {
            for (const TObjectPtr<UMaterialExpression>& E : Host.Material->GetExpressions()) Out.Add(E);
        }
        else if (Host.Function)
        {
            for (const TObjectPtr<UMaterialExpression>& E : Host.Function->GetExpressions()) Out.Add(E);
        }
        return Out;
    }

    // Ensures every expression carries a stable GUID we can hand back to callers, then returns it.
    FGuid EnsureExpressionGuid(UMaterialExpression* Expr)
    {
        if (!Expr) return FGuid();
        if (!Expr->MaterialExpressionGuid.IsValid())
        {
            Expr->MaterialExpressionGuid = FGuid::NewGuid();
        }
        return Expr->MaterialExpressionGuid;
    }

    UMaterialExpression* ResolveExpression(const FGraphHost& Host, const FString& GuidStr, FMCPError& OutError)
    {
        FGuid Guid;
        if (!FGuid::Parse(GuidStr, Guid))
        {
            MtlFail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("invalid expression guid: %s"), *GuidStr));
            return nullptr;
        }
        for (UMaterialExpression* Expr : MtlGetExpressions(Host))
        {
            if (Expr && Expr->MaterialExpressionGuid == Guid) return Expr;
        }
        MtlFail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("expression not found: %s"), *GuidStr));
        return nullptr;
    }

    // Resolves a UMaterialExpression subclass from a full script path ("/Script/Engine.MaterialExpressionAdd")
    // or a bare class name ("MaterialExpressionAdd", assumed to live in /Script/Engine).
    UClass* ResolveExpressionClass(const FString& In)
    {
        UClass* C = LoadObject<UClass>(nullptr, *In);
        if (!C && !In.Contains(TEXT(".")))
        {
            C = LoadObject<UClass>(nullptr, *(FString(TEXT("/Script/Engine.")) + In));
        }
        return (C && C->IsChildOf(UMaterialExpression::StaticClass())) ? C : nullptr;
    }

    // Comments live in a separate collection (GetEditorComments), not the expression array,
    // so they need their own guid resolver.
    UMaterialExpressionComment* ResolveComment(const FGraphHost& Host, const FString& GuidStr, FMCPError& OutError)
    {
        FGuid Guid;
        if (!FGuid::Parse(GuidStr, Guid))
        {
            MtlFail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("invalid comment guid: %s"), *GuidStr));
            return nullptr;
        }
        const TConstArrayView<TObjectPtr<UMaterialExpressionComment>> Comments =
            Host.Material ? Host.Material->GetEditorComments() : Host.Function->GetEditorComments();
        for (UMaterialExpressionComment* C : Comments)
        {
            if (C && C->MaterialExpressionGuid == Guid) return C;
        }
        MtlFail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("comment not found: %s"), *GuidStr));
        return nullptr;
    }

    // ---------- enum helpers ----------

    bool ParseMaterialProperty(const FString& In, EMaterialProperty& Out)
    {
        UEnum* E = StaticEnum<EMaterialProperty>();
        if (!E) return false;
        int64 V = E->GetValueByNameString(In);
        if (V == INDEX_NONE) V = E->GetValueByNameString(FString(TEXT("MP_")) + In);
        if (V == INDEX_NONE) return false;
        Out = static_cast<EMaterialProperty>(V);
        return true;
    }

    EMaterialParameterAssociation ParseAssociation(const TSharedPtr<FJsonObject>& Params)
    {
        FString S;
        if (Params.IsValid() && Params->TryGetStringField(TEXT("association"), S))
        {
            if (S.Equals(TEXT("LayerParameter"), ESearchCase::IgnoreCase)) return EMaterialParameterAssociation::LayerParameter;
            if (S.Equals(TEXT("BlendParameter"), ESearchCase::IgnoreCase)) return EMaterialParameterAssociation::BlendParameter;
        }
        return EMaterialParameterAssociation::GlobalParameter;
    }

    bool ReadLinearColor(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FLinearColor& Out, FMCPError& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!Params->TryGetArrayField(Field, Arr) || !Arr || Arr->Num() < 3)
        {
            MtlFail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("%s must be a [r,g,b] or [r,g,b,a] number array"), Field));
            return false;
        }
        Out.R = (float)(*Arr)[0]->AsNumber();
        Out.G = (float)(*Arr)[1]->AsNumber();
        Out.B = (float)(*Arr)[2]->AsNumber();
        Out.A = Arr->Num() >= 4 ? (float)(*Arr)[3]->AsNumber() : 1.0f;
        return true;
    }

    TSharedPtr<FJsonValue> LinearColorToJson(const FLinearColor& C)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(C.R));
        Arr.Add(MakeShared<FJsonValueNumber>(C.G));
        Arr.Add(MakeShared<FJsonValueNumber>(C.B));
        Arr.Add(MakeShared<FJsonValueNumber>(C.A));
        return MakeShared<FJsonValueArray>(Arr);
    }

    TSharedPtr<FJsonValue> NamesToJson(const TArray<FName>& Names)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FName& N : Names) Arr.Add(MakeShared<FJsonValueString>(N.ToString()));
        return MakeShared<FJsonValueArray>(Arr);
    }

    // Generic reflection-based property write + change notification. Works on materials,
    // material instances and expression nodes alike.
    bool SetPropertyByReflection(UObject* Obj, const FString& PropName, const FString& Value, FMCPError& OutError)
    {
        FProperty* Prop = Obj->GetClass()->FindPropertyByName(FName(*PropName));
        if (!Prop)
        {
            MtlFail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("property not found: %s"), *PropName));
            return false;
        }
        void* Addr = Prop->ContainerPtrToValuePtr<void>(Obj);
        Obj->Modify();
        const TCHAR* Result = Prop->ImportText_Direct(*Value, Addr, Obj, PPF_None);
        if (Result == nullptr)
        {
            MtlFail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("failed to import value '%s' for %s"), *Value, *PropName));
            return false;
        }
        FPropertyChangedEvent Evt(Prop);
        Obj->PostEditChangeProperty(Evt);
        Obj->MarkPackageDirty();
        return true;
    }

    TSharedPtr<FJsonObject> MtlOk()
    {
        auto Out = MakeShared<FJsonObject>();
        Out->SetBoolField(TEXT("ok"), true);
        return Out;
    }
}

void FMaterialCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    // =====================================================================================
    // CREATION
    // =====================================================================================

    // material.create — new UMaterial. Params: package_path, asset_name. Returns { object_path }.
    Registry.Register(TEXT("material.create"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString PackagePath, AssetName;
            if (!MtlRequireString(Params, TEXT("package_path"), PackagePath, OutError)) return nullptr;
            if (!MtlRequireString(Params, TEXT("asset_name"), AssetName, OutError)) return nullptr;

            IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
            UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
            UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UMaterial::StaticClass(), Factory);
            if (!NewAsset) { MtlFail(OutError, FMCPError::InternalError, TEXT("CreateAsset returned null (name in use or invalid path?)")); return nullptr; }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("object_path"), NewAsset->GetPathName());
            return Result;
        });

    // material.create_instance — new UMaterialInstanceConstant. Params: package_path, asset_name, parent_path.
    Registry.Register(TEXT("material.create_instance"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString PackagePath, AssetName, ParentPath;
            if (!MtlRequireString(Params, TEXT("package_path"), PackagePath, OutError)) return nullptr;
            if (!MtlRequireString(Params, TEXT("asset_name"), AssetName, OutError)) return nullptr;
            if (!MtlRequireString(Params, TEXT("parent_path"), ParentPath, OutError)) return nullptr;

            UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *ParentPath);
            if (!Parent) { MtlFail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("Could not load parent_path: %s"), *ParentPath)); return nullptr; }

            IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
            UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
            Factory->InitialParent = Parent;
            UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UMaterialInstanceConstant::StaticClass(), Factory);
            if (!NewAsset) { MtlFail(OutError, FMCPError::InternalError, TEXT("CreateAsset returned null (name in use or invalid path?)")); return nullptr; }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("object_path"), NewAsset->GetPathName());
            return Result;
        });

    // material.create_function — new UMaterialFunction. Params: package_path, asset_name.
    Registry.Register(TEXT("material.create_function"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString PackagePath, AssetName;
            if (!MtlRequireString(Params, TEXT("package_path"), PackagePath, OutError)) return nullptr;
            if (!MtlRequireString(Params, TEXT("asset_name"), AssetName, OutError)) return nullptr;

            IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
            UMaterialFunctionFactoryNew* Factory = NewObject<UMaterialFunctionFactoryNew>();
            UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UMaterialFunction::StaticClass(), Factory);
            if (!NewAsset) { MtlFail(OutError, FMCPError::InternalError, TEXT("CreateAsset returned null (name in use or invalid path?)")); return nullptr; }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("object_path"), NewAsset->GetPathName());
            return Result;
        });

    // =====================================================================================
    // INSPECTION
    // =====================================================================================

    // material.get_info — Params: object_path. Returns class, parent (for instances), expression count,
    // and parameter-name arrays. Works for UMaterial / UMaterialInstanceConstant.
    Registry.Register(TEXT("material.get_info"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UMaterialInterface* Mat = MtlLoadTyped<UMaterialInterface>(Params, TEXT("object_path"), OutError);
            if (!Mat) return nullptr;

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("class"), Mat->GetClass()->GetName());
            Result->SetStringField(TEXT("object_path"), Mat->GetPathName());

            if (UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Mat))
            {
                Result->SetStringField(TEXT("parent"), MIC->Parent ? MIC->Parent->GetPathName() : TEXT(""));
            }
            if (UMaterial* AsMat = Cast<UMaterial>(Mat))
            {
                Result->SetNumberField(TEXT("num_expressions"), UMaterialEditingLibrary::GetNumMaterialExpressions(AsMat));
            }

            TArray<FName> Scalars, Vectors, Textures, Switches;
            UMaterialEditingLibrary::GetScalarParameterNames(Mat, Scalars);
            UMaterialEditingLibrary::GetVectorParameterNames(Mat, Vectors);
            UMaterialEditingLibrary::GetTextureParameterNames(Mat, Textures);
            UMaterialEditingLibrary::GetStaticSwitchParameterNames(Mat, Switches);
            Result->SetField(TEXT("scalar_parameters"), NamesToJson(Scalars));
            Result->SetField(TEXT("vector_parameters"), NamesToJson(Vectors));
            Result->SetField(TEXT("texture_parameters"), NamesToJson(Textures));
            Result->SetField(TEXT("static_switch_parameters"), NamesToJson(Switches));
            return Result;
        });

    // material.list_parameters — Params: object_path. Returns each parameter with its current value
    // (default value for a base material; instance value + overridden flag for an instance).
    Registry.Register(TEXT("material.list_parameters"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UMaterialInterface* Mat = MtlLoadTyped<UMaterialInterface>(Params, TEXT("object_path"), OutError);
            if (!Mat) return nullptr;
            UMaterial* AsMat = Cast<UMaterial>(Mat);
            UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Mat);
            const EMaterialParameterAssociation Assoc = EMaterialParameterAssociation::GlobalParameter;

            TArray<FName> Scalars, Vectors, Textures, Switches;
            UMaterialEditingLibrary::GetScalarParameterNames(Mat, Scalars);
            UMaterialEditingLibrary::GetVectorParameterNames(Mat, Vectors);
            UMaterialEditingLibrary::GetTextureParameterNames(Mat, Textures);
            UMaterialEditingLibrary::GetStaticSwitchParameterNames(Mat, Switches);

            TArray<TSharedPtr<FJsonValue>> ScalarArr, VectorArr, TextureArr, SwitchArr;

            for (const FName& N : Scalars)
            {
                auto O = MakeShared<FJsonObject>();
                O->SetStringField(TEXT("name"), N.ToString());
                const float V = MIC ? UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(MIC, N, Assoc)
                                    : UMaterialEditingLibrary::GetMaterialDefaultScalarParameterValue(AsMat, N);
                O->SetNumberField(TEXT("value"), V);
                ScalarArr.Add(MakeShared<FJsonValueObject>(O));
            }
            for (const FName& N : Vectors)
            {
                auto O = MakeShared<FJsonObject>();
                O->SetStringField(TEXT("name"), N.ToString());
                const FLinearColor V = MIC ? UMaterialEditingLibrary::GetMaterialInstanceVectorParameterValue(MIC, N, Assoc)
                                           : UMaterialEditingLibrary::GetMaterialDefaultVectorParameterValue(AsMat, N);
                O->SetField(TEXT("value"), LinearColorToJson(V));
                VectorArr.Add(MakeShared<FJsonValueObject>(O));
            }
            for (const FName& N : Textures)
            {
                auto O = MakeShared<FJsonObject>();
                O->SetStringField(TEXT("name"), N.ToString());
                UTexture* Tex = MIC ? UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(MIC, N, Assoc)
                                    : UMaterialEditingLibrary::GetMaterialDefaultTextureParameterValue(AsMat, N);
                O->SetStringField(TEXT("value"), Tex ? Tex->GetPathName() : TEXT(""));
                TextureArr.Add(MakeShared<FJsonValueObject>(O));
            }
            for (const FName& N : Switches)
            {
                auto O = MakeShared<FJsonObject>();
                O->SetStringField(TEXT("name"), N.ToString());
                const bool V = MIC ? UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(MIC, N, Assoc)
                                   : UMaterialEditingLibrary::GetMaterialDefaultStaticSwitchParameterValue(AsMat, N);
                O->SetBoolField(TEXT("value"), V);
                SwitchArr.Add(MakeShared<FJsonValueObject>(O));
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("scalars"), ScalarArr);
            Result->SetArrayField(TEXT("vectors"), VectorArr);
            Result->SetArrayField(TEXT("textures"), TextureArr);
            Result->SetArrayField(TEXT("static_switches"), SwitchArr);
            return Result;
        });

    // material.list_expressions — Params: material_path OR function_path. Returns each node's guid,
    // class, grid position, and input/output pin names.
    Registry.Register(TEXT("material.list_expressions"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FGraphHost Host = LoadGraphHost(Params, OutError);
            if (!Host.IsValid()) return nullptr;

            TArray<TSharedPtr<FJsonValue>> Arr;
            for (UMaterialExpression* Expr : MtlGetExpressions(Host))
            {
                if (!Expr) continue;
                const FGuid Guid = EnsureExpressionGuid(Expr);

                int32 X = 0, Y = 0;
                UMaterialEditingLibrary::GetMaterialExpressionNodePosition(Expr, X, Y);

                auto O = MakeShared<FJsonObject>();
                O->SetStringField(TEXT("guid"), Guid.ToString());
                O->SetStringField(TEXT("class"), Expr->GetClass()->GetName());
                O->SetNumberField(TEXT("pos_x"), X);
                O->SetNumberField(TEXT("pos_y"), Y);

                // UE 5.3: only input pin names are exposed by the library (GetMaterialExpressionOutputNames
                // was added in 5.8). Output pin names are omitted here.
                TArray<TSharedPtr<FJsonValue>> Ins;
                for (const FString& S : UMaterialEditingLibrary::GetMaterialExpressionInputNames(Expr)) Ins.Add(MakeShared<FJsonValueString>(S));
                O->SetArrayField(TEXT("inputs"), Ins);
                Arr.Add(MakeShared<FJsonValueObject>(O));
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("expressions"), Arr);
            return Result;
        });

    // material.get_statistics — Params: object_path. Returns shader instruction/sampler counts.
    Registry.Register(TEXT("material.get_statistics"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UMaterialInterface* Mat = MtlLoadTyped<UMaterialInterface>(Params, TEXT("object_path"), OutError);
            if (!Mat) return nullptr;

            const FMaterialStatistics S = UMaterialEditingLibrary::GetStatistics(Mat);
            auto Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("vertex_shader_instructions"), S.NumVertexShaderInstructions);
            Result->SetNumberField(TEXT("pixel_shader_instructions"), S.NumPixelShaderInstructions);
            Result->SetNumberField(TEXT("samplers"), S.NumSamplers);
            Result->SetNumberField(TEXT("vertex_texture_samples"), S.NumVertexTextureSamples);
            Result->SetNumberField(TEXT("pixel_texture_samples"), S.NumPixelTextureSamples);
            Result->SetNumberField(TEXT("virtual_texture_samples"), S.NumVirtualTextureSamples);
            return Result;
        });

    // =====================================================================================
    // INSTANCE PARAMETER EDITING
    // =====================================================================================

    // material.set_scalar_param — Params: instance_path, parameter_name, value, [association].
    Registry.Register(TEXT("material.set_scalar_param"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UMaterialInstanceConstant* MIC = MtlLoadTyped<UMaterialInstanceConstant>(Params, TEXT("instance_path"), OutError);
            if (!MIC) return nullptr;
            FString Name; if (!MtlRequireString(Params, TEXT("parameter_name"), Name, OutError)) return nullptr;
            double Value = 0.0;
            if (!Params->TryGetNumberField(TEXT("value"), Value)) { MtlFail(OutError, FMCPError::InvalidParams, TEXT("value (number) is required")); return nullptr; }

            const bool bOk = UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MIC, FName(*Name), (float)Value, ParseAssociation(Params));
            if (!bOk) { MtlFail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("parameter not found: %s"), *Name)); return nullptr; }
            MIC->MarkPackageDirty();
            return MtlOk();
        });

    // material.set_vector_param — Params: instance_path, parameter_name, value ([r,g,b,a]), [association].
    Registry.Register(TEXT("material.set_vector_param"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UMaterialInstanceConstant* MIC = MtlLoadTyped<UMaterialInstanceConstant>(Params, TEXT("instance_path"), OutError);
            if (!MIC) return nullptr;
            FString Name; if (!MtlRequireString(Params, TEXT("parameter_name"), Name, OutError)) return nullptr;
            FLinearColor Value; if (!ReadLinearColor(Params, TEXT("value"), Value, OutError)) return nullptr;

            const bool bOk = UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(MIC, FName(*Name), Value, ParseAssociation(Params));
            if (!bOk) { MtlFail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("parameter not found: %s"), *Name)); return nullptr; }
            MIC->MarkPackageDirty();
            return MtlOk();
        });

    // material.set_texture_param — Params: instance_path, parameter_name, texture_path, [association].
    Registry.Register(TEXT("material.set_texture_param"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UMaterialInstanceConstant* MIC = MtlLoadTyped<UMaterialInstanceConstant>(Params, TEXT("instance_path"), OutError);
            if (!MIC) return nullptr;
            FString Name, TexPath;
            if (!MtlRequireString(Params, TEXT("parameter_name"), Name, OutError)) return nullptr;
            if (!MtlRequireString(Params, TEXT("texture_path"), TexPath, OutError)) return nullptr;
            UTexture* Tex = LoadObject<UTexture>(nullptr, *TexPath);
            if (!Tex) { MtlFail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("Could not load texture_path: %s"), *TexPath)); return nullptr; }

            const bool bOk = UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MIC, FName(*Name), Tex, ParseAssociation(Params));
            if (!bOk) { MtlFail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("parameter not found: %s"), *Name)); return nullptr; }
            MIC->MarkPackageDirty();
            return MtlOk();
        });

    // material.set_static_switch_param — Params: instance_path, parameter_name, value (bool), [association].
    Registry.Register(TEXT("material.set_static_switch_param"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UMaterialInstanceConstant* MIC = MtlLoadTyped<UMaterialInstanceConstant>(Params, TEXT("instance_path"), OutError);
            if (!MIC) return nullptr;
            FString Name; if (!MtlRequireString(Params, TEXT("parameter_name"), Name, OutError)) return nullptr;
            bool Value = false;
            if (!Params->TryGetBoolField(TEXT("value"), Value)) { MtlFail(OutError, FMCPError::InvalidParams, TEXT("value (bool) is required")); return nullptr; }

            const bool bOk = UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MIC, FName(*Name), Value, ParseAssociation(Params));
            if (!bOk) { MtlFail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("parameter not found: %s"), *Name)); return nullptr; }
            MIC->MarkPackageDirty();
            return MtlOk();
        });

    // NOTE (UE 5.3): material.set_parameter_override is omitted — UMaterialEditingLibrary lacks
    // SetMaterialInstanceParameterOverride / IsMaterialInstanceParameterOverridden until 5.8.

    // material.set_instance_parent — Params: instance_path, parent_path.
    Registry.Register(TEXT("material.set_instance_parent"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UMaterialInstanceConstant* MIC = MtlLoadTyped<UMaterialInstanceConstant>(Params, TEXT("instance_path"), OutError);
            if (!MIC) return nullptr;
            FString ParentPath; if (!MtlRequireString(Params, TEXT("parent_path"), ParentPath, OutError)) return nullptr;
            UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *ParentPath);
            if (!Parent) { MtlFail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("Could not load parent_path: %s"), *ParentPath)); return nullptr; }

            UMaterialEditingLibrary::SetMaterialInstanceParent(MIC, Parent);
            MIC->MarkPackageDirty();
            return MtlOk();
        });

    // material.clear_instance_parameters — Params: instance_path.
    Registry.Register(TEXT("material.clear_instance_parameters"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UMaterialInstanceConstant* MIC = MtlLoadTyped<UMaterialInstanceConstant>(Params, TEXT("instance_path"), OutError);
            if (!MIC) return nullptr;
            UMaterialEditingLibrary::ClearAllMaterialInstanceParameters(MIC);
            MIC->MarkPackageDirty();
            return MtlOk();
        });

    // material.update_instance — recompile shaders after instance edits. Params: instance_path.
    Registry.Register(TEXT("material.update_instance"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UMaterialInstanceConstant* MIC = MtlLoadTyped<UMaterialInstanceConstant>(Params, TEXT("instance_path"), OutError);
            if (!MIC) return nullptr;
            UMaterialEditingLibrary::UpdateMaterialInstance(MIC);
            MIC->MarkPackageDirty();
            return MtlOk();
        });

    // =====================================================================================
    // GRAPH EDITING
    // =====================================================================================

    // material.add_expression — Params: material_path OR function_path, expression_class,
    // [pos_x], [pos_y], [asset_path]. Returns { guid }.
    Registry.Register(TEXT("material.add_expression"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FGraphHost Host = LoadGraphHost(Params, OutError);
            if (!Host.IsValid()) return nullptr;
            FString ClassName; if (!MtlRequireString(Params, TEXT("expression_class"), ClassName, OutError)) return nullptr;
            UClass* ExprClass = ResolveExpressionClass(ClassName);
            if (!ExprClass) { MtlFail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("expression_class not found or not a UMaterialExpression: %s"), *ClassName)); return nullptr; }

            int32 PosX = 0, PosY = 0;
            Params->TryGetNumberField(TEXT("pos_x"), PosX);
            Params->TryGetNumberField(TEXT("pos_y"), PosY);

            // Optionally seed the node from an asset (e.g. a Texture for a TextureSample node).
            UObject* SelectedAsset = nullptr;
            FString AssetPath;
            if (Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !AssetPath.IsEmpty())
            {
                SelectedAsset = LoadObject<UObject>(nullptr, *AssetPath);
                if (!SelectedAsset) { MtlFail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("Could not load asset_path: %s"), *AssetPath)); return nullptr; }
            }

            // CreateMaterialExpressionEx adds to whichever host is non-null and seeds from SelectedAsset.
            UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpressionEx(
                Host.Material, Host.Function, ExprClass, SelectedAsset, PosX, PosY);
            if (!Expr) { MtlFail(OutError, FMCPError::InternalError, TEXT("CreateMaterialExpressionEx returned null")); return nullptr; }

            const FGuid Guid = EnsureExpressionGuid(Expr);
            Host.AsObject()->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("guid"), Guid.ToString());
            return Result;
        });

    // material.set_expression_property — reflection write on an expression node.
    // Params: material_path OR function_path, expression_guid, property, value (Unreal import text).
    Registry.Register(TEXT("material.set_expression_property"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FGraphHost Host = LoadGraphHost(Params, OutError);
            if (!Host.IsValid()) return nullptr;
            FString GuidStr, PropName, Value;
            if (!MtlRequireString(Params, TEXT("expression_guid"), GuidStr, OutError)) return nullptr;
            if (!MtlRequireString(Params, TEXT("property"), PropName, OutError)) return nullptr;
            if (!Params->TryGetStringField(TEXT("value"), Value)) { MtlFail(OutError, FMCPError::InvalidParams, TEXT("value is required")); return nullptr; }

            UMaterialExpression* Expr = ResolveExpression(Host, GuidStr, OutError);
            if (!Expr) return nullptr;
            if (!SetPropertyByReflection(Expr, PropName, Value, OutError)) return nullptr;
            Host.AsObject()->MarkPackageDirty();
            return MtlOk();
        });

    // material.connect_expressions — Params: material_path OR function_path, from_guid, [from_output],
    // to_guid, [to_input]. Empty pin names use the first pin.
    Registry.Register(TEXT("material.connect_expressions"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FGraphHost Host = LoadGraphHost(Params, OutError);
            if (!Host.IsValid()) return nullptr;
            FString FromGuid, ToGuid, FromOut, ToIn;
            if (!MtlRequireString(Params, TEXT("from_guid"), FromGuid, OutError)) return nullptr;
            if (!MtlRequireString(Params, TEXT("to_guid"), ToGuid, OutError)) return nullptr;
            Params->TryGetStringField(TEXT("from_output"), FromOut);
            Params->TryGetStringField(TEXT("to_input"), ToIn);

            UMaterialExpression* From = ResolveExpression(Host, FromGuid, OutError); if (!From) return nullptr;
            UMaterialExpression* To = ResolveExpression(Host, ToGuid, OutError); if (!To) return nullptr;

            const bool bOk = UMaterialEditingLibrary::ConnectMaterialExpressions(From, FromOut, To, ToIn);
            if (!bOk) { MtlFail(OutError, FMCPError::InvalidParams, TEXT("ConnectMaterialExpressions failed (check pin names)")); return nullptr; }
            Host.AsObject()->MarkPackageDirty();
            return MtlOk();
        });

    // material.connect_property — connect an expression output to a material property input.
    // Params: material_path, from_guid, [from_output], property (e.g. "BaseColor" / "MP_BaseColor").
    Registry.Register(TEXT("material.connect_property"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UMaterial* Mat = MtlLoadTyped<UMaterial>(Params, TEXT("material_path"), OutError);
            if (!Mat) return nullptr;
            FString FromGuid, FromOut, PropStr;
            if (!MtlRequireString(Params, TEXT("from_guid"), FromGuid, OutError)) return nullptr;
            if (!MtlRequireString(Params, TEXT("property"), PropStr, OutError)) return nullptr;
            Params->TryGetStringField(TEXT("from_output"), FromOut);

            EMaterialProperty Prop;
            if (!ParseMaterialProperty(PropStr, Prop)) { MtlFail(OutError, FMCPError::InvalidParams, FString::Printf(TEXT("unknown material property: %s"), *PropStr)); return nullptr; }

            FGraphHost Host; Host.Material = Mat;
            UMaterialExpression* From = ResolveExpression(Host, FromGuid, OutError); if (!From) return nullptr;

            const bool bOk = UMaterialEditingLibrary::ConnectMaterialProperty(From, FromOut, Prop);
            if (!bOk) { MtlFail(OutError, FMCPError::InvalidParams, TEXT("ConnectMaterialProperty failed (check output name)")); return nullptr; }
            Mat->MarkPackageDirty();
            return MtlOk();
        });

    // NOTE (UE 5.3): material.disconnect_expression and material.disconnect_property are omitted —
    // DisconnectMaterialExpressions / DisconnectMaterialProperty were added to UMaterialEditingLibrary in 5.8.

    // material.delete_expression — Params: material_path OR function_path, expression_guid.
    Registry.Register(TEXT("material.delete_expression"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FGraphHost Host = LoadGraphHost(Params, OutError);
            if (!Host.IsValid()) return nullptr;
            FString GuidStr; if (!MtlRequireString(Params, TEXT("expression_guid"), GuidStr, OutError)) return nullptr;
            UMaterialExpression* Expr = ResolveExpression(Host, GuidStr, OutError); if (!Expr) return nullptr;

            if (Host.Material) UMaterialEditingLibrary::DeleteMaterialExpression(Host.Material, Expr);
            else UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Host.Function, Expr);
            Host.AsObject()->MarkPackageDirty();
            return MtlOk();
        });

    // material.delete_all_expressions — Params: material_path OR function_path.
    Registry.Register(TEXT("material.delete_all_expressions"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FGraphHost Host = LoadGraphHost(Params, OutError);
            if (!Host.IsValid()) return nullptr;
            if (Host.Material) UMaterialEditingLibrary::DeleteAllMaterialExpressions(Host.Material);
            else UMaterialEditingLibrary::DeleteAllMaterialExpressionsInFunction(Host.Function);
            Host.AsObject()->MarkPackageDirty();
            return MtlOk();
        });

    // NOTE (UE 5.3): material.delete_unused_expressions is omitted — DeleteUnusedExpressions was added in 5.8.

    // material.layout_expressions — arrange nodes in a grid. Params: material_path OR function_path.
    Registry.Register(TEXT("material.layout_expressions"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FGraphHost Host = LoadGraphHost(Params, OutError);
            if (!Host.IsValid()) return nullptr;
            if (Host.Material) UMaterialEditingLibrary::LayoutMaterialExpressions(Host.Material);
            else UMaterialEditingLibrary::LayoutMaterialFunctionExpressions(Host.Function);
            Host.AsObject()->MarkPackageDirty();
            return MtlOk();
        });

    // =====================================================================================
    // NAMED REROUTE NODES (graph-local "named variables": Declaration + Usage)
    // =====================================================================================

    // material.add_named_reroute_declaration — create a Named Reroute Declaration (a graph-local named
    // variable). Feed it a value by connecting an expression to its input (connect_expressions, empty
    // to_input). Params: material_path OR function_path, name, [pos_x], [pos_y], [node_color [r,g,b,a]].
    // Returns { guid, variable_guid, name }.
    Registry.Register(TEXT("material.add_named_reroute_declaration"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FGraphHost Host = LoadGraphHost(Params, OutError);
            if (!Host.IsValid()) return nullptr;
            FString Name; if (!MtlRequireString(Params, TEXT("name"), Name, OutError)) return nullptr;

            int32 PosX = 0, PosY = 0;
            Params->TryGetNumberField(TEXT("pos_x"), PosX);
            Params->TryGetNumberField(TEXT("pos_y"), PosY);

            UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpressionEx(
                Host.Material, Host.Function, UMaterialExpressionNamedRerouteDeclaration::StaticClass(), nullptr, PosX, PosY);
            UMaterialExpressionNamedRerouteDeclaration* Decl = Cast<UMaterialExpressionNamedRerouteDeclaration>(Expr);
            if (!Decl) { MtlFail(OutError, FMCPError::InternalError, TEXT("failed to create Named Reroute Declaration")); return nullptr; }

            Decl->Modify();
            Decl->Name = FName(*Name);
            if (!Decl->VariableGuid.IsValid()) Decl->VariableGuid = FGuid::NewGuid();

            // node_color is optional — only read it if the field is present.
            const TArray<TSharedPtr<FJsonValue>>* ColorArr = nullptr;
            if (Params->TryGetArrayField(TEXT("node_color"), ColorArr))
            {
                FLinearColor Color;
                if (!ReadLinearColor(Params, TEXT("node_color"), Color, OutError)) return nullptr;
                Decl->NodeColor = Color;
            }

            const FGuid Guid = EnsureExpressionGuid(Decl);
            Host.AsObject()->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("guid"), Guid.ToString());
            Result->SetStringField(TEXT("variable_guid"), Decl->VariableGuid.ToString());
            Result->SetStringField(TEXT("name"), Decl->Name.ToString());
            return Result;
        });

    // material.add_named_reroute_usage — create a Named Reroute Usage that reads a declaration. Wire its
    // output onward with connect_expressions. Params: material_path OR function_path,
    // declaration_guid (the declaration NODE's guid), [pos_x], [pos_y]. Returns { guid }.
    Registry.Register(TEXT("material.add_named_reroute_usage"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FGraphHost Host = LoadGraphHost(Params, OutError);
            if (!Host.IsValid()) return nullptr;
            FString DeclGuid; if (!MtlRequireString(Params, TEXT("declaration_guid"), DeclGuid, OutError)) return nullptr;

            UMaterialExpression* DeclExpr = ResolveExpression(Host, DeclGuid, OutError); if (!DeclExpr) return nullptr;
            UMaterialExpressionNamedRerouteDeclaration* Decl = Cast<UMaterialExpressionNamedRerouteDeclaration>(DeclExpr);
            if (!Decl) { MtlFail(OutError, FMCPError::InvalidParams, TEXT("declaration_guid does not reference a Named Reroute Declaration")); return nullptr; }

            int32 PosX = 0, PosY = 0;
            Params->TryGetNumberField(TEXT("pos_x"), PosX);
            Params->TryGetNumberField(TEXT("pos_y"), PosY);

            UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpressionEx(
                Host.Material, Host.Function, UMaterialExpressionNamedRerouteUsage::StaticClass(), nullptr, PosX, PosY);
            UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(Expr);
            if (!Usage) { MtlFail(OutError, FMCPError::InternalError, TEXT("failed to create Named Reroute Usage")); return nullptr; }

            Usage->Modify();
            Usage->Declaration = Decl;
            Usage->DeclarationGuid = Decl->VariableGuid;

            const FGuid Guid = EnsureExpressionGuid(Usage);
            Host.AsObject()->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("guid"), Guid.ToString());
            return Result;
        });

    // material.set_named_reroute_name — rename a Named Reroute Declaration. Linked usages resolve by GUID
    // and reflect the new name automatically. Params: material_path OR function_path, expression_guid, name.
    Registry.Register(TEXT("material.set_named_reroute_name"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FGraphHost Host = LoadGraphHost(Params, OutError);
            if (!Host.IsValid()) return nullptr;
            FString GuidStr, Name;
            if (!MtlRequireString(Params, TEXT("expression_guid"), GuidStr, OutError)) return nullptr;
            if (!MtlRequireString(Params, TEXT("name"), Name, OutError)) return nullptr;

            UMaterialExpression* Expr = ResolveExpression(Host, GuidStr, OutError); if (!Expr) return nullptr;
            UMaterialExpressionNamedRerouteDeclaration* Decl = Cast<UMaterialExpressionNamedRerouteDeclaration>(Expr);
            if (!Decl) { MtlFail(OutError, FMCPError::InvalidParams, TEXT("expression_guid does not reference a Named Reroute Declaration")); return nullptr; }

            Decl->Modify();
            Decl->Name = FName(*Name);
            Host.AsObject()->MarkPackageDirty();
            return MtlOk();
        });

    // material.list_named_reroutes — list Named Reroute declarations and usages with their linkage.
    // Params: material_path OR function_path. Returns { declarations[], usages[] }.
    Registry.Register(TEXT("material.list_named_reroutes"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FGraphHost Host = LoadGraphHost(Params, OutError);
            if (!Host.IsValid()) return nullptr;

            const TArray<UMaterialExpression*> All = MtlGetExpressions(Host);
            TArray<TSharedPtr<FJsonValue>> Decls, Usages;

            for (UMaterialExpression* E : All)
            {
                if (UMaterialExpressionNamedRerouteDeclaration* D = Cast<UMaterialExpressionNamedRerouteDeclaration>(E))
                {
                    auto O = MakeShared<FJsonObject>();
                    O->SetStringField(TEXT("guid"), EnsureExpressionGuid(D).ToString());
                    O->SetStringField(TEXT("name"), D->Name.ToString());
                    O->SetStringField(TEXT("variable_guid"), D->VariableGuid.ToString());
                    O->SetField(TEXT("node_color"), LinearColorToJson(D->NodeColor));
                    Decls.Add(MakeShared<FJsonValueObject>(O));
                }
            }
            for (UMaterialExpression* E : All)
            {
                if (UMaterialExpressionNamedRerouteUsage* U = Cast<UMaterialExpressionNamedRerouteUsage>(E))
                {
                    auto O = MakeShared<FJsonObject>();
                    O->SetStringField(TEXT("guid"), EnsureExpressionGuid(U).ToString());
                    UMaterialExpressionNamedRerouteDeclaration* D = U->Declaration;
                    O->SetStringField(TEXT("declaration_name"), D ? D->Name.ToString() : TEXT(""));
                    O->SetStringField(TEXT("declaration_guid"), D ? EnsureExpressionGuid(D).ToString() : TEXT(""));
                    O->SetStringField(TEXT("declaration_variable_guid"), U->DeclarationGuid.ToString());
                    Usages.Add(MakeShared<FJsonValueObject>(O));
                }
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("declarations"), Decls);
            Result->SetArrayField(TEXT("usages"), Usages);
            return Result;
        });

    // =====================================================================================
    // COMMENT BOXES (UMaterialExpressionComment)
    // =====================================================================================

    // material.add_comment — add a comment box. Params: material_path OR function_path, text,
    // [pos_x], [pos_y], [size_x=400], [size_y=200], [font_size=18], [color [r,g,b,a]]. Returns { guid }.
    Registry.Register(TEXT("material.add_comment"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FGraphHost Host = LoadGraphHost(Params, OutError);
            if (!Host.IsValid()) return nullptr;
            FString Text; if (!MtlRequireString(Params, TEXT("text"), Text, OutError)) return nullptr;

            int32 PosX = 0, PosY = 0, SizeX = 400, SizeY = 200, FontSize = 18;
            Params->TryGetNumberField(TEXT("pos_x"), PosX);
            Params->TryGetNumberField(TEXT("pos_y"), PosY);
            Params->TryGetNumberField(TEXT("size_x"), SizeX);
            Params->TryGetNumberField(TEXT("size_y"), SizeY);
            Params->TryGetNumberField(TEXT("font_size"), FontSize);

            UMaterialExpressionComment* Comment = NewObject<UMaterialExpressionComment>(
                Host.AsObject(), UMaterialExpressionComment::StaticClass(), NAME_None, RF_Transactional);
            if (!Comment) { MtlFail(OutError, FMCPError::InternalError, TEXT("failed to create comment")); return nullptr; }

            Comment->MaterialExpressionEditorX = PosX;
            Comment->MaterialExpressionEditorY = PosY;
            Comment->SizeX = SizeX;
            Comment->SizeY = SizeY;
            Comment->Text = Text;
            Comment->FontSize = FontSize;

            const TArray<TSharedPtr<FJsonValue>>* ColorArr = nullptr;
            if (Params->TryGetArrayField(TEXT("color"), ColorArr))
            {
                FLinearColor Color;
                if (!ReadLinearColor(Params, TEXT("color"), Color, OutError)) return nullptr;
                Comment->CommentColor = Color;
            }

            if (!Comment->MaterialExpressionGuid.IsValid()) Comment->MaterialExpressionGuid = FGuid::NewGuid();

            Host.AsObject()->Modify();
            if (Host.Material) Host.Material->GetExpressionCollection().AddComment(Comment);
            else Host.Function->GetExpressionCollection().AddComment(Comment);
            Host.AsObject()->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("guid"), Comment->MaterialExpressionGuid.ToString());
            return Result;
        });

    // material.list_comments — list comment boxes. Params: material_path OR function_path.
    Registry.Register(TEXT("material.list_comments"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FGraphHost Host = LoadGraphHost(Params, OutError);
            if (!Host.IsValid()) return nullptr;

            const TConstArrayView<TObjectPtr<UMaterialExpressionComment>> Comments =
                Host.Material ? Host.Material->GetEditorComments() : Host.Function->GetEditorComments();

            TArray<TSharedPtr<FJsonValue>> Arr;
            for (UMaterialExpressionComment* C : Comments)
            {
                if (!C) continue;
                if (!C->MaterialExpressionGuid.IsValid()) C->MaterialExpressionGuid = FGuid::NewGuid();
                auto O = MakeShared<FJsonObject>();
                O->SetStringField(TEXT("guid"), C->MaterialExpressionGuid.ToString());
                O->SetStringField(TEXT("text"), C->Text);
                O->SetNumberField(TEXT("pos_x"), C->MaterialExpressionEditorX);
                O->SetNumberField(TEXT("pos_y"), C->MaterialExpressionEditorY);
                O->SetNumberField(TEXT("size_x"), C->SizeX);
                O->SetNumberField(TEXT("size_y"), C->SizeY);
                O->SetNumberField(TEXT("font_size"), C->FontSize);
                O->SetField(TEXT("color"), LinearColorToJson(C->CommentColor));
                Arr.Add(MakeShared<FJsonValueObject>(O));
            }
            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("comments"), Arr);
            return Result;
        });

    // material.set_comment_text — set a comment's text (direct assignment; import text avoided so commas
    // and newlines survive). Params: material_path OR function_path, comment_guid, text.
    Registry.Register(TEXT("material.set_comment_text"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FGraphHost Host = LoadGraphHost(Params, OutError);
            if (!Host.IsValid()) return nullptr;
            FString GuidStr, Text;
            if (!MtlRequireString(Params, TEXT("comment_guid"), GuidStr, OutError)) return nullptr;
            if (!Params->TryGetStringField(TEXT("text"), Text)) { MtlFail(OutError, FMCPError::InvalidParams, TEXT("text is required")); return nullptr; }

            UMaterialExpressionComment* Comment = ResolveComment(Host, GuidStr, OutError);
            if (!Comment) return nullptr;
            Comment->Modify();
            Comment->Text = Text;
            Host.AsObject()->MarkPackageDirty();
            return MtlOk();
        });

    // material.delete_comment — remove a comment box. Params: material_path OR function_path, comment_guid.
    Registry.Register(TEXT("material.delete_comment"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FGraphHost Host = LoadGraphHost(Params, OutError);
            if (!Host.IsValid()) return nullptr;
            FString GuidStr; if (!MtlRequireString(Params, TEXT("comment_guid"), GuidStr, OutError)) return nullptr;

            UMaterialExpressionComment* Comment = ResolveComment(Host, GuidStr, OutError);
            if (!Comment) return nullptr;

            Host.AsObject()->Modify();
            if (Host.Material) Host.Material->GetExpressionCollection().RemoveComment(Comment);
            else Host.Function->GetExpressionCollection().RemoveComment(Comment);
            Host.AsObject()->MarkPackageDirty();
            return MtlOk();
        });

    // =====================================================================================
    // BASE PROPERTIES / BUILD
    // =====================================================================================

    // material.set_property — reflection write on a material / instance / function object itself
    // (e.g. BlendMode, ShadingModel, TwoSided). Params: object_path, property, value (Unreal import text).
    Registry.Register(TEXT("material.set_property"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UObject* Obj = MtlLoadTyped<UObject>(Params, TEXT("object_path"), OutError);
            if (!Obj) return nullptr;
            FString PropName, Value;
            if (!MtlRequireString(Params, TEXT("property"), PropName, OutError)) return nullptr;
            if (!Params->TryGetStringField(TEXT("value"), Value)) { MtlFail(OutError, FMCPError::InvalidParams, TEXT("value is required")); return nullptr; }

            if (!SetPropertyByReflection(Obj, PropName, Value, OutError)) return nullptr;
            return MtlOk();
        });

    // material.recompile — recompile a material after graph edits. Params: material_path.
    // UE 5.3: RecompileMaterial returns void (no compiler error list; that was added in 5.8), so
    // "errors" is always empty and "compiled" is reported true once the call returns.
    Registry.Register(TEXT("material.recompile"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UMaterial* Mat = MtlLoadTyped<UMaterial>(Params, TEXT("material_path"), OutError);
            if (!Mat) return nullptr;

            UMaterialEditingLibrary::RecompileMaterial(Mat);
            Mat->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("compiled"), true);
            Result->SetArrayField(TEXT("errors"), TArray<TSharedPtr<FJsonValue>>());
            return Result;
        });

    // material.update_function — recompile materials that use a function after edits. Params: function_path.
    Registry.Register(TEXT("material.update_function"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UMaterialFunction* Func = MtlLoadTyped<UMaterialFunction>(Params, TEXT("function_path"), OutError);
            if (!Func) return nullptr;
            UMaterialEditingLibrary::UpdateMaterialFunction(Func, nullptr);
            Func->MarkPackageDirty();
            return MtlOk();
        });

    // =====================================================================================
    // PLACEMENT / LAYOUT HELPERS  (16px grid; left->right columns, top->down rows)
    // Convention source: /Game/02_Geo/MasterMaterial (see material-graph-conventions doc).
    // =====================================================================================

    // material.set_node_position — move an expression node to a grid-snapped position.
    // Params: material_path OR function_path, expression_guid, pos_x, pos_y, [snap=true (16px)].
    Registry.Register(TEXT("material.set_node_position"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FGraphHost Host = LoadGraphHost(Params, OutError);
            if (!Host.IsValid()) return nullptr;
            FString GuidStr; if (!MtlRequireString(Params, TEXT("expression_guid"), GuidStr, OutError)) return nullptr;
            int32 PosX = 0, PosY = 0;
            Params->TryGetNumberField(TEXT("pos_x"), PosX);
            Params->TryGetNumberField(TEXT("pos_y"), PosY);
            bool bSnap = true; Params->TryGetBoolField(TEXT("snap"), bSnap);

            UMaterialExpression* Expr = ResolveExpression(Host, GuidStr, OutError);
            if (!Expr) return nullptr;
            if (bSnap) { PosX = FMath::RoundToInt(PosX / 16.0f) * 16; PosY = FMath::RoundToInt(PosY / 16.0f) * 16; }
            Expr->MaterialExpressionEditorX = PosX;
            Expr->MaterialExpressionEditorY = PosY;
            Host.AsObject()->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("pos_x"), PosX);
            Result->SetNumberField(TEXT("pos_y"), PosY);
            return Result;
        });

    // material.arrange_grid — the fast placement formula. Places nodes on a left->right column /
    // top->down row grid. Params: material_path OR function_path, columns (array of arrays of expression
    // guids), [origin_x=0], [origin_y=0], [col_step=256], [row_step=80], [snap=true].
    // columns[i][j] -> X = origin_x + i*col_step, Y = origin_y + j*row_step (snapped 16).
    Registry.Register(TEXT("material.arrange_grid"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FGraphHost Host = LoadGraphHost(Params, OutError);
            if (!Host.IsValid()) return nullptr;

            const TArray<TSharedPtr<FJsonValue>>* Columns = nullptr;
            if (!Params->TryGetArrayField(TEXT("columns"), Columns) || !Columns)
            {
                MtlFail(OutError, FMCPError::InvalidParams, TEXT("columns (array of arrays of guids) is required"));
                return nullptr;
            }
            int32 OriginX = 0, OriginY = 0, ColStep = 256, RowStep = 80;
            Params->TryGetNumberField(TEXT("origin_x"), OriginX);
            Params->TryGetNumberField(TEXT("origin_y"), OriginY);
            Params->TryGetNumberField(TEXT("col_step"), ColStep);
            Params->TryGetNumberField(TEXT("row_step"), RowStep);
            bool bSnap = true; Params->TryGetBoolField(TEXT("snap"), bSnap);
            auto Snap = [bSnap](int32 V) { return bSnap ? FMath::RoundToInt(V / 16.0f) * 16 : V; };

            TArray<TSharedPtr<FJsonValue>> Placed;
            for (int32 i = 0; i < Columns->Num(); ++i)
            {
                const TArray<TSharedPtr<FJsonValue>>* Col = nullptr;
                if (!(*Columns)[i]->TryGetArray(Col) || !Col) continue;
                for (int32 j = 0; j < Col->Num(); ++j)
                {
                    const FString GuidStr = (*Col)[j]->AsString();
                    UMaterialExpression* Expr = ResolveExpression(Host, GuidStr, OutError);
                    if (!Expr) return nullptr;
                    const int32 X = Snap(OriginX + i * ColStep);
                    const int32 Y = Snap(OriginY + j * RowStep);
                    Expr->MaterialExpressionEditorX = X;
                    Expr->MaterialExpressionEditorY = Y;
                    auto O = MakeShared<FJsonObject>();
                    O->SetStringField(TEXT("guid"), GuidStr);
                    O->SetNumberField(TEXT("pos_x"), X);
                    O->SetNumberField(TEXT("pos_y"), Y);
                    Placed.Add(MakeShared<FJsonValueObject>(O));
                }
            }
            Host.AsObject()->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("placed"), Placed.Num());
            Result->SetArrayField(TEXT("positions"), Placed);
            return Result;
        });

    // material.add_channel_reroutes — create the standard PBR channel Named Reroute declarations with the
    // project's fixed channel colors, stacked vertically. Params: material_path OR function_path,
    // [channels (array of names; default the 7 standard)], [origin_x=0], [origin_y=0], [row_step=80].
    // Standard palette: AO/Diffuse/F0/Roughness/Normal/Emissive/UVs. Returns { declarations }.
    Registry.Register(TEXT("material.add_channel_reroutes"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FGraphHost Host = LoadGraphHost(Params, OutError);
            if (!Host.IsValid()) return nullptr;

            struct FChan { const TCHAR* Name; FLinearColor Color; };
            static const FChan Std[] = {
                { TEXT("AO"),        FLinearColor(1.0f,   0.0f,   0.094f) },
                { TEXT("Diffuse"),   FLinearColor(0.0f,   0.414f, 1.0f)   },
                { TEXT("F0"),        FLinearColor(0.734f, 1.0f,   0.0f)   },
                { TEXT("Roughness"), FLinearColor(0.945f, 0.0f,   1.0f)   },
                { TEXT("Normal"),    FLinearColor(0.0f,   1.0f,   0.625f) },
                { TEXT("Emissive"),  FLinearColor(1.0f,   0.305f, 0.0f)   },
                { TEXT("UVs"),       FLinearColor(0.0f,   0.016f, 1.0f)   },
            };

            int32 OriginX = 0, OriginY = 0, RowStep = 80;
            Params->TryGetNumberField(TEXT("origin_x"), OriginX);
            Params->TryGetNumberField(TEXT("origin_y"), OriginY);
            Params->TryGetNumberField(TEXT("row_step"), RowStep);

            TArray<FString> Names;
            const TArray<TSharedPtr<FJsonValue>>* ChArr = nullptr;
            if (Params->TryGetArrayField(TEXT("channels"), ChArr) && ChArr)
            {
                for (const TSharedPtr<FJsonValue>& V : *ChArr) Names.Add(V->AsString());
            }
            else
            {
                for (const FChan& C : Std) Names.Add(C.Name);
            }

            auto ColorFor = [&](const FString& N, FLinearColor& Out) -> bool {
                for (const FChan& C : Std) { if (N.Equals(C.Name, ESearchCase::IgnoreCase)) { Out = C.Color; return true; } }
                return false;
            };

            const int32 X = FMath::RoundToInt(OriginX / 16.0f) * 16;
            TArray<TSharedPtr<FJsonValue>> Decls;
            int32 Row = 0;
            for (const FString& N : Names)
            {
                FLinearColor Color(0.0f, 0.0f, 0.0f, 1.0f);
                ColorFor(N, Color); // unknown name -> black, still created
                const int32 Y = FMath::RoundToInt((OriginY + Row * RowStep) / 16.0f) * 16;

                UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpressionEx(
                    Host.Material, Host.Function, UMaterialExpressionNamedRerouteDeclaration::StaticClass(), nullptr, X, Y);
                UMaterialExpressionNamedRerouteDeclaration* Decl = Cast<UMaterialExpressionNamedRerouteDeclaration>(Expr);
                if (!Decl) { MtlFail(OutError, FMCPError::InternalError, TEXT("failed to create channel reroute")); return nullptr; }
                Decl->Modify();
                Decl->Name = FName(*N);
                Decl->NodeColor = Color;
                if (!Decl->VariableGuid.IsValid()) Decl->VariableGuid = FGuid::NewGuid();

                auto O = MakeShared<FJsonObject>();
                O->SetStringField(TEXT("name"), N);
                O->SetStringField(TEXT("guid"), EnsureExpressionGuid(Decl).ToString());
                O->SetStringField(TEXT("variable_guid"), Decl->VariableGuid.ToString());
                Decls.Add(MakeShared<FJsonValueObject>(O));
                ++Row;
            }
            Host.AsObject()->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("declarations"), Decls);
            return Result;
        });

    // material.add_group_comment — group-box comment auto-sized around a set of expression nodes, using the
    // project's standard group style (dark gray 0.15/0.5, font 18). Params: material_path OR function_path,
    // text, node_guids (array), [padding=48], [font_size=18], [color [r,g,b,a]]. Returns { guid, box }.
    Registry.Register(TEXT("material.add_group_comment"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FGraphHost Host = LoadGraphHost(Params, OutError);
            if (!Host.IsValid()) return nullptr;
            FString Text; if (!MtlRequireString(Params, TEXT("text"), Text, OutError)) return nullptr;
            const TArray<TSharedPtr<FJsonValue>>* Guids = nullptr;
            if (!Params->TryGetArrayField(TEXT("node_guids"), Guids) || !Guids || Guids->Num() == 0)
            {
                MtlFail(OutError, FMCPError::InvalidParams, TEXT("node_guids (non-empty array) is required"));
                return nullptr;
            }
            int32 Padding = 48, FontSize = 18;
            Params->TryGetNumberField(TEXT("padding"), Padding);
            Params->TryGetNumberField(TEXT("font_size"), FontSize);

            // Positions are node top-left corners; sizes are not exposed, so approximate node extent.
            const int32 NodeW = 200, NodeH = 128, HeaderTop = 48;
            bool bAny = false; int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
            for (const TSharedPtr<FJsonValue>& V : *Guids)
            {
                UMaterialExpression* Expr = ResolveExpression(Host, V->AsString(), OutError);
                if (!Expr) return nullptr;
                const int32 X = Expr->MaterialExpressionEditorX, Y = Expr->MaterialExpressionEditorY;
                if (!bAny) { MinX = MaxX = X; MinY = MaxY = Y; bAny = true; }
                MinX = FMath::Min(MinX, X); MinY = FMath::Min(MinY, Y);
                MaxX = FMath::Max(MaxX, X); MaxY = FMath::Max(MaxY, Y);
            }

            auto Snap = [](int32 V) { return FMath::RoundToInt(V / 16.0f) * 16; };
            const int32 BoxX = Snap(MinX - Padding);
            const int32 BoxY = Snap(MinY - Padding - HeaderTop);
            const int32 BoxW = Snap((MaxX + NodeW + Padding) - BoxX);
            const int32 BoxH = Snap((MaxY + NodeH + Padding) - BoxY);

            FLinearColor Color(0.15f, 0.15f, 0.15f, 0.5f);
            const TArray<TSharedPtr<FJsonValue>>* ColorArr = nullptr;
            if (Params->TryGetArrayField(TEXT("color"), ColorArr)) { if (!ReadLinearColor(Params, TEXT("color"), Color, OutError)) return nullptr; }

            UMaterialExpressionComment* Comment = NewObject<UMaterialExpressionComment>(
                Host.AsObject(), UMaterialExpressionComment::StaticClass(), NAME_None, RF_Transactional);
            if (!Comment) { MtlFail(OutError, FMCPError::InternalError, TEXT("failed to create comment")); return nullptr; }
            Comment->MaterialExpressionEditorX = BoxX;
            Comment->MaterialExpressionEditorY = BoxY;
            Comment->SizeX = BoxW;
            Comment->SizeY = BoxH;
            Comment->Text = Text;
            Comment->FontSize = FontSize;
            Comment->CommentColor = Color;
            if (!Comment->MaterialExpressionGuid.IsValid()) Comment->MaterialExpressionGuid = FGuid::NewGuid();

            Host.AsObject()->Modify();
            if (Host.Material) Host.Material->GetExpressionCollection().AddComment(Comment);
            else Host.Function->GetExpressionCollection().AddComment(Comment);
            Host.AsObject()->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("guid"), Comment->MaterialExpressionGuid.ToString());
            Result->SetNumberField(TEXT("pos_x"), BoxX);
            Result->SetNumberField(TEXT("pos_y"), BoxY);
            Result->SetNumberField(TEXT("size_x"), BoxW);
            Result->SetNumberField(TEXT("size_y"), BoxH);
            return Result;
        });
}
