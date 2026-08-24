using UnrealBuildTool;
using System;

public class UnrealMCPBridge : ModuleRules
{
    public UnrealMCPBridge(ReadOnlyTargetRules Target) : base(Target)
    {
        // Editor-only module: refuse to build outside editor targets.
        if (!Target.bBuildEditor)
        {
            throw new Exception("UnrealMCPBridge is an Editor-only module and cannot be built for non-editor targets.");
        }

        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "DeveloperSettings",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Slate",
            "SlateCore",
            "Json",
            "JsonUtilities",
            "Sockets",
            "Networking",
            "UnrealEd",
            "EditorSubsystem",
            "EditorScriptingUtilities",
            // UEditorUtilityLibrary::GetSelectedAssetData for asset.get_selection
            "Blutility",
            "AssetRegistry",
            "AssetTools",
            // UEditorUtilityLibrary::GetSelectedAssetData for asset.get_selection
            "Blutility",
            // Content Browser asset selection, so asset.export_fbx can act on what the user
            // has highlighted rather than requiring explicit object paths
            "ContentBrowser",
            "Kismet",
            "KismetCompiler",
            "BlueprintGraph",
            // Material authoring (UMaterialEditingLibrary lives in the MaterialEditor module)
            "MaterialEditor",
            // Live node geometry (SGraphPanel::GetBoundsForNode) for graph layout queries
            "GraphEditor",
            "LiveCoding",
            "LevelEditor",
            "PythonScriptPlugin",
            "AnimGraph",
            "AnimGraphRuntime",
            // StateTree authoring (states/evaluators/tasks/transitions/bindings/compile)
            "StateTreeModule",
            "StateTreeEditorModule",
            "PropertyBindingUtils",
            // Gameplay Camera System rig editing (node inspection + property tuning)
            "GameplayCameras",
            // Niagara module stack authoring. NiagaraEditor exports its stack view models in
            // full, which is why the commands go through those rather than the mostly
            // unexported FNiagaraStackGraphUtilities free functions.
            "Niagara",
            "NiagaraEditor",
            // Widget Blueprint (UMG) authoring. UMGEditor exports FWidgetBlueprintOperationUtils,
            // which UE 5.6 added for exactly this: headless widget tree manipulation that takes a
            // UWidgetBlueprint* instead of a live editor toolkit.
            "UMG",
            "UMGEditor",
            // Widget animations are UMovieSceneSequences; tracks, sections and keyframe channels
            // all come from these two.
            "MovieScene",
            "MovieSceneTracks",
        });
    }
}
