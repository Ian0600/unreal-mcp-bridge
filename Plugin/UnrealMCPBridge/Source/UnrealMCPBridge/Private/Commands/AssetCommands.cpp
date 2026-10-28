#include "Commands/AssetCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "EditorAssetLibrary.h"
#include "EditorUtilityLibrary.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "Containers/Ticker.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
    // 리디렉터 하나와 그 참조자 상태(asset.fixup_redirectors).
    struct FRedirectorFixup
    {
        FName Package;
        FString Destination;
        TArray<FName> Referencers;
    };

    // 참조자 패키지가 왜 안 고쳐졌는지 — 비어 있으면 고쳐졌다.
    FString CheckReferencer(FName Package, FString& OutFilename)
    {
        if (!FPackageName::DoesPackageExist(Package.ToString(), &OutFilename))
        {
            return TEXT("package file not found");
        }
        const FString PackageString = Package.ToString();
        if (FPaths::GetExtension(OutFilename, /*bIncludeDot=*/true) == FPackageName::GetMapPackageExtension()
            || PackageString.Contains(TEXT("/__ExternalActors__/")) || PackageString.Contains(TEXT("/__ExternalObjects__/")))
        {
            /* 맵 로드는 브리지 디스패치 안에서 에디터를 세울 수 있고, 월드 파티션 외부 액터 패키지는 월드 없이 따로 불러
               저장하면 안 된다 — 둘 다 에디터의 Fix Up Redirectors 로. */
            return TEXT("map or level actor — fix it from the editor (Fix Up Redirectors) instead");
        }
        if (IFileManager::Get().IsReadOnly(*OutFilename))
        {
            return TEXT("read-only (check it out in P4 first)");
        }
        return FString();
    }

    // Deferred asset.save results by job number (GameThread only: handlers and core tickers).
    int32& SaveJobCounter()
    {
        static int32 Counter = 0;
        return Counter;
    }

    TMap<int32, TSharedPtr<FJsonObject>>& SaveJobs()
    {
        static TMap<int32, TSharedPtr<FJsonObject>> Jobs;
        return Jobs;
    }

    // Saves each path and reports the file state, so the caller learns "check it out first"
    // instead of a bare false. A read-only file is not attempted: the editor would either fail
    // or raise a checkout dialog that blocks the GameThread.
    TSharedPtr<FJsonObject> SaveAssets(const TArray<FString>& Paths)
    {
        bool bAllSaved = true;
        TArray<TSharedPtr<FJsonValue>> Results;
        for (const FString& Path : Paths)
        {
            const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
            FString File;
            const bool bExists = FPackageName::DoesPackageExist(PackageName, &File);
            const bool bReadOnly = bExists && IFileManager::Get().IsReadOnly(*File);

            auto Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("path"), Path);
            Entry->SetStringField(TEXT("file"), bExists ? FPaths::ConvertRelativePathToFull(File) : FString());
            Entry->SetBoolField(TEXT("read_only"), bReadOnly);
            Entry->SetStringField(TEXT("p4"), !bExists ? TEXT("add") : (bReadOnly ? TEXT("edit") : TEXT("")));

            bool bSaved = false;
            if (bReadOnly)
            {
                Entry->SetStringField(TEXT("error"), TEXT("read-only - p4 edit the file first"));
            }
            else
            {
                bSaved = UEditorAssetLibrary::SaveAsset(Path, /*bOnlyIfIsDirty=*/false);
                if (!bSaved)
                {
                    Entry->SetStringField(TEXT("error"), TEXT("save failed - see the log (LogSavePackage)"));
                }
            }
            Entry->SetBoolField(TEXT("saved"), bSaved);
            bAllSaved &= bSaved;
            Results.Add(MakeShared<FJsonValueObject>(Entry));
        }

        auto Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("saved"), bAllSaved);
        Result->SetArrayField(TEXT("results"), Results);
        return Result;
    }
}

void FAssetCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    // asset.list — returns assets under a given content path
    Registry.Register(TEXT("asset.list"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            if (!Params.IsValid())
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("params object is required");
                return nullptr;
            }

            FString PackagePath;
            if (!Params->TryGetStringField(TEXT("package_path"), PackagePath) || PackagePath.IsEmpty())
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("package_path is required and must be non-empty");
                return nullptr;
            }

            bool bRecursive = false;
            Params->TryGetBoolField(TEXT("recursive"), bRecursive);

            IAssetRegistry& AssetRegistry =
                FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

            TArray<FAssetData> AssetList;
            AssetRegistry.GetAssetsByPath(FName(*PackagePath), AssetList, bRecursive);

            TArray<TSharedPtr<FJsonValue>> AssetArray;
            for (const FAssetData& Asset : AssetList)
            {
                AssetArray.Add(MakeShared<FJsonValueObject>(UnrealMCPBridge::Json::MakeAssetJson(Asset)));
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("assets"), AssetArray);
            return Result;
        });

    // asset.get_metadata — returns class and tag map for an asset by object path
    Registry.Register(TEXT("asset.get_metadata"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            if (!Params.IsValid())
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("params object is required");
                return nullptr;
            }

            FString ObjectPath;
            if (!Params->TryGetStringField(TEXT("object_path"), ObjectPath) || ObjectPath.IsEmpty())
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("object_path is required and must be non-empty");
                return nullptr;
            }

            IAssetRegistry& AssetRegistry =
                FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

            FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
            if (!AssetData.IsValid())
            {
                OutError.Code = MCPProtocol::FMCPError::ServerErrorMin; // -32099: Asset not found
                OutError.Message = FString::Printf(TEXT("Asset not found: %s"), *ObjectPath);
                return nullptr;
            }

            auto TagsObj = MakeShared<FJsonObject>();
            AssetData.TagsAndValues.ForEach([&TagsObj](const TPair<FName, FAssetTagValueRef>& Tag)
            {
                TagsObj->SetStringField(Tag.Key.ToString(), Tag.Value.GetValue());
            });

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("class"), AssetData.AssetClassPath.GetAssetName().ToString());
            Result->SetObjectField(TEXT("tags"),  TagsObj);
            return Result;
        });

    // asset.save — saves asset packages to disk.
    // Params: object_path (string) or paths (array) — one is required; deferred (bool, default
    // false): save on the next editor tick instead of inside this request, and return a job to
    // read with asset.save_result. Deferring keeps the save out of whatever the request runs inside
    // (a tool that just compiled or edited the asset, a modal); scripts used to defer it by hand.
    // Read-only files are not attempted — Perforce has not checked them out.
    // Returns { saved (all ok), results: [{path, saved, read_only, p4 ("edit" = check out first,
    // "add" = new file, "" = fine), file, error}] } or, deferred, { job, queued }.
    Registry.Register(TEXT("asset.save"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            TArray<FString> Paths;
            if (Params.IsValid())
            {
                FString ObjectPath;
                if (Params->TryGetStringField(TEXT("object_path"), ObjectPath) && !ObjectPath.IsEmpty())
                {
                    Paths.Add(ObjectPath);
                }
                const TArray<TSharedPtr<FJsonValue>>* PathValues = nullptr;
                if (Params->TryGetArrayField(TEXT("paths"), PathValues) && PathValues)
                {
                    for (const TSharedPtr<FJsonValue>& Value : *PathValues)
                    {
                        if (!Value->AsString().IsEmpty()) { Paths.Add(Value->AsString()); }
                    }
                }
            }
            if (Paths.Num() == 0)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("object_path or paths is required");
                return nullptr;
            }
            bool bDeferred = false;
            Params->TryGetBoolField(TEXT("deferred"), bDeferred);

            if (!bDeferred)
            {
                return SaveAssets(Paths);
            }

            const int32 Job = ++SaveJobCounter();
            SaveJobs().Add(Job, nullptr);
            FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Job, Paths](float)
            {
                SaveJobs().Add(Job, SaveAssets(Paths));
                return false;   // run once
            }));

            auto Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("job"), Job);
            Result->SetBoolField(TEXT("queued"), true);
            return Result;
        });

    // asset.save_result — the outcome of a deferred asset.save. Params: job (required).
    // Returns { job, done } and, once done, the same fields asset.save returns.
    Registry.Register(TEXT("asset.save_result"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            double JobNumber = 0.0;
            if (!Params.IsValid() || !Params->TryGetNumberField(TEXT("job"), JobNumber))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("job is required");
                return nullptr;
            }
            const int32 Job = static_cast<int32>(JobNumber);
            const TSharedPtr<FJsonObject>* Found = SaveJobs().Find(Job);
            if (!Found)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("No save job %d"), Job);
                return nullptr;
            }
            TSharedPtr<FJsonObject> Result = Found->IsValid() ? MakeShared<FJsonObject>(**Found) : MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("job"), Job);
            Result->SetBoolField(TEXT("done"), Found->IsValid());
            return Result;
        });

    // asset.get_selection — what is currently selected in the Content Browser.
    //
    // Distinct from level.get_selection: an artist picking a SkeletalMesh in the Content
    // Browser selects an *asset*, which never shows up in GEditor->GetSelectedActors().
    // Without this, callers had to shell out to editor.python_exec to answer "what did the
    // user pick?", which is both slower and unavailable when Python scripting is disabled.
    Registry.Register(TEXT("asset.get_selection"),
        [](const TSharedPtr<FJsonObject>& /*Params*/, MCPProtocol::FMCPError& /*OutError*/) -> TSharedPtr<FJsonObject>
        {
            const TArray<FAssetData> Selected = UEditorUtilityLibrary::GetSelectedAssetData();

            TArray<TSharedPtr<FJsonValue>> AssetArray;
            for (const FAssetData& Asset : Selected)
            {
                AssetArray.Add(MakeShared<FJsonValueObject>(UnrealMCPBridge::Json::MakeAssetJson(Asset)));
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("count"), static_cast<double>(Selected.Num()));
            Result->SetArrayField(TEXT("assets"), AssetArray);
            return Result;
        });

    // asset.set_selection — select assets in the Content Browser by object path.
    // Params: object_paths (required, array of strings), focus (optional bool, default true).
    //
    // Paths that do not resolve are reported in "not_found" rather than failing the whole
    // call: selecting four of five assets is more useful than an error naming one of them.
    Registry.Register(TEXT("asset.set_selection"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            if (!Params.IsValid())
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("params object is required");
                return nullptr;
            }

            const TArray<TSharedPtr<FJsonValue>>* PathValues = nullptr;
            if (!Params->TryGetArrayField(TEXT("object_paths"), PathValues) || PathValues == nullptr)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("object_paths is required and must be an array of strings");
                return nullptr;
            }

            if (!GEditor)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("GEditor is not available");
                return nullptr;
            }

            bool bFocus = true;
            Params->TryGetBoolField(TEXT("focus"), bFocus);

            IAssetRegistry& AssetRegistry =
                FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

            TArray<FAssetData> ToSync;
            TArray<TSharedPtr<FJsonValue>> NotFound;
            for (const TSharedPtr<FJsonValue>& Value : *PathValues)
            {
                FString ObjectPath;
                if (!Value.IsValid() || !Value->TryGetString(ObjectPath) || ObjectPath.IsEmpty())
                {
                    continue;
                }

                const FAssetData Asset = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
                if (Asset.IsValid())
                {
                    ToSync.Add(Asset);
                }
                else
                {
                    NotFound.Add(MakeShared<FJsonValueString>(ObjectPath));
                }
            }

            if (ToSync.Num() > 0)
            {
                GEditor->SyncBrowserToObjects(ToSync, bFocus);
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("selected"), static_cast<double>(ToSync.Num()));
            Result->SetArrayField(TEXT("not_found"), NotFound);
            return Result;
        });

    // asset.fixup_redirectors — Fix Up Redirectors without any dialog. Params: package_path (folder,
    // recursive) and/or object_paths (array: redirectors, or any asset — non-redirectors are skipped),
    // dry_run (bool, default false).
    // Returns { redirectors, fixed, referencers_saved: [...], remaining: [ { redirector, destination,
    // referencers: [ { package, reason } ] } ], delete_with_p4: [ files ] }.
    //
    // Why not IAssetTools::FixupReferencers: in 5.8 it opens modal dialogs on several paths ("no
    // referencing assets", revision control errors, the report window when anything is left), and a
    // modal on the GameThread stalls the bridge until "GameThread dispatch timed out". Here each
    // writable referencing package is loaded (loading follows the redirector, so its pointers now aim
    // at the destination) and re-saved directly. A referencer that is read-only (not checked out) or a
    // map is left alone and reported. Nothing is checked out and nothing is deleted: a redirector whose
    // referencers were all re-saved is listed in delete_with_p4 — `p4 delete` it (it is a depot file).
    Registry.Register(TEXT("asset.fixup_redirectors"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            if (!Params.IsValid())
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("params object is required");
                return nullptr;
            }

            IAssetRegistry& AssetRegistry =
                FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

            TArray<FAssetData> Found;
            FString PackagePath;
            const bool bHasFolder = Params->TryGetStringField(TEXT("package_path"), PackagePath) && !PackagePath.IsEmpty();
            if (bHasFolder)
            {
                FARFilter Filter;
                Filter.PackagePaths.Add(FName(*PackagePath));
                Filter.bRecursivePaths = true;
                Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
                AssetRegistry.GetAssets(Filter, Found);
            }
            const TArray<TSharedPtr<FJsonValue>>* PathValues = nullptr;
            TArray<TSharedPtr<FJsonValue>> NotRedirectors;
            if (Params->TryGetArrayField(TEXT("object_paths"), PathValues) && PathValues)
            {
                for (const TSharedPtr<FJsonValue>& Value : *PathValues)
                {
                    FString ObjectPath;
                    if (!Value.IsValid() || !Value->TryGetString(ObjectPath) || ObjectPath.IsEmpty()) { continue; }
                    const FAssetData Asset = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
                    if (Asset.IsValid() && Asset.IsRedirector())
                    {
                        Found.AddUnique(Asset);
                    }
                    else
                    {
                        NotRedirectors.Add(MakeShared<FJsonValueString>(ObjectPath));
                    }
                }
            }
            else if (!bHasFolder)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("package_path or object_paths is required");
                return nullptr;
            }

            bool bDryRun = false;
            Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

            // 리디렉터 패키지 집합 — 리디렉터끼리 가리키는 참조(체인)는 참조자로 치지 않는다.
            TSet<FName> RedirectorPackages;
            for (const FAssetData& Asset : Found) { RedirectorPackages.Add(Asset.PackageName); }

            TArray<FRedirectorFixup> Fixups;
            TSet<FName> AllReferencers;
            for (const FAssetData& Asset : Found)
            {
                FRedirectorFixup& Fixup = Fixups.AddDefaulted_GetRef();
                Fixup.Package = Asset.PackageName;
                // LOAD_NoRedirects — 그냥 불러오면 리디렉터를 따라가 대상 애셋이 돌아온다.
                if (const UObjectRedirector* Redirector = LoadObject<UObjectRedirector>(
                        nullptr, *Asset.GetObjectPathString(), nullptr, LOAD_NoRedirects | LOAD_NoWarn | LOAD_Quiet))
                {
                    if (Redirector->DestinationObject)
                    {
                        Fixup.Destination = Redirector->DestinationObject->GetPathName();
                    }
                }
                if (Fixup.Destination.IsEmpty())
                {
                    // 대상이 지워졌거나 못 불렀다 — 레지스트리 태그에 남은 경로라도 보여 준다.
                    FString Tag;
                    Fixup.Destination = Asset.GetTagValue(TEXT("DestinationObject"), Tag) ? Tag + TEXT(" (not loadable)") : TEXT("(unknown)");
                }
                TArray<FName> Referencers;
                AssetRegistry.GetReferencers(Asset.PackageName, Referencers);
                for (FName Ref : Referencers)
                {
                    if (Ref != Asset.PackageName && !RedirectorPackages.Contains(Ref))
                    {
                        Fixup.Referencers.AddUnique(Ref);
                        AllReferencers.Add(Ref);
                    }
                }
            }

            // 참조자 패키지마다 한 번: 막힌 이유를 보고, 고칠 수 있으면 불러서 다시 저장한다.
            TMap<FName, FString> Blocked;
            TArray<TSharedPtr<FJsonValue>> Saved;
            for (FName Ref : AllReferencers)
            {
                FString Filename;
                FString Reason = CheckReferencer(Ref, Filename);
                if (Reason.IsEmpty() && !bDryRun)
                {
                    UPackage* Package = FindPackage(nullptr, *Ref.ToString());
                    if (!Package)
                    {
                        Package = LoadPackage(nullptr, *Ref.ToString(), LOAD_NoWarn | LOAD_Quiet);
                    }
                    if (!Package)
                    {
                        Reason = TEXT("could not load");
                    }
                    else
                    {
                        FSavePackageArgs SaveArgs;
                        SaveArgs.TopLevelFlags = RF_Standalone;
                        SaveArgs.SaveFlags = SAVE_NoError;
                        SaveArgs.bSlowTask = false;
                        if (UPackage::Save(Package, nullptr, *Filename, SaveArgs).Result == ESavePackageResult::Success)
                        {
                            Saved.Add(MakeShared<FJsonValueString>(Ref.ToString()));
                        }
                        else
                        {
                            Reason = TEXT("save failed (see log)");
                        }
                    }
                }
                if (!Reason.IsEmpty())
                {
                    Blocked.Add(Ref, Reason);
                }
            }

            int32 FixedCount = 0;
            TArray<TSharedPtr<FJsonValue>> Remaining;
            TArray<TSharedPtr<FJsonValue>> DeleteWithP4;
            for (const FRedirectorFixup& Fixup : Fixups)
            {
                TArray<TSharedPtr<FJsonValue>> Left;
                for (FName Ref : Fixup.Referencers)
                {
                    const FString* Reason = Blocked.Find(Ref);
                    if (Reason || bDryRun)
                    {
                        auto Entry = MakeShared<FJsonObject>();
                        Entry->SetStringField(TEXT("package"), Ref.ToString());
                        Entry->SetStringField(TEXT("reason"), Reason ? *Reason : FString(TEXT("dry run — would re-save")));
                        Left.Add(MakeShared<FJsonValueObject>(Entry));
                    }
                }
                if (Left.Num() == 0 && !bDryRun)
                {
                    ++FixedCount;
                    FString RedirectorFile;
                    FPackageName::DoesPackageExist(Fixup.Package.ToString(), &RedirectorFile);
                    DeleteWithP4.Add(MakeShared<FJsonValueString>(
                        RedirectorFile.IsEmpty() ? Fixup.Package.ToString() : FPaths::ConvertRelativePathToFull(RedirectorFile)));
                }
                else
                {
                    auto Entry = MakeShared<FJsonObject>();
                    Entry->SetStringField(TEXT("redirector"), Fixup.Package.ToString());
                    Entry->SetStringField(TEXT("destination"), Fixup.Destination);
                    Entry->SetArrayField(TEXT("referencers"), Left);
                    Remaining.Add(MakeShared<FJsonValueObject>(Entry));
                }
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("dry_run"), bDryRun);
            Result->SetNumberField(TEXT("redirectors"), Fixups.Num());
            Result->SetNumberField(TEXT("fixed"), FixedCount);
            Result->SetArrayField(TEXT("referencers_saved"), Saved);
            Result->SetArrayField(TEXT("remaining"), Remaining);
            Result->SetArrayField(TEXT("delete_with_p4"), DeleteWithP4);
            Result->SetArrayField(TEXT("not_redirectors"), NotRedirectors);
            return Result;
        });
}
