#include "Commands/AssetSearchCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Modules/ModuleManager.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimSequence.h"

namespace MCPAssetSearch
{
    using UnrealMCPBridge::Json::RequireString;
    using UnrealMCPBridge::Json::MakeVector3JsonArray;

    void Fail(MCPProtocol::FMCPError& OutError, const FString& Message)
    {
        OutError.Code = MCPProtocol::FMCPError::InvalidParams;
        OutError.Message = Message;
    }

    IAssetRegistry& Registry()
    {
        return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    }

    /** "StaticMesh", "/Script/Engine.StaticMesh" or a Blueprint-class short name -> class path. */
    bool ResolveClassPath(const FString& ClassName, FTopLevelAssetPath& Out, FString& OutError)
    {
        if (ClassName.StartsWith(TEXT("/")))
        {
            Out = FTopLevelAssetPath(ClassName);
            return Out.IsValid();
        }
        if (const UClass* Class = UClass::TryFindTypeSlow<UClass>(ClassName, EFindFirstObjectOptions::NativeFirst))
        {
            Out = Class->GetClassPathName();
            return true;
        }
        OutError = FString::Printf(TEXT("Unknown class '%s' — use the short native name (StaticMesh, SkeletalMesh, AnimSequence, Blueprint, ...) or a /Script/ path"), *ClassName);
        return false;
    }

    /** Space-separated words, all of which must appear in Name (case-insensitive). */
    bool MatchesQuery(const FString& Name, const TArray<FString>& Words)
    {
        for (const FString& Word : Words)
        {
            if (!Name.Contains(Word, ESearchCase::IgnoreCase))
            {
                return false;
            }
        }
        return true;
    }

    TSharedPtr<FJsonObject> MakeFoundAssetJson(const FAssetData& Asset, bool bIncludeSize)
    {
        TSharedPtr<FJsonObject> Obj = UnrealMCPBridge::Json::MakeAssetJson(Asset);
        Obj->SetStringField(TEXT("package_path"), Asset.PackagePath.ToString());

        FString Skeleton;
        if (Asset.GetTagValue(TEXT("Skeleton"), Skeleton))
        {
            Obj->SetStringField(TEXT("skeleton"), FPackageName::ExportTextPathToObjectPath(Skeleton));
        }
        if (bIncludeSize)
        {
            const TOptional<FAssetPackageData> PackageData = Registry().GetAssetPackageDataCopy(Asset.PackageName);
            if (PackageData.IsSet())
            {
                Obj->SetNumberField(TEXT("disk_size"), static_cast<double>(PackageData->DiskSize));
            }
        }
        return Obj;
    }

    /** Bounds report shared by static and skeletal meshes. */
    TSharedPtr<FJsonObject> MakeMeshBoundsJson(const UObject* Mesh, const FBoxSphereBounds& Bounds)
    {
        const FBox Box = Bounds.GetBox();
        auto Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("asset"), Mesh->GetPathName());
        Obj->SetStringField(TEXT("class"), Mesh->GetClass()->GetName());
        Obj->SetField(TEXT("center"), MakeVector3JsonArray(Box.GetCenter()));
        Obj->SetField(TEXT("extent"), MakeVector3JsonArray(Box.GetExtent()));
        Obj->SetField(TEXT("size_m"), MakeVector3JsonArray(Box.GetSize() / 100.0));
        Obj->SetField(TEXT("min"), MakeVector3JsonArray(Box.Min));
        Obj->SetField(TEXT("max"), MakeVector3JsonArray(Box.Max));
        Obj->SetNumberField(TEXT("sphere_radius"), Bounds.SphereRadius);
        // How far the bottom sits below the pivot — add it to a ground height to rest the mesh on it.
        Obj->SetNumberField(TEXT("pivot_to_bottom"), -Box.Min.Z);
        return Obj;
    }

    int32 ReadMax(const TSharedPtr<FJsonObject>& Params, int32 Default)
    {
        double Max = Default;
        Params->TryGetNumberField(TEXT("max"), Max);
        return FMath::Clamp(static_cast<int32>(Max), 1, 20000);
    }
}

void FAssetSearchCommandHandler::RegisterCommands(FMCPCommandRegistry& CommandRegistry)
{
    using namespace MCPAssetSearch;

    // asset.find — assets whose name contains every word of query. Read-only.
    // Params: query (space-separated, case-insensitive), class (StaticMesh / SkeletalMesh /
    // AnimSequence / Blueprint / any class, subclasses included), path (default /Game),
    // max (default 100), include_size (disk size, default false). query or class is required.
    // Returns { assets: [{name, class, object_path, package_path, skeleton?, disk_size?}], total, truncated }.
    CommandRegistry.Register(TEXT("asset.find"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString Query, ClassName, Path = TEXT("/Game");
            bool bIncludeSize = false;
            if (Params.IsValid())
            {
                Params->TryGetStringField(TEXT("query"), Query);
                Params->TryGetStringField(TEXT("class"), ClassName);
                Params->TryGetStringField(TEXT("path"), Path);
                Params->TryGetBoolField(TEXT("include_size"), bIncludeSize);
            }
            if (Query.IsEmpty() && ClassName.IsEmpty())
            {
                Fail(OutError, TEXT("query or class is required"));
                return nullptr;
            }

            FARFilter Filter;
            Filter.PackagePaths.Add(FName(*Path));
            Filter.bRecursivePaths = true;
            if (!ClassName.IsEmpty())
            {
                FTopLevelAssetPath ClassPath;
                FString Error;
                if (!ResolveClassPath(ClassName, ClassPath, Error))
                {
                    Fail(OutError, Error);
                    return nullptr;
                }
                Filter.ClassPaths.Add(ClassPath);
                Filter.bRecursiveClasses = true;
            }

            TArray<FAssetData> Assets;
            Registry().GetAssets(Filter, Assets);

            TArray<FString> Words;
            Query.ParseIntoArrayWS(Words);
            Assets.RemoveAll([&Words](const FAssetData& Asset) { return !MatchesQuery(Asset.AssetName.ToString(), Words); });
            Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.AssetName.LexicalLess(B.AssetName); });

            const int32 Max = ReadMax(Params, 100);
            TArray<TSharedPtr<FJsonValue>> Found;
            for (int32 Index = 0; Index < Assets.Num() && Index < Max; ++Index)
            {
                Found.Add(MakeShared<FJsonValueObject>(MakeFoundAssetJson(Assets[Index], bIncludeSize)));
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("assets"), Found);
            Result->SetNumberField(TEXT("total"), Assets.Num());
            Result->SetBoolField(TEXT("truncated"), Assets.Num() > Max);
            return Result;
        });

    // asset.bounds — size of static / skeletal meshes, loading them if needed. Read-only.
    // Params: assets ([object paths]) or asset (one path).
    // Returns { bounds: [{asset, class, center, extent, size_m, min, max, sphere_radius,
    // pivot_to_bottom} | {asset, error}] }.
    CommandRegistry.Register(TEXT("asset.bounds"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            TArray<FString> Paths;
            if (Params.IsValid())
            {
                FString Single;
                if (Params->TryGetStringField(TEXT("asset"), Single) && !Single.IsEmpty())
                {
                    Paths.Add(Single);
                }
                const TArray<TSharedPtr<FJsonValue>>* Many = nullptr;
                if (Params->TryGetArrayField(TEXT("assets"), Many) && Many)
                {
                    for (const TSharedPtr<FJsonValue>& Value : *Many)
                    {
                        Paths.Add(Value->AsString());
                    }
                }
            }
            if (Paths.Num() == 0)
            {
                Fail(OutError, TEXT("asset or assets is required"));
                return nullptr;
            }

            TArray<TSharedPtr<FJsonValue>> Bounds;
            for (const FString& Path : Paths)
            {
                UObject* Asset = LoadObject<UObject>(nullptr, *Path);
                TSharedPtr<FJsonObject> Entry;
                if (const UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
                {
                    Entry = MakeMeshBoundsJson(StaticMesh, StaticMesh->GetBounds());
                }
                else if (const USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset))
                {
                    // Imported bounds = the reference pose, without the editor's bounds extension.
                    Entry = MakeMeshBoundsJson(SkeletalMesh, SkeletalMesh->GetImportedBounds());
                }
                else
                {
                    Entry = MakeShared<FJsonObject>();
                    Entry->SetStringField(TEXT("asset"), Path);
                    Entry->SetStringField(TEXT("error"), Asset ? FString::Printf(TEXT("%s is not a mesh"), *Asset->GetClass()->GetName()) : TEXT("could not load"));
                }
                Bounds.Add(MakeShared<FJsonValueObject>(Entry));
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("bounds"), Bounds);
            return Result;
        });

    // asset.anims_for_skeleton — animation assets a skeleton (or a skeletal mesh's skeleton) can
    // play, compatible skeletons included. Read-only, from the asset registry.
    // Params: skeleton or mesh (path, one required), classes (default ["AnimSequence"]; e.g.
    // AnimMontage, BlendSpace), query (name words), path (default /Game), max (default 500).
    // Returns { skeleton, anims: [{name, class, object_path, skeleton, frames?, frame_rate?, length?}], total, truncated }.
    CommandRegistry.Register(TEXT("asset.anims_for_skeleton"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            if (!Params.IsValid())
            {
                Fail(OutError, TEXT("skeleton or mesh is required"));
                return nullptr;
            }

            FString SkeletonPath, MeshPath;
            Params->TryGetStringField(TEXT("skeleton"), SkeletonPath);
            Params->TryGetStringField(TEXT("mesh"), MeshPath);
            USkeleton* Skeleton = nullptr;
            if (!SkeletonPath.IsEmpty())
            {
                Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
            }
            else if (!MeshPath.IsEmpty())
            {
                if (USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath))
                {
                    Skeleton = Mesh->GetSkeleton();
                }
            }
            if (!Skeleton)
            {
                Fail(OutError, FString::Printf(TEXT("Could not load a skeleton from %s"), SkeletonPath.IsEmpty() ? *MeshPath : *SkeletonPath));
                return nullptr;
            }

            FARFilter Filter;
            FString Path = TEXT("/Game");
            Params->TryGetStringField(TEXT("path"), Path);
            Filter.PackagePaths.Add(FName(*Path));
            Filter.bRecursivePaths = true;
            Filter.bRecursiveClasses = true;
            const TArray<TSharedPtr<FJsonValue>>* Classes = nullptr;
            if (Params->TryGetArrayField(TEXT("classes"), Classes) && Classes && Classes->Num() > 0)
            {
                for (const TSharedPtr<FJsonValue>& Value : *Classes)
                {
                    FTopLevelAssetPath ClassPath;
                    FString Error;
                    if (!ResolveClassPath(Value->AsString(), ClassPath, Error))
                    {
                        Fail(OutError, Error);
                        return nullptr;
                    }
                    Filter.ClassPaths.Add(ClassPath);
                }
            }
            else
            {
                Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
            }

            TArray<FAssetData> Assets;
            Registry().GetAssets(Filter, Assets);

            FString Query;
            Params->TryGetStringField(TEXT("query"), Query);
            TArray<FString> Words;
            Query.ParseIntoArrayWS(Words);
            // IsCompatibleForEditor reads the asset's Skeleton tag and honours the compatible-skeleton list.
            Assets.RemoveAll([Skeleton, &Words](const FAssetData& Asset)
            {
                return !MatchesQuery(Asset.AssetName.ToString(), Words) || !Skeleton->IsCompatibleForEditor(Asset);
            });
            Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.AssetName.LexicalLess(B.AssetName); });

            const int32 Max = ReadMax(Params, 500);
            TArray<TSharedPtr<FJsonValue>> Anims;
            for (int32 Index = 0; Index < Assets.Num() && Index < Max; ++Index)
            {
                const FAssetData& Asset = Assets[Index];
                TSharedPtr<FJsonObject> Entry = MakeFoundAssetJson(Asset, /*bIncludeSize=*/false);
                FString Frames, Rate;
                const bool bHasFrames = Asset.GetTagValue(TEXT("Number of Frames"), Frames);
                const bool bHasRate = Asset.GetTagValue(TEXT("Source Frame Rate"), Rate);
                if (bHasFrames)
                {
                    Entry->SetNumberField(TEXT("frames"), FCString::Atod(*Frames));
                }
                if (bHasRate)
                {
                    Entry->SetNumberField(TEXT("frame_rate"), FCString::Atod(*Rate));
                }
                if (bHasFrames && bHasRate && FCString::Atod(*Rate) > 0.0)
                {
                    // The tag is the data model's frame count (keys - 1), so frames / rate is the length.
                    Entry->SetNumberField(TEXT("length"), FCString::Atod(*Frames) / FCString::Atod(*Rate));
                }
                Anims.Add(MakeShared<FJsonValueObject>(Entry));
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
            Result->SetArrayField(TEXT("anims"), Anims);
            Result->SetNumberField(TEXT("total"), Assets.Num());
            Result->SetBoolField(TEXT("truncated"), Assets.Num() > Max);
            return Result;
        });
}
