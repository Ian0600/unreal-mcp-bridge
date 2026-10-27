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
            // 피직스 애셋 바디 저작. UBodySetupCore(BoneName/PhysicsType)와 EPhysicsType 이
            // Engine 이 아니라 PhysicsCore 에 산다.
            "PhysicsCore",
            // 시퀀서(sequencer.*). 베이크는 MovieSceneTools(ExportToAnimSequence), 링크는 SequencerScripting
            // 플러그인의 에디터 모듈(LinkAnimSequence)에 있다.
            "LevelSequence",
            "MovieSceneTools",
            "SequencerScriptingEditor",
            // nav.project_point / nav.test_path (UNavigationSystemV1, FPathFindingQuery).
            "NavigationSystem",
            // scene.capture_view writes png/jpg through FImageUtils + FImageView.
            "ImageCore",
            // sequence.capture_frame reads the cine camera's horizontal field of view.
            "CinematicCamera",
        });
    }
}
