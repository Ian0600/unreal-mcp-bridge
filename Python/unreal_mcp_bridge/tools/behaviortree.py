from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_behaviortree_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def behaviortree_create_blackboard(asset_path: str, parent_blackboard_path: str) -> dict:
        """Behavior Tree / Blackboard authoring (the official AIModuleToolset is read-only and has no stable node ids).
        Nodes are created in the BT editor graph (UBehaviorTreeGraph) exactly like the BT editor does, then the runtime
        tree is rebuilt with UBehaviorTreeGraph::UpdateAsset, so the result opens correctly in the Behavior Tree editor.
        
        Node ids are the editor graph node GUIDs (UEdGraphNode::NodeGuid, saved with the asset, stable across reloads).
        Use DumpBehaviorTree to list ids. The root node id is the GUID of the ROOT graph node (also accepted: "root").
        Child order follows the BT editor rule: children of a composite are ordered by their X position in the graph.
        Class arguments accept a native class path ("/Script/AIModule.BTTask_Wait"), a short class name ("BTTask_Wait",
        "Selector", "Sequence", "SimpleParallel", "Wait") or a Blueprint asset path ("/Game/AI/BTT_MyTask").
        Changes are NOT saved; save with the official asset tools.
        /
        UCLASS(BlueprintType)
        class BLUEPRINTINSPECTOR_API UBPIBehaviorTreeToolset : public UToolsetDefinition
        {
        	GENERATED_BODY()
        
        public:
        	virtual FString GetToolsetVersion() const override { return TEXT("1.0"); }
        
        	/**
        Create a new Blackboard asset.
        asset_path: Package path of the new asset, e.g. "/Game/AI/BB_Enemy". Fails if it exists.
        parent_blackboard_path: Optional parent Blackboard asset path (keys are inherited). Empty = none.
        Returns: Object path of the created Blackboard, or an "ERROR|..." string.

        Built-in port of BlueprintInspector's BPIBehaviorTreeToolset::CreateBlackboard; no other plugin needed."""
        return conn.call("behaviortree.create_blackboard", {"asset_path": asset_path, "parent_blackboard_path": parent_blackboard_path})

    @mcp.tool()
    def behaviortree_add_blackboard_key(blackboard_path: str, key_name: str, key_type: str, base_class_or_enum_path: str, instance_synced: bool) -> dict:
        """Add a key to a Blackboard asset.
        blackboard_path: Blackboard asset path.
        key_name: New key name (must not exist in this Blackboard or its parent chain).
        key_type: One of: bool, int, float, string, name, vector, rotator, object, class, enum.
        base_class_or_enum_path: For object/class keys: base class path (empty = UObject), e.g. "/Script/Engine.Actor" or a Blueprint asset path. For enum keys: enum path, e.g. "/Game/Enums/E_State" or "/Script/AIModule.EBTNodeResult". Ignored for other types.
        instance_synced: If true the key value is shared by all instances of this Blackboard.
        Returns: "OK" or "ERROR|...".

        Built-in port of BlueprintInspector's BPIBehaviorTreeToolset::AddBlackboardKey; no other plugin needed."""
        return conn.call("behaviortree.add_blackboard_key", {"blackboard_path": blackboard_path, "key_name": key_name, "key_type": key_type, "base_class_or_enum_path": base_class_or_enum_path, "instance_synced": instance_synced})

    @mcp.tool()
    def behaviortree_remove_blackboard_key(blackboard_path: str, key_name: str) -> dict:
        """Remove a key owned by a Blackboard asset (inherited parent keys cannot be removed here).
        Nodes that referenced the key keep the name and become invalid until re-bound.
        Returns: "OK" or "ERROR|...".

        Built-in port of BlueprintInspector's BPIBehaviorTreeToolset::RemoveBlackboardKey; no other plugin needed."""
        return conn.call("behaviortree.remove_blackboard_key", {"blackboard_path": blackboard_path, "key_name": key_name})

    @mcp.tool()
    def behaviortree_create(asset_path: str, blackboard_path: str) -> dict:
        """Create a new Behavior Tree asset with its editor graph (ROOT node) and assign a Blackboard.
        asset_path: Package path of the new asset, e.g. "/Game/AI/BT_Enemy". Fails if it exists.
        blackboard_path: Blackboard asset path to use. Empty = none.
        Returns: JSON {"path":..., "rootId":...} or "ERROR|...".

        Built-in port of BlueprintInspector's BPIBehaviorTreeToolset::CreateBehaviorTree; no other plugin needed."""
        return conn.call("behaviortree.create", {"asset_path": asset_path, "blackboard_path": blackboard_path})

    @mcp.tool()
    def behaviortree_add_node(tree_path: str, parent_node_id: str, node_class: str, child_index: int) -> dict:
        """Add a composite or task node under a parent node.
        Composites: Selector, Sequence, SimpleParallel (or any UBTCompositeNode subclass). Tasks: any UBTTaskNode subclass, including Blueprint tasks by asset path.
        Rules (same as the BT editor): ROOT accepts exactly one composite; tasks have no children;
        SimpleParallel: ChildIndex 0 = main task slot (task only), 1 = background slot (one node).
        tree_path: Behavior Tree asset path.
        parent_node_id: Parent node id (graph node GUID). Empty or "root" = the ROOT node.
        node_class: Node class (path, short name or Blueprint asset path).
        child_index: Insert position among the parent's children (-1 = append).
        Returns: The new node id (GUID string) or "ERROR|...".

        Built-in port of BlueprintInspector's BPIBehaviorTreeToolset::AddBTNode; no other plugin needed."""
        return conn.call("behaviortree.add_node", {"tree_path": tree_path, "parent_node_id": parent_node_id, "node_class": node_class, "child_index": child_index})

    @mcp.tool()
    def behaviortree_add_aux_node(tree_path: str, node_id: str, aux_class: str) -> dict:
        """Attach a decorator (UBTDecorator subclass) or a service (UBTService subclass) to a composite or task node.
        Decorators on the composite attached to ROOT become root decorators.
        tree_path: Behavior Tree asset path.
        node_id: Target composite/task node id.
        aux_class: Decorator or service class (path, short name e.g. "BTDecorator_Blackboard", or Blueprint asset path).
        Returns: The new aux node id (GUID string) or "ERROR|...".

        Built-in port of BlueprintInspector's BPIBehaviorTreeToolset::AddBTAuxNode; no other plugin needed."""
        return conn.call("behaviortree.add_aux_node", {"tree_path": tree_path, "node_id": node_id, "aux_class": aux_class})

    @mcp.tool()
    def behaviortree_set_node_property(tree_path: str, node_id: str, property_path: str, value_text: str) -> dict:
        """Set a property on a node instance (composite, task, decorator or service) via reflection ImportText.
        PropertyPath supports nesting and indices ("A.B[2].C"). For a Blackboard key selector property
        (FBlackboardKeySelector, e.g. "BlackboardKey") ValueText may be just the key name ("TargetActor");
        the key must exist in the tree's Blackboard and match the selector's allowed key types.
        For value-or-key properties (FValueOrBBKey_*, e.g. BTTask_Wait "WaitTime", BTTask_MoveTo "AcceptableRadius")
        ValueText "2.5" sets the default value and "bb:KeyName" binds a Blackboard key.
        On the ROOT node only "BlackboardAsset" is supported (Blackboard asset path).
        value_text: Value in UE text-import format, e.g. "5.0", "true", "(X=1,Y=2,Z=3)", "/Game/Path.Asset".
        Returns: "OK" or "ERROR|...".

        Built-in port of BlueprintInspector's BPIBehaviorTreeToolset::SetBTNodeProperty; no other plugin needed."""
        return conn.call("behaviortree.set_node_property", {"tree_path": tree_path, "node_id": node_id, "property_path": property_path, "value_text": value_text})

    @mcp.tool()
    def behaviortree_remove_node(tree_path: str, node_id: str) -> dict:
        """Remove a node. For a composite/task the whole subtree below it (children, their decorators/services) is removed too.
        For a decorator/service only that aux node is removed. The ROOT node cannot be removed.
        Returns: "OK" or "ERROR|...".

        Built-in port of BlueprintInspector's BPIBehaviorTreeToolset::RemoveBTNode; no other plugin needed."""
        return conn.call("behaviortree.remove_node", {"tree_path": tree_path, "node_id": node_id})

    @mcp.tool()
    def behaviortree_dump(tree_path: str) -> dict:
        """Dump the editor graph of a Behavior Tree as JSON with node ids usable by the tools above.
        Returns: JSON {"tree","blackboard","root":{"id","children":[node]},"disconnected":[node]} where node =
         {"id","kind","class","name","description","executionIndex","keys":{prop:keyName},"decorators":[aux],"services":[aux],"children":[node]}
         or "ERROR|...".

        Built-in port of BlueprintInspector's BPIBehaviorTreeToolset::DumpBehaviorTree; no other plugin needed."""
        return conn.call("behaviortree.dump", {"tree_path": tree_path})
