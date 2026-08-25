from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_camerarig_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def camerarig_get_info(rig_path: str) -> dict:
        """Lists a Gameplay Camera System UCameraRigAsset's camera nodes and how they are
        wired. Returns {"root_node_class", "root_node", "needs_build", "nodes":
        [{class, name, enabled, connections: [{property, index, node}]}]}.

        connections names the property each link lives on ("Children" index 3, "InputSlot"
        index -1), which is what camerarig_set_node_children and object_set_property both
        address. Use "name" (not class + index) to target the other camerarig tools —
        indices shift whenever a node is added or removed."""
        if not rig_path:
            raise ValueError("rig_path is required")
        return conn.call("camerarig.get_info", {"rig_path": rig_path})

    @mcp.tool()
    def camerarig_set_node_property(
        rig_path: str,
        property_name: str,
        value: str,
        node_name: str = "",
        node_class: str = "",
        node_index: int = 0,
    ) -> dict:
        """Sets a property on a camera node via reflection ImportText. Target the node with
        node_name (exact, preferred) or node_class + node_index. value uses Unreal's text
        format; camera parameters wrap their payload in a Value field, e.g.
        property_name="BoomOffset", value="(Value=(X=-200.0,Y=60.0,Z=60.0))".

        Careful: a node property wired to an interface parameter IGNORES what you write
        here — the evaluator reads it from the variable table, seeded from the rig's
        parameter defaults. camerarig_list_parameters tells you which properties those
        are, and camerarig_set_parameter_default is what moves them.

        Returns {"ok": true, "node": ...}."""
        for name, val in (("rig_path", rig_path), ("property_name", property_name), ("value", value)):
            if not val:
                raise ValueError(f"{name} is required")
        if not node_name and not node_class:
            raise ValueError("either node_name or node_class is required")
        return conn.call("camerarig.set_node_property", {
            "rig_path": rig_path,
            "node_name": node_name,
            "node_class": node_class,
            "property_name": property_name,
            "value": value,
            "node_index": node_index,
        })

    @mcp.tool()
    def camerarig_list_parameters(rig_path: str) -> dict:
        """Lists the rig's exposed interface parameters and their default values.

        These defaults are what the rig actually evaluates with for every node property
        wired to a parameter — in that case the node's own field is dead, whatever it
        shows. Check here first when a node's value "does not take".

        Returns {"needs_build", "parameters": [{name, type, visible, value_type, value}]}."""
        if not rig_path:
            raise ValueError("rig_path is required")
        return conn.call("camerarig.list_parameters", {"rig_path": rig_path})

    @mcp.tool()
    def camerarig_set_parameter_default(
        rig_path: str,
        parameter_name: str,
        value: str,
        member: str = "",
    ) -> dict:
        """Writes an exposed parameter's default value — the value the rig really runs with.

        A blendable parameter is stored as a camera-parameter struct that wraps the payload
        next to its variable binding:
          (Value=(X=-144.0,Y=45.0,Z=40.0),VariableID=(Value=3629198531),Variable=None)
        Retyping all of that risks clobbering the binding, so pass member="Value" with just
        the payload — "(X=-144.0,Y=45.0,Z=40.0)" — to write the inner field and leave the
        binding alone.

        Follow with camerarig_build, then asset_save. Returns {"ok", "parameter",
        "old_value", "new_value"}."""
        for name, val in (("rig_path", rig_path), ("parameter_name", parameter_name), ("value", value)):
            if not val:
                raise ValueError(f"{name} is required")
        return conn.call("camerarig.set_parameter_default", {
            "rig_path": rig_path,
            "parameter_name": parameter_name,
            "value": value,
            "member": member,
        })

    @mcp.tool()
    def camerarig_add_node(
        rig_path: str,
        node_class: str,
        parent_node: str = "",
        parent_property: str = "Children",
        index: int = -1,
    ) -> dict:
        """Creates a camera node in the rig. node_class is a short class name
        ("OffsetCameraNode", "BoomArmCameraNode").

        With parent_node set, the node is wired into that parent's parent_property —
        an array slot ("Children", at index, appended when index < 0) or a single pointer
        slot ("InputSlot"). Without it the node is created loose: present in the asset and
        visible in the rig editor, unconnected.

        Follow with camerarig_build, then asset_save. Returns {"ok", "node", "node_class"}."""
        for name, val in (("rig_path", rig_path), ("node_class", node_class)):
            if not val:
                raise ValueError(f"{name} is required")
        return conn.call("camerarig.add_node", {
            "rig_path": rig_path,
            "node_class": node_class,
            "parent_node": parent_node,
            "parent_property": parent_property,
            "index": index,
        })

    @mcp.tool()
    def camerarig_remove_node(
        rig_path: str,
        node_name: str = "",
        node_class: str = "",
        node_index: int = 0,
    ) -> dict:
        """Deletes a camera node from the rig, the way the rig editor does: every reference
        to it is cleared (array entries are removed, not left as null holes), it is dropped
        from the rig's node-graph object list, and the object is moved to the transient
        package so a later save cannot bring it back.

        Target with node_name (exact, preferred) or node_class + node_index. Follow with
        camerarig_build, then asset_save.

        Returns {"ok", "removed", "removed_class", "cleared_references": [...]}."""
        if not rig_path:
            raise ValueError("rig_path is required")
        if not node_name and not node_class:
            raise ValueError("either node_name or node_class is required")
        return conn.call("camerarig.remove_node", {
            "rig_path": rig_path,
            "node_name": node_name,
            "node_class": node_class,
            "node_index": node_index,
        })

    @mcp.tool()
    def camerarig_set_node_children(
        rig_path: str,
        children: list[str],
        node_name: str = "",
        node_class: str = "",
        node_index: int = 0,
        property: str = "Children",
    ) -> dict:
        """Rewrites a node's child list by node name. children is an array of node names,
        with "None" for an empty slot; the array replaces the existing one wholesale.

        The order IS the evaluation order, and that decides what the rig does — a node that
        sets the camera location means one thing ahead of a boom arm and something else
        entirely after it. Every name is resolved before anything is written, so a typo
        changes nothing.

        Follow with camerarig_build, then asset_save. Returns {"ok", "node", "children"}."""
        if not rig_path:
            raise ValueError("rig_path is required")
        if not node_name and not node_class:
            raise ValueError("either node_name or node_class is required")
        if children is None:
            raise ValueError("children is required")
        return conn.call("camerarig.set_node_children", {
            "rig_path": rig_path,
            "node_name": node_name,
            "node_class": node_class,
            "node_index": node_index,
            "property": property,
            "children": list(children),
        })

    @mcp.tool()
    def camerarig_build(rig_path: str) -> dict:
        """Rebuilds the camera rig (validates data + rebuilds allocation info and parameter
        defaults) and marks it dirty. Call after any camerarig_* edit, then asset_save.
        Returns {"ok": true}."""
        if not rig_path:
            raise ValueError("rig_path is required")
        return conn.call("camerarig.build", {"rig_path": rig_path})
