from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_material_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    # ---------------- creation ----------------

    @mcp.tool()
    def material_create(package_path: str, asset_name: str) -> dict:
        """Create a new UMaterial asset. Returns {object_path}."""
        if not package_path or not asset_name:
            raise ValueError("package_path and asset_name are required")
        return conn.call("material.create", {
            "package_path": package_path,
            "asset_name": asset_name,
        })

    @mcp.tool()
    def material_create_instance(package_path: str, asset_name: str, parent_path: str) -> dict:
        """Create a new Material Instance Constant with the given parent material/instance."""
        if not (package_path and asset_name and parent_path):
            raise ValueError("package_path, asset_name and parent_path are required")
        return conn.call("material.create_instance", {
            "package_path": package_path,
            "asset_name": asset_name,
            "parent_path": parent_path,
        })

    @mcp.tool()
    def material_create_function(package_path: str, asset_name: str) -> dict:
        """Create a new UMaterialFunction asset."""
        if not package_path or not asset_name:
            raise ValueError("package_path and asset_name are required")
        return conn.call("material.create_function", {
            "package_path": package_path,
            "asset_name": asset_name,
        })

    # ---------------- inspection ----------------

    @mcp.tool()
    def material_get_info(object_path: str) -> dict:
        """Class, parent (for instances), expression count and parameter names for a material/instance."""
        if not object_path:
            raise ValueError("object_path is required")
        return conn.call("material.get_info", {"object_path": object_path})

    @mcp.tool()
    def material_list_parameters(object_path: str) -> dict:
        """All scalar/vector/texture/switch parameters with current values (and overridden flags for instances)."""
        if not object_path:
            raise ValueError("object_path is required")
        return conn.call("material.list_parameters", {"object_path": object_path})

    @mcp.tool()
    def material_list_expressions(
        material_path: str | None = None,
        function_path: str | None = None,
    ) -> dict:
        """List graph expression nodes (guid, class, position, pin names). Pass exactly one path."""
        return conn.call("material.list_expressions", _graph_host(material_path, function_path))

    @mcp.tool()
    def material_get_statistics(object_path: str) -> dict:
        """Shader instruction/sampler/texture-sample counts for a material or instance."""
        if not object_path:
            raise ValueError("object_path is required")
        return conn.call("material.get_statistics", {"object_path": object_path})

    # ---------------- instance parameter editing ----------------

    @mcp.tool()
    def material_set_scalar_param(
        instance_path: str, parameter_name: str, value: float,
        association: str | None = None,
    ) -> dict:
        """Set a scalar (float) parameter on a Material Instance Constant."""
        return conn.call("material.set_scalar_param",
                         _param(instance_path, parameter_name, association, value=value))

    @mcp.tool()
    def material_set_vector_param(
        instance_path: str, parameter_name: str, value: list[float],
        association: str | None = None,
    ) -> dict:
        """Set a vector parameter (value = [r,g,b] or [r,g,b,a]) on a Material Instance Constant."""
        return conn.call("material.set_vector_param",
                         _param(instance_path, parameter_name, association, value=value))

    @mcp.tool()
    def material_set_texture_param(
        instance_path: str, parameter_name: str, texture_path: str,
        association: str | None = None,
    ) -> dict:
        """Set a texture parameter on a Material Instance Constant."""
        return conn.call("material.set_texture_param",
                         _param(instance_path, parameter_name, association, texture_path=texture_path))

    @mcp.tool()
    def material_set_static_switch_param(
        instance_path: str, parameter_name: str, value: bool,
        association: str | None = None,
    ) -> dict:
        """Set a static switch (bool) parameter on a Material Instance Constant."""
        return conn.call("material.set_static_switch_param",
                         _param(instance_path, parameter_name, association, value=value))

    @mcp.tool()
    def material_set_parameter_override(
        instance_path: str, parameter_name: str, override: bool,
        association: str | None = None,
    ) -> dict:
        """Enable/disable a parameter override on a Material Instance Constant."""
        return conn.call("material.set_parameter_override",
                         _param(instance_path, parameter_name, association, override=override))

    @mcp.tool()
    def material_set_instance_parent(instance_path: str, parent_path: str) -> dict:
        """Change the parent of a Material Instance Constant."""
        if not (instance_path and parent_path):
            raise ValueError("instance_path and parent_path are required")
        return conn.call("material.set_instance_parent", {
            "instance_path": instance_path,
            "parent_path": parent_path,
        })

    @mcp.tool()
    def material_clear_instance_parameters(instance_path: str) -> dict:
        """Clear all parameter overrides set by a Material Instance Constant."""
        if not instance_path:
            raise ValueError("instance_path is required")
        return conn.call("material.clear_instance_parameters", {"instance_path": instance_path})

    @mcp.tool()
    def material_update_instance(instance_path: str) -> dict:
        """Recompile shaders for a Material Instance Constant after edits."""
        if not instance_path:
            raise ValueError("instance_path is required")
        return conn.call("material.update_instance", {"instance_path": instance_path})

    # ---------------- graph editing ----------------

    @mcp.tool()
    def material_add_expression(
        expression_class: str,
        material_path: str | None = None,
        function_path: str | None = None,
        pos_x: int = 0,
        pos_y: int = 0,
        asset_path: str | None = None,
    ) -> dict:
        """Add an expression node. expression_class is a class name (e.g. 'MaterialExpressionScalarParameter')
        or full path. Optionally seed from asset_path (e.g. a Texture). Returns {guid}."""
        if not expression_class:
            raise ValueError("expression_class is required")
        params = _graph_host(material_path, function_path)
        params.update({"expression_class": expression_class, "pos_x": pos_x, "pos_y": pos_y})
        if asset_path:
            params["asset_path"] = asset_path
        return conn.call("material.add_expression", params)

    @mcp.tool()
    def material_set_expression_property(
        expression_guid: str, property: str, value: str,
        material_path: str | None = None,
        function_path: str | None = None,
    ) -> dict:
        """Set a property on an expression node by reflection (value = Unreal import text, e.g. '1.0',
        '(R=1,G=0,B=0)', or a parameter name string)."""
        if not (expression_guid and property):
            raise ValueError("expression_guid and property are required")
        params = _graph_host(material_path, function_path)
        params.update({"expression_guid": expression_guid, "property": property, "value": value})
        return conn.call("material.set_expression_property", params)

    @mcp.tool()
    def material_connect_expressions(
        from_guid: str, to_guid: str,
        from_output: str = "", to_input: str = "",
        material_path: str | None = None,
        function_path: str | None = None,
    ) -> dict:
        """Connect an output pin of one expression to an input pin of another. Empty pin name = first pin."""
        if not (from_guid and to_guid):
            raise ValueError("from_guid and to_guid are required")
        params = _graph_host(material_path, function_path)
        params.update({
            "from_guid": from_guid, "to_guid": to_guid,
            "from_output": from_output, "to_input": to_input,
        })
        return conn.call("material.connect_expressions", params)

    @mcp.tool()
    def material_connect_property(
        material_path: str, from_guid: str, property: str, from_output: str = "",
    ) -> dict:
        """Connect an expression output to a material property input (property e.g. 'BaseColor', 'Roughness')."""
        if not (material_path and from_guid and property):
            raise ValueError("material_path, from_guid and property are required")
        return conn.call("material.connect_property", {
            "material_path": material_path, "from_guid": from_guid,
            "property": property, "from_output": from_output,
        })

    @mcp.tool()
    def material_disconnect_expression(
        to_guid: str, to_input: str,
        material_path: str | None = None,
        function_path: str | None = None,
    ) -> dict:
        """Clear an input pin of an expression node."""
        if not (to_guid and to_input):
            raise ValueError("to_guid and to_input are required")
        params = _graph_host(material_path, function_path)
        params.update({"to_guid": to_guid, "to_input": to_input})
        return conn.call("material.disconnect_expression", params)

    @mcp.tool()
    def material_disconnect_property(material_path: str, property: str) -> dict:
        """Clear a material property input (e.g. 'BaseColor')."""
        if not (material_path and property):
            raise ValueError("material_path and property are required")
        return conn.call("material.disconnect_property", {
            "material_path": material_path, "property": property,
        })

    @mcp.tool()
    def material_delete_expression(
        expression_guid: str,
        material_path: str | None = None,
        function_path: str | None = None,
    ) -> dict:
        """Delete a single expression node (disconnects it first)."""
        if not expression_guid:
            raise ValueError("expression_guid is required")
        params = _graph_host(material_path, function_path)
        params["expression_guid"] = expression_guid
        return conn.call("material.delete_expression", params)

    @mcp.tool()
    def material_delete_all_expressions(
        material_path: str | None = None,
        function_path: str | None = None,
    ) -> dict:
        """Delete every expression node in a material or function graph."""
        return conn.call("material.delete_all_expressions", _graph_host(material_path, function_path))

    @mcp.tool()
    def material_delete_unused_expressions(material_path: str) -> dict:
        """Delete expression nodes unreachable from any material output."""
        if not material_path:
            raise ValueError("material_path is required")
        return conn.call("material.delete_unused_expressions", {"material_path": material_path})

    @mcp.tool()
    def material_layout_expressions(
        material_path: str | None = None,
        function_path: str | None = None,
    ) -> dict:
        """Auto-arrange expression nodes in a grid."""
        return conn.call("material.layout_expressions", _graph_host(material_path, function_path))

    # ---------------- named reroute (graph-local "named variable") nodes ----------------

    @mcp.tool()
    def material_add_named_reroute_declaration(
        name: str,
        material_path: str | None = None,
        function_path: str | None = None,
        pos_x: int = 0,
        pos_y: int = 0,
        node_color: list[float] | None = None,
    ) -> dict:
        """Create a Named Reroute Declaration (a graph-local named variable). Feed a value into it by
        connecting an expression to its input (material_connect_expressions with empty to_input).
        Returns {guid, variable_guid, name}."""
        if not name:
            raise ValueError("name is required")
        params = _graph_host(material_path, function_path)
        params.update({"name": name, "pos_x": pos_x, "pos_y": pos_y})
        if node_color is not None:
            params["node_color"] = node_color
        return conn.call("material.add_named_reroute_declaration", params)

    @mcp.tool()
    def material_add_named_reroute_usage(
        declaration_guid: str,
        material_path: str | None = None,
        function_path: str | None = None,
        pos_x: int = 0,
        pos_y: int = 0,
    ) -> dict:
        """Create a Named Reroute Usage linked to a declaration (declaration_guid = the declaration
        NODE's guid). Wire its output onward with material_connect_expressions. Returns {guid}."""
        if not declaration_guid:
            raise ValueError("declaration_guid is required")
        params = _graph_host(material_path, function_path)
        params.update({"declaration_guid": declaration_guid, "pos_x": pos_x, "pos_y": pos_y})
        return conn.call("material.add_named_reroute_usage", params)

    @mcp.tool()
    def material_set_named_reroute_name(
        expression_guid: str,
        name: str,
        material_path: str | None = None,
        function_path: str | None = None,
    ) -> dict:
        """Rename a Named Reroute Declaration. Linked usages reflect the new name automatically."""
        if not (expression_guid and name):
            raise ValueError("expression_guid and name are required")
        params = _graph_host(material_path, function_path)
        params.update({"expression_guid": expression_guid, "name": name})
        return conn.call("material.set_named_reroute_name", params)

    @mcp.tool()
    def material_list_named_reroutes(
        material_path: str | None = None,
        function_path: str | None = None,
    ) -> dict:
        """List Named Reroute declarations and usages with their linkage. Returns {declarations, usages}."""
        return conn.call("material.list_named_reroutes", _graph_host(material_path, function_path))

    # ---------------- comment boxes ----------------

    @mcp.tool()
    def material_add_comment(
        text: str,
        material_path: str | None = None,
        function_path: str | None = None,
        pos_x: int = 0,
        pos_y: int = 0,
        size_x: int = 400,
        size_y: int = 200,
        font_size: int = 18,
        color: list[float] | None = None,
    ) -> dict:
        """Add a comment box to a material/function graph. Returns {guid}."""
        if not text:
            raise ValueError("text is required")
        params = _graph_host(material_path, function_path)
        params.update({"text": text, "pos_x": pos_x, "pos_y": pos_y,
                       "size_x": size_x, "size_y": size_y, "font_size": font_size})
        if color is not None:
            params["color"] = color
        return conn.call("material.add_comment", params)

    @mcp.tool()
    def material_list_comments(
        material_path: str | None = None,
        function_path: str | None = None,
    ) -> dict:
        """List comment boxes (guid, text, position, size, color, font_size)."""
        return conn.call("material.list_comments", _graph_host(material_path, function_path))

    @mcp.tool()
    def material_set_comment_text(
        comment_guid: str,
        text: str,
        material_path: str | None = None,
        function_path: str | None = None,
    ) -> dict:
        """Set a comment box's text."""
        if not comment_guid:
            raise ValueError("comment_guid is required")
        params = _graph_host(material_path, function_path)
        params.update({"comment_guid": comment_guid, "text": text})
        return conn.call("material.set_comment_text", params)

    @mcp.tool()
    def material_delete_comment(
        comment_guid: str,
        material_path: str | None = None,
        function_path: str | None = None,
    ) -> dict:
        """Delete a comment box."""
        if not comment_guid:
            raise ValueError("comment_guid is required")
        params = _graph_host(material_path, function_path)
        params["comment_guid"] = comment_guid
        return conn.call("material.delete_comment", params)

    # ---------------- base properties / build ----------------

    @mcp.tool()
    def material_set_property(object_path: str, property: str, value: str) -> dict:
        """Set a property on the material/instance/function object itself by reflection
        (e.g. property='BlendMode', value='BLEND_Translucent'; property='TwoSided', value='true')."""
        if not (object_path and property):
            raise ValueError("object_path and property are required")
        return conn.call("material.set_property", {
            "object_path": object_path, "property": property, "value": value,
        })

    @mcp.tool()
    def material_recompile(material_path: str) -> dict:
        """Recompile a material after graph edits. Returns {compiled, errors[]}."""
        if not material_path:
            raise ValueError("material_path is required")
        return conn.call("material.recompile", {"material_path": material_path})

    @mcp.tool()
    def material_update_function(function_path: str) -> dict:
        """Recompile materials that use a material function after editing it."""
        if not function_path:
            raise ValueError("function_path is required")
        return conn.call("material.update_function", {"function_path": function_path})

    # ---------------- placement / layout helpers ----------------

    @mcp.tool()
    def material_set_node_position(
        expression_guid: str,
        pos_x: int,
        pos_y: int,
        material_path: str | None = None,
        function_path: str | None = None,
        snap: bool = True,
    ) -> dict:
        """Move an expression node to a (16px-snapped) position."""
        if not expression_guid:
            raise ValueError("expression_guid is required")
        params = _graph_host(material_path, function_path)
        params.update({"expression_guid": expression_guid, "pos_x": pos_x, "pos_y": pos_y, "snap": snap})
        return conn.call("material.set_node_position", params)

    @mcp.tool()
    def material_arrange_grid(
        columns: list[list[str]],
        material_path: str | None = None,
        function_path: str | None = None,
        origin_x: int = 0,
        origin_y: int = 0,
        col_step: int = 256,
        row_step: int = 80,
        snap: bool = True,
    ) -> dict:
        """Fast placement: lay nodes out as left->right columns of top->down rows. columns[i][j] is an
        expression guid placed at (origin_x + i*col_step, origin_y + j*row_step), snapped to 16px."""
        if not columns:
            raise ValueError("columns (list of lists of guids) is required")
        params = _graph_host(material_path, function_path)
        params.update({"columns": columns, "origin_x": origin_x, "origin_y": origin_y,
                       "col_step": col_step, "row_step": row_step, "snap": snap})
        return conn.call("material.arrange_grid", params)

    @mcp.tool()
    def material_add_channel_reroutes(
        material_path: str | None = None,
        function_path: str | None = None,
        channels: list[str] | None = None,
        origin_x: int = 0,
        origin_y: int = 0,
        row_step: int = 80,
    ) -> dict:
        """Create the standard PBR channel Named Reroute declarations (AO/Diffuse/F0/Roughness/Normal/
        Emissive/UVs) with the project's fixed channel colors, stacked vertically. Pass `channels` to
        pick a subset/order. Returns {declarations}."""
        params = _graph_host(material_path, function_path)
        params.update({"origin_x": origin_x, "origin_y": origin_y, "row_step": row_step})
        if channels is not None:
            params["channels"] = channels
        return conn.call("material.add_channel_reroutes", params)

    @mcp.tool()
    def material_add_group_comment(
        text: str,
        node_guids: list[str],
        material_path: str | None = None,
        function_path: str | None = None,
        padding: int = 48,
        font_size: int = 18,
        color: list[float] | None = None,
    ) -> dict:
        """Create a group-box comment auto-sized around the given expression nodes, using the project's
        standard group style. Returns {guid, pos_x, pos_y, size_x, size_y}."""
        if not text:
            raise ValueError("text is required")
        if not node_guids:
            raise ValueError("node_guids (non-empty list) is required")
        params = _graph_host(material_path, function_path)
        params.update({"text": text, "node_guids": node_guids, "padding": padding, "font_size": font_size})
        if color is not None:
            params["color"] = color
        return conn.call("material.add_group_comment", params)


def _graph_host(material_path: str | None, function_path: str | None) -> dict:
    """Build the params dict selecting exactly one graph host."""
    if bool(material_path) == bool(function_path):
        raise ValueError("provide exactly one of material_path or function_path")
    return {"material_path": material_path} if material_path else {"function_path": function_path}


def _param(instance_path: str, parameter_name: str, association: str | None, **extra) -> dict:
    if not (instance_path and parameter_name):
        raise ValueError("instance_path and parameter_name are required")
    params = {"instance_path": instance_path, "parameter_name": parameter_name}
    if association:
        params["association"] = association
    params.update(extra)
    return params
