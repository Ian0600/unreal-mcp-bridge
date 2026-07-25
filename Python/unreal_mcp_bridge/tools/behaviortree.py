from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_behaviortree_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def behaviortree_get_tree(tree_path: str) -> dict:
        """Read a Behavior Tree's structure: a flat list of graph nodes, each with node_class,
        instance_class, decorators/services (with their instance properties), child node names, and
        the node instance's editable properties. Also returns the root node name and blackboard path."""
        if not tree_path:
            raise ValueError("tree_path is required")
        return conn.call("behaviortree.get_tree", {"tree_path": tree_path})

    @mcp.tool()
    def behaviortree_list_blackboard_keys(tree_path: str) -> dict:
        """List the Blackboard keys (name + key type) of the BT's assigned Blackboard asset."""
        if not tree_path:
            raise ValueError("tree_path is required")
        return conn.call("behaviortree.list_blackboard_keys", {"tree_path": tree_path})

    @mcp.tool()
    def behaviortree_add_blackboard_key(tree_path: str, key_name: str, key_type: str) -> dict:
        """Add a Blackboard key. key_type: bool|int|float|string|name|vector|rotator|object|class|enum,
        or a /Script/... UBlackboardKeyType path."""
        if not tree_path or not key_name or not key_type:
            raise ValueError("tree_path, key_name, key_type are required")
        return conn.call("behaviortree.add_blackboard_key", {
            "tree_path": tree_path,
            "key_name": key_name,
            "key_type": key_type,
        })

    @mcp.tool()
    def behaviortree_set_node_property(tree_path: str, node_name: str, property_name: str, value: str) -> dict:
        """Set a property on a BT node's (or decorator/service subnode's) runtime instance by Unreal
        import text. node_name is a name from behaviortree_get_tree."""
        if not tree_path or not node_name or not property_name:
            raise ValueError("tree_path, node_name, property_name are required")
        return conn.call("behaviortree.set_node_property", {
            "tree_path": tree_path,
            "node_name": node_name,
            "property": property_name,
            "value": value,
        })

    @mcp.tool()
    def behaviortree_compile(tree_path: str) -> dict:
        """Regenerate the BT's runtime tree from its editor graph (call after structural edits)."""
        if not tree_path:
            raise ValueError("tree_path is required")
        return conn.call("behaviortree.compile", {"tree_path": tree_path})

    @mcp.tool()
    def behaviortree_add_node(
        tree_path: str,
        node_kind: str,
        instance_class_path: str,
        parent_node_name: str = "",
        pos_x: int = 0,
        pos_y: int = 0,
    ) -> dict:
        """Add a composite or task node under a parent (default: the root). node_kind: "composite" |
        "task". instance_class_path e.g. "/Script/AIModule.BTComposite_Selector" or a BP task class.
        Call behaviortree_compile afterwards. Returns node_name."""
        if not tree_path or not node_kind or not instance_class_path:
            raise ValueError("tree_path, node_kind, instance_class_path are required")
        return conn.call("behaviortree.add_node", {
            "tree_path": tree_path,
            "node_kind": node_kind,
            "instance_class_path": instance_class_path,
            "parent_node_name": parent_node_name,
            "pos_x": pos_x,
            "pos_y": pos_y,
        })

    @mcp.tool()
    def behaviortree_add_subnode(
        tree_path: str,
        parent_node_name: str,
        subnode_kind: str,
        instance_class_path: str,
    ) -> dict:
        """Attach a decorator or service to a node. subnode_kind: "decorator" | "service".
        instance_class_path e.g. "/Script/AIModule.BTDecorator_Blackboard". Returns node_name."""
        if not tree_path or not parent_node_name or not subnode_kind or not instance_class_path:
            raise ValueError("tree_path, parent_node_name, subnode_kind, instance_class_path are required")
        return conn.call("behaviortree.add_subnode", {
            "tree_path": tree_path,
            "parent_node_name": parent_node_name,
            "subnode_kind": subnode_kind,
            "instance_class_path": instance_class_path,
        })
