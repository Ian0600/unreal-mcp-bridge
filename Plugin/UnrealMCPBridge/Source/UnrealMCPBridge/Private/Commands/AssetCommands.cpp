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
#include "Commands/InspectorInvoke.h"

void FAssetCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    // ---- BEGIN commands forwarded to BlueprintInspector (generated from its function headers) ----
    Registry.Register(TEXT("asset.fixup_redirectors"), MakeInspectorCommand(TEXT("/Script/UnrealMCPBridge.MCPInspectorEditorToolset"), TEXT("FixupRedirectors"), { TEXT("content_paths") }));
    // ---- END forwarded commands ----

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

    // asset.save — saves an asset package to disk
    Registry.Register(TEXT("asset.save"),
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

            // SaveAsset expects a package path (without the asset name after the last dot)
            const bool bSaved = UEditorAssetLibrary::SaveAsset(ObjectPath, /*bOnlyIfIsDirty=*/false);

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("saved"), bSaved);
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
}
