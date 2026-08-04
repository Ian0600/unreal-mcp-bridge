#include "Commands/LevelIOCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "Editor.h"
#include "Selection.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/MeshComponent.h"
#include "Components/BrushComponent.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "UObject/GCObjectScopeGuard.h"

// Export
#include "AssetExportTask.h"
#include "Exporters/Exporter.h"
#include "Exporters/FbxExportOption.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Animation/AnimSequence.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"

// Import
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "Animation/Skeleton.h"

// Spawn
#include "Subsystems/EditorActorSubsystem.h"

namespace
{
    // Upper bound on how many actor names the export response echoes back. A whole-level
    // export can touch thousands of actors and the caller only needs a sample plus the count.
    constexpr int32 LevelIOMaxReportedActors = 200;

    // Helpers carry a LevelIO prefix rather than relying on the anonymous namespace: UBT's
    // unity build merges command files into one translation unit, where a plain ReadVector3
    // would collide with the next handler that wants the same name. Same reasoning as the
    // shared helpers in CommandJsonHelpers.h.

    // Reads an optional [x, y, z] number array. Leaves Out untouched when the field is
    // absent; returns false only when present but malformed.
    bool LevelIOReadVector3(
        const TSharedPtr<FJsonObject>& Params,
        const TCHAR* Field,
        FVector& Out,
        MCPProtocol::FMCPError& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!Params->TryGetArrayField(Field, Arr) || !Arr)
        {
            return true;
        }

        if (Arr->Num() != 3)
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("%s must be a 3-element number array"), Field);
            return false;
        }

        Out.X = (*Arr)[0]->AsNumber();
        Out.Y = (*Arr)[1]->AsNumber();
        Out.Z = (*Arr)[2]->AsNumber();
        return true;
    }

    // Reads an optional [pitch, yaw, roll] array — Unreal's FRotator ordering, as the
    // protocol documents.
    bool LevelIOReadRotator(
        const TSharedPtr<FJsonObject>& Params,
        const TCHAR* Field,
        FRotator& Out,
        MCPProtocol::FMCPError& OutError)
    {
        FVector AsVector(Out.Pitch, Out.Yaw, Out.Roll);
        if (!LevelIOReadVector3(Params, Field, AsVector, OutError))
        {
            return false;
        }
        Out.Pitch = AsVector.X;
        Out.Yaw   = AsVector.Y;
        Out.Roll  = AsVector.Z;
        return true;
    }

    // True when the FBX exporter has geometry to write for this actor. Actors that carry
    // no mesh or brush geometry (pure logic actors, volumes, Blueprint actors whose
    // components are all non-visual) are reported back as skipped so the caller can tell
    // "exported nothing" apart from "exported fewer things than selected".
    bool LevelIOHasExportableGeometry(const AActor* Actor)
    {
        if (!Actor)
        {
            return false;
        }
        if (Actor->FindComponentByClass<UMeshComponent>() != nullptr)
        {
            return true;
        }
        return Actor->FindComponentByClass<UBrushComponent>() != nullptr;
    }

    // Counts LOD0 sections that are driven by cloth simulation.
    //
    // This matters because the engine's FBX exporter drops them on purpose:
    // FFbxExporter::CreateMesh builds its vertex list from
    // FSkeletalMeshLODModel::GetNonClothVertices(), so a mesh whose body is cloth-simulated
    // exports as the leftover non-cloth sections and nothing reports the loss. A caller who
    // only checks "did I get a file" ships half a character. There is no UFbxExportOption
    // switch to change this, so the honest move is to count them and say so.
    void LevelIOCountClothSections(const USkeletalMesh* SkelMesh, int32& OutCloth, int32& OutTotal)
    {
        OutCloth = 0;
        OutTotal = 0;
        if (!SkelMesh)
        {
            return;
        }

        const FSkeletalMeshModel* Model = SkelMesh->GetImportedModel();
        if (!Model || !Model->LODModels.IsValidIndex(0))
        {
            return;
        }

        OutTotal = Model->LODModels[0].Sections.Num();
        for (int32 SectionIndex = 0; SectionIndex < OutTotal; ++SectionIndex)
        {
            if (SkelMesh->IsSectionUsingCloth(SectionIndex))
            {
                ++OutCloth;
            }
        }
    }

    // Ensures the parent directory of a file path exists. Returns false when it cannot be created.
    bool LevelIOEnsureParentDirectory(const FString& FullFilePath)
    {
        const FString ParentDir = FPaths::GetPath(FullFilePath);
        if (ParentDir.IsEmpty())
        {
            return false;
        }
        if (IFileManager::Get().DirectoryExists(*ParentDir))
        {
            return true;
        }
        return IFileManager::Get().MakeDirectory(*ParentDir, /*Tree=*/true);
    }

    // Runs one FBX export to disk. Object is a UWorld for level exports or a mesh asset for
    // asset exports; the exporter is resolved from the .fbx extension either way.
    // Returns false and fills OutMessage on failure.
    bool LevelIORunFbxExport(
        UObject* Object,
        const FString& FullPath,
        bool bSelectedOnly,
        UFbxExportOption* ExportOptions,
        FString& OutMessage)
    {
        if (!LevelIOEnsureParentDirectory(FullPath))
        {
            OutMessage = FString::Printf(TEXT("Could not create parent directory for: %s"), *FullPath);
            return false;
        }

        UAssetExportTask* Task = NewObject<UAssetExportTask>();
        // Keep the task and its options reachable by the GC for the duration of the export.
        FGCObjectScopeGuard TaskGuard(Task);
        FGCObjectScopeGuard OptionsGuard(ExportOptions);

        Task->Object            = Object;
        Task->Exporter          = nullptr;   // resolved from the .fbx extension
        Task->Filename          = FullPath;
        Task->bSelected         = bSelectedOnly;
        Task->bReplaceIdentical = true;
        Task->bPrompt           = false;
        // Without bAutomated the exporter raises a modal options dialog and the GameThread
        // dispatch this handler runs under blocks until it times out.
        Task->bAutomated        = true;
        Task->bUseFileArchive   = false;
        Task->bWriteEmptyFiles  = false;
        Task->Options           = ExportOptions;

        const bool bExported = UExporter::RunAssetExportTask(Task);
        if (!bExported || !IFileManager::Get().FileExists(*FullPath))
        {
            OutMessage = Task->Errors.Num() > 0
                ? FString::Printf(TEXT("FBX export failed: %s"), *FString::Join(Task->Errors, TEXT("; ")))
                : FString::Printf(TEXT("FBX export failed and produced no file: %s"), *FullPath);
            return false;
        }
        return true;
    }
}

void FLevelIOCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    // level.export_selected — writes selected actors (or the whole level) to one FBX file.
    // Params: path (string, required), selected_only (bool, default true),
    //         include_collision (bool, default false), ascii (bool, default false).
    Registry.Register(TEXT("level.export_selected"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString Path;
            if (!UnrealMCPBridge::Json::RequireString(Params, TEXT("path"), Path, OutError))
            {
                return nullptr;
            }

            bool bSelectedOnly = true;
            Params->TryGetBoolField(TEXT("selected_only"), bSelectedOnly);

            bool bIncludeCollision = false;
            Params->TryGetBoolField(TEXT("include_collision"), bIncludeCollision);

            bool bAscii = false;
            Params->TryGetBoolField(TEXT("ascii"), bAscii);

            if (!GEditor)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("GEditor is not available");
                return nullptr;
            }

            UWorld* World = GEditor->GetEditorWorldContext().World();
            if (!World)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("Editor world is not available");
                return nullptr;
            }

            // Collect the actors the exporter will consider, so the response can report what
            // went out without re-parsing the FBX.
            TArray<AActor*> Candidates;
            if (bSelectedOnly)
            {
                USelection* Selection = GEditor->GetSelectedActors();
                if (Selection)
                {
                    Selection->GetSelectedObjects<AActor>(Candidates);
                }

                if (Candidates.Num() == 0)
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = TEXT("selected_only is true but nothing is selected in the level");
                    return nullptr;
                }
            }
            else
            {
                for (TActorIterator<AActor> It(World); It; ++It)
                {
                    Candidates.Add(*It);
                }
            }

            TArray<TSharedPtr<FJsonValue>> ExportedNames;
            TArray<TSharedPtr<FJsonValue>> SkippedNames;
            int32 ExportedCount = 0;
            for (AActor* Actor : Candidates)
            {
                if (!Actor)
                {
                    continue;
                }

                if (LevelIOHasExportableGeometry(Actor))
                {
                    ++ExportedCount;
                    if (ExportedNames.Num() < LevelIOMaxReportedActors)
                    {
                        ExportedNames.Add(MakeShared<FJsonValueString>(Actor->GetActorNameOrLabel()));
                    }
                }
                else if (SkippedNames.Num() < LevelIOMaxReportedActors)
                {
                    SkippedNames.Add(MakeShared<FJsonValueString>(Actor->GetActorNameOrLabel()));
                }
            }

            const FString FullPath = FPaths::ConvertRelativePathToFull(Path);

            UFbxExportOption* ExportOptions = NewObject<UFbxExportOption>();
            ExportOptions->bASCII    = bAscii ? 1 : 0;
            ExportOptions->Collision = bIncludeCollision ? 1 : 0;

            FString ExportMessage;
            if (!LevelIORunFbxExport(World, FullPath, bSelectedOnly, ExportOptions, ExportMessage))
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = ExportMessage;
                return nullptr;
            }

            const int64 SizeBytes = IFileManager::Get().FileSize(*FullPath);

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("path"),        FullPath);
            Result->SetBoolField(TEXT("exists"),        true);
            Result->SetNumberField(TEXT("size_bytes"),  static_cast<double>(SizeBytes));
            Result->SetNumberField(TEXT("actor_count"), static_cast<double>(ExportedCount));
            Result->SetArrayField(TEXT("actors"),       ExportedNames);
            Result->SetArrayField(TEXT("skipped"),      SkippedNames);
            Result->SetBoolField(TEXT("actors_truncated"), ExportedCount > ExportedNames.Num());
            return Result;
        });

    // asset.export_fbx — writes individual mesh assets to FBX, one file each, without going
    // through the level. This is the per-asset counterpart to level.export_selected: picking a
    // Blueprint character in the viewport drags every one of its skeletal mesh components into a
    // single file, whereas here SK_Foo_Body comes out on its own.
    // Params: path (string, required) — a *.fbx file when exactly one asset resolves, otherwise
    //           a directory that receives one <AssetName>.fbx per asset.
    //         object_path (string) or object_paths (string array) — what to export. When both are
    //           omitted the current Content Browser selection is used.
    //         ascii (bool, default false), include_collision (bool, default false),
    //         include_lods (bool, default false), include_morph_targets (bool, default true).
    Registry.Register(TEXT("asset.export_fbx"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString Path;
            if (!UnrealMCPBridge::Json::RequireString(Params, TEXT("path"), Path, OutError))
            {
                return nullptr;
            }

            bool bAscii = false;
            Params->TryGetBoolField(TEXT("ascii"), bAscii);

            bool bIncludeCollision = false;
            Params->TryGetBoolField(TEXT("include_collision"), bIncludeCollision);

            bool bIncludeLods = false;
            Params->TryGetBoolField(TEXT("include_lods"), bIncludeLods);

            bool bIncludeMorphTargets = true;
            Params->TryGetBoolField(TEXT("include_morph_targets"), bIncludeMorphTargets);

            // Animation exports carry the skeleton but not the geometry unless this is on.
            // Default on: a bare skeleton is rarely what a DCC round trip wants, and the
            // extra mesh is what lets an artist see the motion on the character in Maya.
            bool bExportPreviewMesh = true;
            Params->TryGetBoolField(TEXT("export_preview_mesh"), bExportPreviewMesh);

            // Resolve what to export: explicit paths win over the Content Browser selection.
            TArray<FString> RequestedPaths;
            FString SingleObjectPath;
            if (Params->TryGetStringField(TEXT("object_path"), SingleObjectPath) && !SingleObjectPath.IsEmpty())
            {
                RequestedPaths.Add(SingleObjectPath);
            }

            const TArray<TSharedPtr<FJsonValue>>* PathArray = nullptr;
            if (Params->TryGetArrayField(TEXT("object_paths"), PathArray) && PathArray)
            {
                for (const TSharedPtr<FJsonValue>& Value : *PathArray)
                {
                    const FString Entry = Value->AsString();
                    if (!Entry.IsEmpty())
                    {
                        RequestedPaths.AddUnique(Entry);
                    }
                }
            }

            const bool bFromContentBrowser = RequestedPaths.Num() == 0;
            if (bFromContentBrowser)
            {
                FContentBrowserModule& ContentBrowser =
                    FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

                TArray<FAssetData> SelectedAssets;
                ContentBrowser.Get().GetSelectedAssets(SelectedAssets);
                if (SelectedAssets.Num() == 0)
                {
                    // GetSelectedAssets only reads the *primary* browser, which is empty when the
                    // user is working out of the content drawer or a second docked browser.
                    ContentBrowser.Get().GetAllSelectedAssets(SelectedAssets);
                }

                for (const FAssetData& AssetData : SelectedAssets)
                {
                    RequestedPaths.AddUnique(AssetData.GetObjectPathString());
                }

                if (RequestedPaths.Num() == 0)
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = TEXT("No object_path/object_paths given and nothing is selected in the Content Browser");
                    return nullptr;
                }
            }

            // A path ending in .fbx names one output file, which only makes sense for a single
            // asset. Anything else is a directory that collects one file per asset.
            const bool bPathIsFile = Path.EndsWith(TEXT(".fbx"), ESearchCase::IgnoreCase);
            if (bPathIsFile && RequestedPaths.Num() > 1)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(
                    TEXT("path names a single .fbx file but %d assets were requested — pass a directory instead"),
                    RequestedPaths.Num());
                return nullptr;
            }

            const FString FullPath = FPaths::ConvertRelativePathToFull(Path);

            TArray<TSharedPtr<FJsonValue>> Exported;
            TArray<TSharedPtr<FJsonValue>> Skipped;

            auto AddSkipped = [&Skipped](const FString& ObjectPath, const FString& Reason)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("object_path"), ObjectPath);
                Entry->SetStringField(TEXT("reason"),      Reason);
                Skipped.Add(MakeShared<FJsonValueObject>(Entry));
            };

            for (const FString& ObjectPath : RequestedPaths)
            {
                UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath);
                if (!Asset)
                {
                    AddSkipped(ObjectPath, TEXT("could not be loaded"));
                    continue;
                }

                // Restricting to the types with a registered FBX exporter keeps the failure mode
                // legible: everything else would either find no exporter or emit something the
                // caller did not ask for.
                if (!Asset->IsA<UStaticMesh>() && !Asset->IsA<USkeletalMesh>() && !Asset->IsA<UAnimSequence>())
                {
                    AddSkipped(ObjectPath, FString::Printf(
                        TEXT("%s is not a StaticMesh, SkeletalMesh or AnimSequence"),
                        *Asset->GetClass()->GetName()));
                    continue;
                }

                // UAnimSequenceExporterFBX needs both a skeleton and a preview mesh; without
                // them it logs a warning and returns false, which would surface here as a bare
                // "export failed" with no way to tell why. Check up front and say which is
                // missing — the fix (assign a preview mesh on the skeleton) is not obvious.
                if (const UAnimSequence* AnimSeq = Cast<UAnimSequence>(Asset))
                {
                    USkeleton* AnimSkeleton = AnimSeq->GetSkeleton();
                    if (!AnimSkeleton)
                    {
                        AddSkipped(ObjectPath, TEXT("AnimSequence has no skeleton assigned"));
                        continue;
                    }

                    USkeletalMesh* PreviewMesh =
                        AnimSkeleton->GetAssetPreviewMesh(const_cast<UAnimSequence*>(AnimSeq));
                    if (!PreviewMesh)
                    {
                        PreviewMesh = AnimSkeleton->FindCompatibleMesh();
                    }
                    if (!PreviewMesh)
                    {
                        AddSkipped(ObjectPath, FString::Printf(
                            TEXT("no preview mesh available for skeleton %s — assign one on the ")
                            TEXT("skeleton or the animation before exporting"),
                            *AnimSkeleton->GetName()));
                        continue;
                    }
                }

                const FString OutFile = bPathIsFile
                    ? FullPath
                    : FPaths::Combine(FullPath, Asset->GetName() + TEXT(".fbx"));

                UFbxExportOption* ExportOptions = NewObject<UFbxExportOption>();
                ExportOptions->bASCII              = bAscii ? 1 : 0;
                ExportOptions->Collision           = bIncludeCollision ? 1 : 0;
                ExportOptions->LevelOfDetail       = bIncludeLods ? 1 : 0;
                ExportOptions->bExportMorphTargets = bIncludeMorphTargets ? 1 : 0;
                ExportOptions->bExportPreviewMesh  = bExportPreviewMesh ? 1 : 0;

                FString ExportMessage;
                if (!LevelIORunFbxExport(Asset, OutFile, /*bSelectedOnly=*/false, ExportOptions, ExportMessage))
                {
                    AddSkipped(ObjectPath, ExportMessage);
                    continue;
                }

                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("object_path"), ObjectPath);
                Entry->SetStringField(TEXT("path"),        OutFile);
                Entry->SetStringField(TEXT("class"),       Asset->GetClass()->GetName());
                Entry->SetNumberField(TEXT("size_bytes"),
                    static_cast<double>(IFileManager::Get().FileSize(*OutFile)));

                // Report geometry the engine dropped on the floor. Silence here is what let a
                // half-exported character look like a success.
                if (const USkeletalMesh* SkelMesh = Cast<USkeletalMesh>(Asset))
                {
                    int32 ClothSections = 0;
                    int32 TotalSections = 0;
                    LevelIOCountClothSections(SkelMesh, ClothSections, TotalSections);

                    Entry->SetNumberField(TEXT("sections"),        static_cast<double>(TotalSections));
                    Entry->SetNumberField(TEXT("cloth_sections_excluded"), static_cast<double>(ClothSections));
                    if (ClothSections > 0)
                    {
                        Entry->SetStringField(TEXT("warning"), FString::Printf(
                            TEXT("%d of %d LOD0 sections use cloth simulation and were EXCLUDED by ")
                            TEXT("Unreal's FBX exporter — the exported mesh is incomplete. This is an ")
                            TEXT("engine limitation with no export option to override it."),
                            ClothSections, TotalSections));
                    }
                }
                else if (Asset->IsA<UAnimSequence>())
                {
                    Entry->SetBoolField(TEXT("preview_mesh_included"), bExportPreviewMesh);
                    Entry->SetStringField(TEXT("warning"),
                        TEXT("FBX carries skeleton and curves only: anim notifies, sync markers and ")
                        TEXT("custom curves are Unreal-side data and are lost on round trip. ")
                        TEXT("Additive animations export as raw poses, not deltas."));
                }

                Exported.Add(MakeShared<FJsonValueObject>(Entry));
            }

            if (Exported.Num() == 0)
            {
                // Spell the reasons out in the message as well as in data — a client that only
                // surfaces error.message should still learn why nothing came out.
                TArray<FString> Reasons;
                for (const TSharedPtr<FJsonValue>& Value : Skipped)
                {
                    const TSharedPtr<FJsonObject>& Entry = Value->AsObject();
                    Reasons.Add(FString::Printf(TEXT("%s (%s)"),
                        *Entry->GetStringField(TEXT("object_path")),
                        *Entry->GetStringField(TEXT("reason"))));
                }

                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = FString::Printf(
                    TEXT("Nothing was exported out of %d requested asset(s): %s"),
                    RequestedPaths.Num(), *FString::Join(Reasons, TEXT("; ")));
                OutError.Data = MakeShared<FJsonValueArray>(Skipped);
                return nullptr;
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("exported"),  Exported);
            Result->SetNumberField(TEXT("count"),    static_cast<double>(Exported.Num()));
            Result->SetArrayField(TEXT("skipped"),   Skipped);
            Result->SetBoolField(TEXT("from_content_browser_selection"), bFromContentBrowser);
            return Result;
        });

    // asset.import_fbx — brings an FBX on disk into the content browser.
    // Params: path (string, required), destination_path (string, required),
    //         replace_existing (bool, default true), import_as_skeletal (bool, default false),
    //         skeleton_path (string, optional), combine_meshes (bool, default false),
    //         generate_lightmap_uvs (bool, default true), auto_generate_collision (bool, default false),
    //         save (bool, default true), use_interchange (bool, default false).
    Registry.Register(TEXT("asset.import_fbx"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString Path;
            if (!UnrealMCPBridge::Json::RequireString(Params, TEXT("path"), Path, OutError))
            {
                return nullptr;
            }

            FString DestinationPath;
            if (!UnrealMCPBridge::Json::RequireString(Params, TEXT("destination_path"), DestinationPath, OutError))
            {
                return nullptr;
            }

            const FString FullPath = FPaths::ConvertRelativePathToFull(Path);
            if (!IFileManager::Get().FileExists(*FullPath))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("FBX file not found: %s"), *FullPath);
                return nullptr;
            }

            if (!DestinationPath.StartsWith(TEXT("/")))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(
                    TEXT("destination_path must be a content path such as /Game/Props, got: %s"), *DestinationPath);
                return nullptr;
            }

            bool bReplaceExisting = true;
            Params->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);

            bool bImportAsSkeletal = false;
            Params->TryGetBoolField(TEXT("import_as_skeletal"), bImportAsSkeletal);

            bool bCombineMeshes = false;
            Params->TryGetBoolField(TEXT("combine_meshes"), bCombineMeshes);

            bool bGenerateLightmapUVs = true;
            Params->TryGetBoolField(TEXT("generate_lightmap_uvs"), bGenerateLightmapUVs);

            bool bAutoGenerateCollision = false;
            Params->TryGetBoolField(TEXT("auto_generate_collision"), bAutoGenerateCollision);

            bool bSave = true;
            Params->TryGetBoolField(TEXT("save"), bSave);

            // The engine routes FBX through Interchange by default, which ignores the legacy
            // UFbxImportUI flags above. Naming the legacy UFbxFactory explicitly keeps the
            // documented options meaningful; use_interchange gives the default routing back.
            bool bUseInterchange = false;
            Params->TryGetBoolField(TEXT("use_interchange"), bUseInterchange);

            USkeleton* Skeleton = nullptr;
            FString SkeletonPath;
            if (Params->TryGetStringField(TEXT("skeleton_path"), SkeletonPath) && !SkeletonPath.IsEmpty())
            {
                Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
                if (!Skeleton)
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = FString::Printf(TEXT("Could not load skeleton: %s"), *SkeletonPath);
                    return nullptr;
                }
            }

            UAssetImportTask* Task = NewObject<UAssetImportTask>();
            FGCObjectScopeGuard TaskGuard(Task);

            Task->Filename         = FullPath;
            Task->DestinationPath  = DestinationPath;
            Task->bReplaceExisting = bReplaceExisting;
            Task->bSave            = bSave;
            Task->bAsync           = false;
            // Same reason as the export side: suppress the modal import options dialog.
            Task->bAutomated       = true;

            UFbxImportUI* Options = NewObject<UFbxImportUI>(Task);
            Options->bImportMesh       = true;
            Options->bImportAsSkeletal = bImportAsSkeletal;
            Options->SetMeshTypeToImport();
            Options->bAutomatedImportShouldDetectType = false;
            // Materials and textures do not round trip — an Unreal material and a Maya shader
            // are different objects. Geometry, names and slot layout are what cross over.
            Options->bImportMaterials  = 0;
            Options->bImportTextures   = 0;
            Options->bImportAnimations = 0;
            Options->Skeleton          = Skeleton;

            if (Options->StaticMeshImportData)
            {
                Options->StaticMeshImportData->bCombineMeshes         = bCombineMeshes ? 1 : 0;
                Options->StaticMeshImportData->bGenerateLightmapUVs   = bGenerateLightmapUVs ? 1 : 0;
                Options->StaticMeshImportData->bAutoGenerateCollision = bAutoGenerateCollision ? 1 : 0;
            }
            if (Options->SkeletalMeshImportData)
            {
                Options->SkeletalMeshImportData->bImportMeshesInBoneHierarchy = true;
            }

            Task->Options = Options;

            if (!bUseInterchange)
            {
                UFbxFactory* Factory = NewObject<UFbxFactory>(Task);
                // We already decided static vs skeletal from the params; do not let the factory
                // re-sniff the file and override MeshTypeToImport.
                Factory->SetDetectImportTypeOnImport(false);
                Factory->ImportUI = Options;
                Task->Factory = Factory;
            }

            IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
            AssetTools.ImportAssetTasks({ Task });

            TArray<TSharedPtr<FJsonValue>> ImportedPaths;
            for (UObject* Imported : Task->GetObjects())
            {
                if (Imported)
                {
                    ImportedPaths.Add(MakeShared<FJsonValueString>(Imported->GetPathName()));
                }
            }

            if (ImportedPaths.Num() == 0)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = FString::Printf(TEXT("FBX import produced no assets from: %s"), *FullPath);
                return nullptr;
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("imported"), ImportedPaths);
            Result->SetNumberField(TEXT("count"),   static_cast<double>(ImportedPaths.Num()));
            Result->SetBoolField(TEXT("saved"),     bSave);
            return Result;
        });

    // level.spawn_asset — places an imported asset into the current editor level.
    // Params: object_path (string, required), location/rotation/scale ([x,y,z], optional),
    //         label (string, optional). rotation is [pitch, yaw, roll].
    Registry.Register(TEXT("level.spawn_asset"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString ObjectPath;
            if (!UnrealMCPBridge::Json::RequireString(Params, TEXT("object_path"), ObjectPath, OutError))
            {
                return nullptr;
            }

            FVector  Location = FVector::ZeroVector;
            FRotator Rotation = FRotator::ZeroRotator;
            FVector  Scale    = FVector::OneVector;

            if (!LevelIOReadVector3(Params, TEXT("location"), Location, OutError)) { return nullptr; }
            if (!LevelIOReadRotator(Params, TEXT("rotation"), Rotation, OutError)) { return nullptr; }
            if (!LevelIOReadVector3(Params, TEXT("scale"),    Scale,    OutError)) { return nullptr; }

            if (!GEditor)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("GEditor is not available");
                return nullptr;
            }

            UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath);
            if (!Asset)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("Could not load asset: %s"), *ObjectPath);
                return nullptr;
            }

            UEditorActorSubsystem* ActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
            if (!ActorSubsystem)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("UEditorActorSubsystem is not available");
                return nullptr;
            }

            AActor* Spawned = ActorSubsystem->SpawnActorFromObject(Asset, Location, Rotation);
            if (!Spawned)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = FString::Printf(
                    TEXT("SpawnActorFromObject returned null — %s may not be a spawnable asset type"), *ObjectPath);
                return nullptr;
            }

            Spawned->SetActorScale3D(Scale);

            FString Label;
            if (Params->TryGetStringField(TEXT("label"), Label) && !Label.IsEmpty())
            {
                Spawned->SetActorLabel(Label);
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("actor_name"), Spawned->GetActorNameOrLabel());
            Result->SetStringField(TEXT("class"),      Spawned->GetClass()->GetName());
            Result->SetStringField(TEXT("actor_path"), Spawned->GetPathName());
            return Result;
        });

    // level.get_selection — which actors are selected in the level viewport.
    //
    // Pairs with level.export_selected: check what will be exported before writing a file.
    // Note this is a different selection from asset.get_selection — an artist picking a mesh
    // in the Content Browser selects an asset and leaves the level selection empty.
    Registry.Register(TEXT("level.get_selection"),
        [](const TSharedPtr<FJsonObject>& /*Params*/, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            if (!GEditor)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("GEditor is not available");
                return nullptr;
            }

            TArray<AActor*> Actors;
            if (USelection* Selection = GEditor->GetSelectedActors())
            {
                Selection->GetSelectedObjects<AActor>(Actors);
            }

            TArray<TSharedPtr<FJsonValue>> ActorArray;
            for (AActor* Actor : Actors)
            {
                if (Actor)
                {
                    ActorArray.Add(MakeShared<FJsonValueObject>(UnrealMCPBridge::Json::MakeActorJson(Actor)));
                }
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("count"), static_cast<double>(ActorArray.Num()));
            Result->SetArrayField(TEXT("actors"), ActorArray);
            return Result;
        });
}
