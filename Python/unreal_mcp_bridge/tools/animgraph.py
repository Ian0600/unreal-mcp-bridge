from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_animgraph_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def animgraph_add_state_machine(
        blueprint_path: str,
        graph_name: str = "AnimGraph",
        pos_x: int = 0,
        pos_y: int = 0,
    ) -> dict:
        """Adds a State Machine node to an AnimBlueprint's AnimGraph. Auto-creates the
        machine's inner graph + Entry node. Returns {"node_name": ...} to pass as
        state_machine_node to animgraph_add_state / animgraph_add_transition."""
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        return conn.call("animgraph.add_state_machine", {
            "blueprint_path": blueprint_path,
            "graph_name": graph_name,
            "pos_x": pos_x,
            "pos_y": pos_y,
        })

    @mcp.tool()
    def animgraph_add_state(
        blueprint_path: str,
        state_machine_node: str,
        state_name: str,
        graph_name: str = "AnimGraph",
        pos_x: int = 0,
        pos_y: int = 0,
    ) -> dict:
        """Adds a state to a state machine's inner graph. Auto-creates the state's bound
        graph (with its Output Animation Pose sink). The first state added is auto-wired
        to the machine's Entry node."""
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        if not state_machine_node:
            raise ValueError("state_machine_node is required")
        if not state_name:
            raise ValueError("state_name is required")
        return conn.call("animgraph.add_state", {
            "blueprint_path": blueprint_path,
            "state_machine_node": state_machine_node,
            "state_name": state_name,
            "graph_name": graph_name,
            "pos_x": pos_x,
            "pos_y": pos_y,
        })

    @mcp.tool()
    def animgraph_add_blend_space_player(
        blueprint_path: str,
        state_machine_node: str,
        state_name: str,
        blend_space_path: str,
        graph_name: str = "AnimGraph",
        pos_x: int = 0,
        pos_y: int = 0,
    ) -> dict:
        """Adds a BlendSpacePlayer node inside state_name's bound graph, sets its
        BlendSpace asset, and wires it to the state's pose sink. The blend space's axis
        input pins (typically X/Y) are left unwired — connect them with a follow-up
        blueprint graph edit if the axes need to be driven by variables."""
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        if not state_machine_node:
            raise ValueError("state_machine_node is required")
        if not state_name:
            raise ValueError("state_name is required")
        if not blend_space_path:
            raise ValueError("blend_space_path is required")
        return conn.call("animgraph.add_blend_space_player", {
            "blueprint_path": blueprint_path,
            "state_machine_node": state_machine_node,
            "state_name": state_name,
            "blend_space_path": blend_space_path,
            "graph_name": graph_name,
            "pos_x": pos_x,
            "pos_y": pos_y,
        })

    @mcp.tool()
    def animgraph_add_transition(
        blueprint_path: str,
        state_machine_node: str,
        from_state: str,
        to_state: str,
        graph_name: str = "AnimGraph",
        condition_property: str = "",
        negate_condition: bool = False,
    ) -> dict:
        """Adds a transition between two states. If condition_property is set (e.g.
        "bIsInAir", a bool property on the AnimBlueprint's parent AnimInstance class),
        the transition rule is wired to `Get <condition_property>` -> Result, optionally
        negated via negate_condition."""
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        if not state_machine_node:
            raise ValueError("state_machine_node is required")
        if not from_state:
            raise ValueError("from_state is required")
        if not to_state:
            raise ValueError("to_state is required")
        params: dict = {
            "blueprint_path": blueprint_path,
            "state_machine_node": state_machine_node,
            "from_state": from_state,
            "to_state": to_state,
            "graph_name": graph_name,
            "negate_condition": negate_condition,
        }
        if condition_property:
            params["condition_property"] = condition_property
        return conn.call("animgraph.add_transition", params)

    @mcp.tool()
    def animgraph_add_variable_get(
        blueprint_path: str,
        graph_name: str,
        property_name: str,
        pos_x: int = 0,
        pos_y: int = 0,
    ) -> dict:
        """Adds a `Get <property_name>` node to a graph. graph_name resolves nested anim
        graphs too, so pass a state's name to add the getter inside that state's bound
        graph (e.g. to drive a BlendSpacePlayer's Speed axis from GroundSpeed). The new
        node's output pin is named after the property. Returns {"node_name": ...}."""
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        if not graph_name:
            raise ValueError("graph_name is required")
        if not property_name:
            raise ValueError("property_name is required")
        return conn.call("animgraph.add_variable_get", {
            "blueprint_path": blueprint_path,
            "graph_name": graph_name,
            "property_name": property_name,
            "pos_x": pos_x,
            "pos_y": pos_y,
        })

    @mcp.tool()
    def animgraph_connect_pins(
        blueprint_path: str,
        graph_name: str,
        src_node: str,
        src_pin: str,
        dst_node: str,
        dst_pin: str,
    ) -> dict:
        """Connects src_node's output pin to dst_node's input pin within one graph. Use to
        wire a variable getter into a node's input, e.g. src_pin="GroundSpeed" (a
        Get GroundSpeed node) -> dst_pin="X" (a BlendSpacePlayer's axis pin). Returns
        {"connected": true}."""
        for name, value in (
            ("blueprint_path", blueprint_path), ("graph_name", graph_name),
            ("src_node", src_node), ("src_pin", src_pin),
            ("dst_node", dst_node), ("dst_pin", dst_pin),
        ):
            if not value:
                raise ValueError(f"{name} is required")
        return conn.call("animgraph.connect_pins", {
            "blueprint_path": blueprint_path,
            "graph_name": graph_name,
            "src_node": src_node,
            "src_pin": src_pin,
            "dst_node": dst_node,
            "dst_pin": dst_pin,
        })

    @mcp.tool()
    def animgraph_disconnect_pins(
        blueprint_path: str,
        graph_name: str,
        node_a: str,
        pin_a: str,
        node_b: str,
        pin_b: str,
    ) -> dict:
        """Removes a specific link between two pins in a graph. Returns
        {"disconnected": true}."""
        for name, value in (
            ("blueprint_path", blueprint_path), ("graph_name", graph_name),
            ("node_a", node_a), ("pin_a", pin_a),
            ("node_b", node_b), ("pin_b", pin_b),
        ):
            if not value:
                raise ValueError(f"{name} is required")
        return conn.call("animgraph.disconnect_pins", {
            "blueprint_path": blueprint_path,
            "graph_name": graph_name,
            "node_a": node_a,
            "pin_a": pin_a,
            "node_b": node_b,
            "pin_b": pin_b,
        })

    @mcp.tool()
    def animgraph_add_sequence_player(
        blueprint_path: str,
        state_machine_node: str,
        state_name: str,
        sequence_path: str,
        graph_name: str = "AnimGraph",
        loop: bool = True,
        pos_x: int = 0,
        pos_y: int = 0,
    ) -> dict:
        """Adds a SequencePlayer node inside state_name's bound graph, sets its AnimSequence
        + loop flag, and wires it to the state's pose sink. Use for one-shot transition
        clips (Start/Stop) in a dedicated state. Returns {"node_name": ...}."""
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        if not state_machine_node:
            raise ValueError("state_machine_node is required")
        if not state_name:
            raise ValueError("state_name is required")
        if not sequence_path:
            raise ValueError("sequence_path is required")
        return conn.call("animgraph.add_sequence_player", {
            "blueprint_path": blueprint_path,
            "state_machine_node": state_machine_node,
            "state_name": state_name,
            "sequence_path": sequence_path,
            "graph_name": graph_name,
            "loop": loop,
            "pos_x": pos_x,
            "pos_y": pos_y,
        })

    @mcp.tool()
    def animgraph_add_slot(
        blueprint_path: str,
        slot_name: str,
        graph_name: str = "AnimGraph",
        pos_x: int = 0,
        pos_y: int = 0,
    ) -> dict:
        """Adds a Slot node (montage layering point) to the top-level AnimGraph. slot_name
        must match a slot defined on the Skeleton (e.g. "UpperBody"). Pins: "Source" (in) /
        "Pose" (out) — wire both with animgraph_connect_pins so the slot sits in series
        between the pose source and the output. Returns {"node_name": ...}."""
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        if not slot_name:
            raise ValueError("slot_name is required")
        return conn.call("animgraph.add_slot", {
            "blueprint_path": blueprint_path,
            "slot_name": slot_name,
            "graph_name": graph_name,
            "pos_x": pos_x,
            "pos_y": pos_y,
        })

    @mcp.tool()
    def animgraph_remove_node(
        blueprint_path: str,
        graph_name: str,
        node_name: str,
    ) -> dict:
        """Removes a node from the named graph (state bound graphs resolve by state name).
        Returns {"removed": true}."""
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        if not graph_name:
            raise ValueError("graph_name is required")
        if not node_name:
            raise ValueError("node_name is required")
        return conn.call("animgraph.remove_node", {
            "blueprint_path": blueprint_path,
            "graph_name": graph_name,
            "node_name": node_name,
        })

    @mcp.tool()
    def animgraph_set_preview_mesh(blueprint_path: str, mesh_path: str) -> dict:
        """Sets an AnimBlueprint's preview mesh (what Preview Scene → "Apply To Asset" stores; Python
        has no setter). mesh_path "" clears it. The mesh must be on a skeleton compatible with the
        AnimBP's target skeleton. Returns {"ok", "previous", "mesh"}. Does not save — call asset_save."""
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        return conn.call("animgraph.set_preview_mesh", {
            "blueprint_path": blueprint_path,
            "mesh_path": mesh_path,
        })
