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
            "AssetRegistry",
            "AssetTools",
            "Kismet",
            "KismetCompiler",
            "BlueprintGraph",
            // Material authoring (UMaterialEditingLibrary lives in the MaterialEditor module)
            "MaterialEditor",
            "LiveCoding",
            "LevelEditor",
            "PythonScriptPlugin",
            "AnimGraph",
            "AnimGraphRuntime",
            // StateTree authoring (states/evaluators/tasks/transitions/bindings/compile)
            // UE 5.3: bindings use FStateTreePropertyPath (StateTreeModule); no PropertyBindingUtils module.
            "StateTreeModule",
            "StateTreeEditorModule",
            // UE 5.3: FInstancedStruct::InitializeAs lives in the StructUtils module (merged into
            // CoreUObject only in 5.5+). Needed to link StateTree node creation in AddNodeToArray.
            "StructUtils",
            // BehaviorTree/Blackboard authoring (behaviortree.* commands): runtime nodes + blackboard
            // (AIModule), the editor graph (BehaviorTreeEditor), and its AIGraph base.
            "AIModule",
            "AIGraph",
            "BehaviorTreeEditor",
        });
    }
}
