from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_blueprint_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def blueprint_create(
        package_path: str,
        asset_name: str,
        parent_class_path: str = "",
        blueprint_type: str = "normal",
    ) -> dict:
        """Create a Blueprint asset. blueprint_type: normal | const | interface.
        parent_class_path is required for normal/const; ignored for interface (parent is UInterface)."""
        if not package_path:
            raise ValueError("package_path is required")
        if not asset_name:
            raise ValueError("asset_name is required")
        if blueprint_type.lower() != "interface" and not parent_class_path:
            raise ValueError("parent_class_path is required (except for blueprint_type=interface)")
        params: dict = {
            "package_path": package_path,
            "asset_name": asset_name,
            "blueprint_type": blueprint_type,
        }
        if parent_class_path:
            params["parent_class_path"] = parent_class_path
        return conn.call("blueprint.create", params)

    @mcp.tool()
    def blueprint_compile(blueprint_path: str) -> dict:
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        return conn.call("blueprint.compile", {"blueprint_path": blueprint_path})

    @mcp.tool()
    def blueprint_list_variables(blueprint_path: str) -> dict:
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        return conn.call("blueprint.list_variables", {"blueprint_path": blueprint_path})

    @mcp.tool()
    def blueprint_get_graph_nodes(blueprint_path: str, graph_name: str = "") -> dict:
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        params: dict = {"blueprint_path": blueprint_path}
        if graph_name:
            params["graph_name"] = graph_name
        return conn.call("blueprint.get_graph_nodes", params)

    @mcp.tool()
    def blueprint_list_functions(blueprint_path: str, graph_name: str = "") -> dict:
        """Lightweight inventory of a Blueprint's graphs: name, graph_type
        (ubergraph/function/macro), node_count, and for function graphs the
        resolved input/output signature. Use this before blueprint_get_graph_nodes
        to see what's there without pulling every node."""
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        params: dict = {"blueprint_path": blueprint_path}
        if graph_name:
            params["graph_name"] = graph_name
        return conn.call("blueprint.list_functions", params)

    # ---- authoring: structure ----

    @mcp.tool()
    def blueprint_add_variable(
        blueprint_path: str,
        var_name: str,
        var_type: str,
        sub_type_path: str = "",
        is_array: bool = False,
        default_value: str = "",
    ) -> dict:
        """Add a member variable. var_type: bool|byte|int|int64|float|string|name|text|
        vector|rotator|transform|object|class|struct|enum, or a /Script/... path. For
        object/class/struct/enum, pass the class/struct/enum path in sub_type_path
        (e.g. "/Script/Engine.StaticMeshComponent")."""
        if not blueprint_path or not var_name or not var_type:
            raise ValueError("blueprint_path, var_name, var_type are required")
        return conn.call("blueprint.add_variable", {
            "blueprint_path": blueprint_path,
            "var_name": var_name,
            "type": var_type,
            "sub_type_path": sub_type_path,
            "is_array": is_array,
            "default_value": default_value,
        })

    @mcp.tool()
    def blueprint_add_function(blueprint_path: str, function_name: str) -> dict:
        """Add a new (empty) user function graph with editable entry/result nodes."""
        if not blueprint_path or not function_name:
            raise ValueError("blueprint_path and function_name are required")
        return conn.call("blueprint.add_function", {
            "blueprint_path": blueprint_path,
            "function_name": function_name,
        })

    @mcp.tool()
    def blueprint_add_component(
        blueprint_path: str,
        component_class_path: str,
        component_name: str,
        parent_component: str = "",
    ) -> dict:
        """Add a component to the Blueprint's construction script. component_class_path e.g.
        "/Script/Engine.StaticMeshComponent". If parent_component names an existing SCS
        component, the new one attaches under it; otherwise it is added at the root."""
        if not blueprint_path or not component_class_path or not component_name:
            raise ValueError("blueprint_path, component_class_path, component_name are required")
        return conn.call("blueprint.add_component", {
            "blueprint_path": blueprint_path,
            "component_class_path": component_class_path,
            "component_name": component_name,
            "parent_component": parent_component,
        })

    @mcp.tool()
    def blueprint_set_class_default(blueprint_path: str, property_name: str, value: str) -> dict:
        """Set a property on the Blueprint's Class Defaults (CDO) by Unreal import text
        (e.g. value "true", "1.5", "(X=1,Y=2,Z=0)")."""
        if not blueprint_path or not property_name:
            raise ValueError("blueprint_path and property_name are required")
        return conn.call("blueprint.set_class_default", {
            "blueprint_path": blueprint_path,
            "property": property_name,
            "value": value,
        })

    # ---- authoring: graph nodes ----

    @mcp.tool()
    def blueprint_add_node(
        blueprint_path: str,
        graph_name: str,
        node_class_path: str,
        pos_x: int = 0,
        pos_y: int = 0,
    ) -> dict:
        """Spawn a K2 node of an arbitrary class (e.g.
        "/Script/BlueprintGraph.K2Node_CustomEvent") into the named graph. Returns node_name."""
        if not blueprint_path or not graph_name or not node_class_path:
            raise ValueError("blueprint_path, graph_name, node_class_path are required")
        return conn.call("blueprint.add_node", {
            "blueprint_path": blueprint_path,
            "graph_name": graph_name,
            "node_class_path": node_class_path,
            "pos_x": pos_x,
            "pos_y": pos_y,
        })

    @mcp.tool()
    def blueprint_add_call_function(
        blueprint_path: str,
        graph_name: str,
        function_class_path: str,
        function_name: str,
        pos_x: int = 0,
        pos_y: int = 0,
    ) -> dict:
        """Add a Call Function node calling function_class_path::function_name. Returns node_name."""
        if not blueprint_path or not graph_name or not function_class_path or not function_name:
            raise ValueError("blueprint_path, graph_name, function_class_path, function_name are required")
        return conn.call("blueprint.add_call_function", {
            "blueprint_path": blueprint_path,
            "graph_name": graph_name,
            "function_class_path": function_class_path,
            "function_name": function_name,
            "pos_x": pos_x,
            "pos_y": pos_y,
        })

    @mcp.tool()
    def blueprint_add_branch(blueprint_path: str, graph_name: str, pos_x: int = 0, pos_y: int = 0) -> dict:
        """Add a Branch (if/then/else) node. Returns node_name."""
        if not blueprint_path or not graph_name:
            raise ValueError("blueprint_path and graph_name are required")
        return conn.call("blueprint.add_branch", {
            "blueprint_path": blueprint_path,
            "graph_name": graph_name,
            "pos_x": pos_x,
            "pos_y": pos_y,
        })

    @mcp.tool()
    def blueprint_add_variable_get(
        blueprint_path: str, graph_name: str, property_name: str, pos_x: int = 0, pos_y: int = 0
    ) -> dict:
        """Add a Get node for a variable on the Blueprint's own/parent class. Returns node_name."""
        if not blueprint_path or not graph_name or not property_name:
            raise ValueError("blueprint_path, graph_name, property_name are required")
        return conn.call("blueprint.add_variable_get", {
            "blueprint_path": blueprint_path,
            "graph_name": graph_name,
            "property": property_name,
            "pos_x": pos_x,
            "pos_y": pos_y,
        })

    @mcp.tool()
    def blueprint_add_variable_set(
        blueprint_path: str, graph_name: str, property_name: str, pos_x: int = 0, pos_y: int = 0
    ) -> dict:
        """Add a Set node for a variable on the Blueprint's own/parent class. Returns node_name."""
        if not blueprint_path or not graph_name or not property_name:
            raise ValueError("blueprint_path, graph_name, property_name are required")
        return conn.call("blueprint.add_variable_set", {
            "blueprint_path": blueprint_path,
            "graph_name": graph_name,
            "property": property_name,
            "pos_x": pos_x,
            "pos_y": pos_y,
        })

    @mcp.tool()
    def blueprint_set_pin_default(
        blueprint_path: str, graph_name: str, node_name: str, pin_name: str, value: str
    ) -> dict:
        """Set a node input pin's literal default value (what typing into the pin does)."""
        if not blueprint_path or not graph_name or not node_name or not pin_name:
            raise ValueError("blueprint_path, graph_name, node_name, pin_name are required")
        return conn.call("blueprint.set_pin_default", {
            "blueprint_path": blueprint_path,
            "graph_name": graph_name,
            "node_name": node_name,
            "pin_name": pin_name,
            "value": value,
        })

    @mcp.tool()
    def blueprint_connect_pins(
        blueprint_path: str, graph_name: str, src_node: str, src_pin: str, dst_node: str, dst_pin: str
    ) -> dict:
        """Connect src_node.src_pin -> dst_node.dst_pin through the graph schema."""
        if not all([blueprint_path, graph_name, src_node, src_pin, dst_node, dst_pin]):
            raise ValueError("blueprint_path, graph_name, src_node, src_pin, dst_node, dst_pin are required")
        return conn.call("blueprint.connect_pins", {
            "blueprint_path": blueprint_path,
            "graph_name": graph_name,
            "src_node": src_node,
            "src_pin": src_pin,
            "dst_node": dst_node,
            "dst_pin": dst_pin,
        })

    @mcp.tool()
    def blueprint_disconnect_pins(
        blueprint_path: str, graph_name: str, node_a: str, pin_a: str, node_b: str, pin_b: str
    ) -> dict:
        """Remove the link between node_a.pin_a and node_b.pin_b."""
        if not all([blueprint_path, graph_name, node_a, pin_a, node_b, pin_b]):
            raise ValueError("blueprint_path, graph_name, node_a, pin_a, node_b, pin_b are required")
        return conn.call("blueprint.disconnect_pins", {
            "blueprint_path": blueprint_path,
            "graph_name": graph_name,
            "node_a": node_a,
            "pin_a": pin_a,
            "node_b": node_b,
            "pin_b": pin_b,
        })

    @mcp.tool()
    def blueprint_remove_node(blueprint_path: str, graph_name: str, node_name: str) -> dict:
        """Remove a node from the named graph."""
        if not blueprint_path or not graph_name or not node_name:
            raise ValueError("blueprint_path, graph_name, node_name are required")
        return conn.call("blueprint.remove_node", {
            "blueprint_path": blueprint_path,
            "graph_name": graph_name,
            "node_name": node_name,
        })

    # ---- authoring: interfaces & function signatures ----

    @mcp.tool()
    def blueprint_implement_interface(blueprint_path: str, interface_class_path: str) -> dict:
        """Add an implemented interface to a Blueprint. interface_class_path is a native interface
        (e.g. "/Script/Module.UMyInterface") or a BP interface's generated class
        (e.g. "/Game/BPI_Thing.BPI_Thing_C")."""
        if not blueprint_path or not interface_class_path:
            raise ValueError("blueprint_path and interface_class_path are required")
        return conn.call("blueprint.implement_interface", {
            "blueprint_path": blueprint_path,
            "interface_class_path": interface_class_path,
        })

    @mcp.tool()
    def blueprint_add_function_param(
        blueprint_path: str,
        function_graph: str,
        param_name: str,
        param_type: str,
        sub_type_path: str = "",
        is_array: bool = False,
        is_output: bool = False,
    ) -> dict:
        """Add an input (is_output=False) or output (is_output=True) parameter to a function graph's
        signature. Works for normal functions and Blueprint Interface functions. param_type as in
        blueprint_add_variable. Returns pin_name."""
        if not blueprint_path or not function_graph or not param_name or not param_type:
            raise ValueError("blueprint_path, function_graph, param_name, param_type are required")
        return conn.call("blueprint.add_function_param", {
            "blueprint_path": blueprint_path,
            "function_graph": function_graph,
            "param_name": param_name,
            "type": param_type,
            "sub_type_path": sub_type_path,
            "is_array": is_array,
            "is_output": is_output,
        })
