#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MCPInspectorBehaviorTreeToolset.generated.h"

/**
 * Behavior Tree / Blackboard authoring (the official AIModuleToolset is read-only and has no stable node ids).
 * Nodes are created in the BT editor graph (UBehaviorTreeGraph) exactly like the BT editor does, then the runtime
 * tree is rebuilt with UBehaviorTreeGraph::UpdateAsset, so the result opens correctly in the Behavior Tree editor.
 *
 * Node ids are the editor graph node GUIDs (UEdGraphNode::NodeGuid, saved with the asset, stable across reloads).
 * Use DumpBehaviorTree to list ids. The root node id is the GUID of the ROOT graph node (also accepted: "root").
 * Child order follows the BT editor rule: children of a composite are ordered by their X position in the graph.
 * Class arguments accept a native class path ("/Script/AIModule.BTTask_Wait"), a short class name ("BTTask_Wait",
 * "Selector", "Sequence", "SimpleParallel", "Wait") or a Blueprint asset path ("/Game/AI/BTT_MyTask").
 * Changes are NOT saved; save with the official asset tools.
 */
UCLASS(BlueprintType)
class UMCPInspectorBehaviorTreeToolset : public UObject
{
	GENERATED_BODY()

public:

	/**
	 * Create a new Blackboard asset.
	 * @param AssetPath Package path of the new asset, e.g. "/Game/AI/BB_Enemy". Fails if it exists.
	 * @param ParentBlackboardPath Optional parent Blackboard asset path (keys are inherited). Empty = none.
	 * @return Object path of the created Blackboard, or an "ERROR|..." string.
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|BehaviorTree", meta = (AICallable))
	static FString CreateBlackboard(const FString& AssetPath, const FString& ParentBlackboardPath);

	/**
	 * Add a key to a Blackboard asset.
	 * @param BlackboardPath Blackboard asset path.
	 * @param KeyName New key name (must not exist in this Blackboard or its parent chain).
	 * @param KeyType One of: bool, int, float, string, name, vector, rotator, object, class, enum.
	 * @param BaseClassOrEnumPath For object/class keys: base class path (empty = UObject), e.g. "/Script/Engine.Actor" or a Blueprint asset path. For enum keys: enum path, e.g. "/Game/Enums/E_State" or "/Script/AIModule.EBTNodeResult". Ignored for other types.
	 * @param bInstanceSynced If true the key value is shared by all instances of this Blackboard.
	 * @return "OK" or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|BehaviorTree", meta = (AICallable))
	static FString AddBlackboardKey(const FString& BlackboardPath, const FString& KeyName, const FString& KeyType, const FString& BaseClassOrEnumPath, bool bInstanceSynced);

	/**
	 * Remove a key owned by a Blackboard asset (inherited parent keys cannot be removed here).
	 * Nodes that referenced the key keep the name and become invalid until re-bound.
	 * @return "OK" or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|BehaviorTree", meta = (AICallable))
	static FString RemoveBlackboardKey(const FString& BlackboardPath, const FString& KeyName);

	/**
	 * Create a new Behavior Tree asset with its editor graph (ROOT node) and assign a Blackboard.
	 * @param AssetPath Package path of the new asset, e.g. "/Game/AI/BT_Enemy". Fails if it exists.
	 * @param BlackboardPath Blackboard asset path to use. Empty = none.
	 * @return JSON {"path":..., "rootId":...} or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|BehaviorTree", meta = (AICallable))
	static FString CreateBehaviorTree(const FString& AssetPath, const FString& BlackboardPath);

	/**
	 * Add a composite or task node under a parent node.
	 * Composites: Selector, Sequence, SimpleParallel (or any UBTCompositeNode subclass). Tasks: any UBTTaskNode subclass, including Blueprint tasks by asset path.
	 * Rules (same as the BT editor): ROOT accepts exactly one composite; tasks have no children;
	 * SimpleParallel: ChildIndex 0 = main task slot (task only), 1 = background slot (one node).
	 * @param TreePath Behavior Tree asset path.
	 * @param ParentNodeId Parent node id (graph node GUID). Empty or "root" = the ROOT node.
	 * @param NodeClass Node class (path, short name or Blueprint asset path).
	 * @param ChildIndex Insert position among the parent's children (-1 = append).
	 * @return The new node id (GUID string) or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|BehaviorTree", meta = (AICallable))
	static FString AddBTNode(const FString& TreePath, const FString& ParentNodeId, const FString& NodeClass, int32 ChildIndex);

	/**
	 * Attach a decorator (UBTDecorator subclass) or a service (UBTService subclass) to a composite or task node.
	 * Decorators on the composite attached to ROOT become root decorators.
	 * @param TreePath Behavior Tree asset path.
	 * @param NodeId Target composite/task node id.
	 * @param AuxClass Decorator or service class (path, short name e.g. "BTDecorator_Blackboard", or Blueprint asset path).
	 * @return The new aux node id (GUID string) or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|BehaviorTree", meta = (AICallable))
	static FString AddBTAuxNode(const FString& TreePath, const FString& NodeId, const FString& AuxClass);

	/**
	 * Set a property on a node instance (composite, task, decorator or service) via reflection ImportText.
	 * PropertyPath supports nesting and indices ("A.B[2].C"). For a Blackboard key selector property
	 * (FBlackboardKeySelector, e.g. "BlackboardKey") ValueText may be just the key name ("TargetActor");
	 * the key must exist in the tree's Blackboard and match the selector's allowed key types.
	 * For value-or-key properties (FValueOrBBKey_*, e.g. BTTask_Wait "WaitTime", BTTask_MoveTo "AcceptableRadius")
	 * ValueText "2.5" sets the default value and "bb:KeyName" binds a Blackboard key.
	 * On the ROOT node only "BlackboardAsset" is supported (Blackboard asset path).
	 * @param ValueText Value in UE text-import format, e.g. "5.0", "true", "(X=1,Y=2,Z=3)", "/Game/Path.Asset".
	 * @return "OK" or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|BehaviorTree", meta = (AICallable))
	static FString SetBTNodeProperty(const FString& TreePath, const FString& NodeId, const FString& PropertyPath, const FString& ValueText);

	/**
	 * Remove a node. For a composite/task the whole subtree below it (children, their decorators/services) is removed too.
	 * For a decorator/service only that aux node is removed. The ROOT node cannot be removed.
	 * @return "OK" or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|BehaviorTree", meta = (AICallable))
	static FString RemoveBTNode(const FString& TreePath, const FString& NodeId);

	/**
	 * Dump the editor graph of a Behavior Tree as JSON with node ids usable by the tools above.
	 * @return JSON {"tree","blackboard","root":{"id","children":[node]},"disconnected":[node]} where node =
	 *  {"id","kind","class","name","description","executionIndex","keys":{prop:keyName},"decorators":[aux],"services":[aux],"children":[node]}
	 *  or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|BehaviorTree", meta = (AICallable))
	static FString DumpBehaviorTree(const FString& TreePath);
};
