#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MCPInspectorStateTreeToolset.generated.h"

/**
 * StateTree authoring through UStateTreeEditorData (the official StateTreeToolset is read-only and returns object handles, not ids).
 * Ids: state ids = UStateTreeState::ID, node ids = FStateTreeEditorNode::ID, transition ids = FStateTreeTransition::ID.
 * All are FGuid strings saved with the asset (stable across reloads). Use DumpStateTree to list them.
 * Node type arguments accept a native struct path ("/Script/StateTreeModule.StateTreeDelayTask"), a short struct name
 * ("StateTreeDelayTask" / "FStateTreeDelayTask"), or a Blueprint task/condition asset path ("/Game/AI/STT_MyTask").
 * After every edit the tree is validated and marked modified like the StateTree editor does; call CompileStateTree to
 * compile and get the compiler messages. Changes are NOT saved; save with the official asset tools.
 */
UCLASS(BlueprintType)
class UMCPInspectorStateTreeToolset : public UObject
{
	GENERATED_BODY()

public:












	// ---- property bindings (inputs such as Compare conditions' Left/Right must be bound to compile) ----

	/**
	 * List the structs a node (task/condition/evaluator) can bind its properties to: schema context data (e.g. Actor),
	 * root parameters, state parameters and outputs of other nodes visible from the target, with their top-level properties.
	 * @param TargetNodeId Id of the node whose property will be bound (from DumpStateTree).
	 * @return JSON [{"id":structId,"name","struct","properties":[{"name","type"}]}] or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|StateTree", meta = (AICallable))
	static FString ListStateTreeBindingSources(const FString& TreePath, const FString& TargetNodeId);


	/**
	 * Remove the binding(s) of a node property.
	 * @return "OK" or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|StateTree", meta = (AICallable))
	static FString UnbindStateTreeProperty(const FString& TreePath, const FString& TargetNodeId, const FString& TargetPropertyPath);

	/**
	 * Add a root parameter to the StateTree (bindable by every node; set per instance by the StateTree component).
	 * @param Type bool, int, int64, float, double, name, string, text, enum:<EnumPath>, struct:<StructPath>, object:<ClassPath>, class:<ClassPath>.
	 * @return The root parameters struct id (use as SourceStructId) or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|StateTree", meta = (AICallable))
	static FString AddStateTreeParameter(const FString& TreePath, const FString& Name, const FString& Type);
};
