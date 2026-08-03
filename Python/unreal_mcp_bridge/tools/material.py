from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def _graph_host(material_path: str, function_path: str) -> dict:
    """Builds the {material_path | function_path} pair every graph-editing command takes.

    A material graph and a material function graph are edited through the same commands but
    live on different objects, so the C++ side accepts exactly one of the two and rejects
    both-or-neither. Validating here turns that into a plain Python error instead of a
    round trip."""
    if bool(material_path) == bool(function_path):
        raise ValueError("provide exactly one of material_path or function_path")
    return {"material_path": material_path} if material_path else {"function_path": function_path}


def register_material_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    # ---------------------------------------------------------------- creation

    @mcp.tool()
    def material_create(package_path: str, asset_name: str) -> dict:
        """Creates a new empty UMaterial asset. package_path is the content folder
        (e.g. "/Game/02_Geo/Materials"), asset_name the asset name. Build its graph with
        material_add_expression / material_connect_property, then material_recompile and
        asset_save. Returns {"object_path": ...}."""
        if not package_path:
            raise ValueError("package_path is required")
        if not asset_name:
            raise ValueError("asset_name is required")
        return conn.call("material.create", {
            "package_path": package_path,
            "asset_name": asset_name,
        })

    @mcp.tool()
    def material_create_instance(package_path: str, asset_name: str, parent_path: str) -> dict:
        """Creates a UMaterialInstanceConstant parented to parent_path (a material or another
        instance). Override its parameters with the material_set_*_param tools, then
        material_update_instance and asset_save. Returns {"object_path": ...}."""
        if not package_path:
            raise ValueError("package_path is required")
        if not asset_name:
            raise ValueError("asset_name is required")
        if not parent_path:
            raise ValueError("parent_path is required")
        return conn.call("material.create_instance", {
            "package_path": package_path,
            "asset_name": asset_name,
            "parent_path": parent_path,
        })

    @mcp.tool()
    def material_create_function(package_path: str, asset_name: str) -> dict:
        """Creates a new empty UMaterialFunction asset. Its graph is edited with the same
        expression commands, passing function_path instead of material_path. Call
        material_update_function afterwards so materials using it recompile.
        Returns {"object_path": ...}."""
        if not package_path:
            raise ValueError("package_path is required")
        if not asset_name:
            raise ValueError("asset_name is required")
        return conn.call("material.create_function", {
            "package_path": package_path,
            "asset_name": asset_name,
        })

    # -------------------------------------------------------------- inspection

    @mcp.tool()
    def material_get_info(object_path: str) -> dict:
        """Summary of a material or material instance: class, parent (instances only),
        expression count (base materials only) and the names of its scalar / vector /
        texture / static-switch parameters. Returns
        {"class","object_path","parent"?,"num_expressions"?,"scalar_parameters",
        "vector_parameters","texture_parameters","static_switch_parameters"}."""
        if not object_path:
            raise ValueError("object_path is required")
        return conn.call("material.get_info", {"object_path": object_path})

    @mcp.tool()
    def material_list_parameters(object_path: str) -> dict:
        """Lists every parameter with its current value — the default value for a base
        material, or the instance value plus an "overridden" flag for a material instance.
        Returns {"scalars":[{name,value,overridden?}], "vectors":[{name,value:[r,g,b,a],...}],
        "textures":[{name,value:<asset path>,...}], "static_switches":[{name,value,...}]}."""
        if not object_path:
            raise ValueError("object_path is required")
        return conn.call("material.list_parameters", {"object_path": object_path})

    @mcp.tool()
    def material_list_expressions(material_path: str = "", function_path: str = "") -> dict:
        """Lists the graph's expression nodes: guid, class, grid position, and input/output pin
        names. The guid is what every other graph command uses to address a node.

        Node size_x/size_y are only included when the asset editor happens to have this graph
        open — a node's width depends on its title, pins and preview, so it exists only as the
        live widget's size and is not stored on the asset. "sizes_available" in the result says
        which case you got, so a layout pass can tell "editor closed" from "no nodes".
        Returns {"expressions":[{guid,class,pos_x,pos_y,size_x?,size_y?,inputs,outputs}],
        "sizes_available": bool}."""
        return conn.call("material.list_expressions", _graph_host(material_path, function_path))

    @mcp.tool()
    def material_get_statistics(object_path: str) -> dict:
        """Shader cost of a material or instance. Returns
        {"vertex_shader_instructions","pixel_shader_instructions","samplers",
        "vertex_texture_samples","pixel_texture_samples","virtual_texture_samples"}."""
        if not object_path:
            raise ValueError("object_path is required")
        return conn.call("material.get_statistics", {"object_path": object_path})

    # ----------------------------------------------- instance parameter editing

    @mcp.tool()
    def material_set_scalar_param(
        instance_path: str,
        parameter_name: str,
        value: float,
        association: str = "",
    ) -> dict:
        """Sets a scalar parameter override on a material instance. association is
        "GlobalParameter" (default), "LayerParameter" or "BlendParameter" — the last two
        address parameters inside a material-layer stack. Call material_update_instance
        afterwards to recompile. Returns {"ok": true}."""
        if not instance_path:
            raise ValueError("instance_path is required")
        if not parameter_name:
            raise ValueError("parameter_name is required")
        params: dict = {
            "instance_path": instance_path,
            "parameter_name": parameter_name,
            "value": value,
        }
        if association:
            params["association"] = association
        return conn.call("material.set_scalar_param", params)

    @mcp.tool()
    def material_set_vector_param(
        instance_path: str,
        parameter_name: str,
        value: list[float],
        association: str = "",
    ) -> dict:
        """Sets a vector (LinearColor) parameter override on a material instance. value is
        [r,g,b] or [r,g,b,a] (alpha defaults to 1). association as in
        material_set_scalar_param. Returns {"ok": true}."""
        if not instance_path:
            raise ValueError("instance_path is required")
        if not parameter_name:
            raise ValueError("parameter_name is required")
        if not value or len(value) < 3:
            raise ValueError("value must be [r,g,b] or [r,g,b,a]")
        params: dict = {
            "instance_path": instance_path,
            "parameter_name": parameter_name,
            "value": value,
        }
        if association:
            params["association"] = association
        return conn.call("material.set_vector_param", params)

    @mcp.tool()
    def material_set_texture_param(
        instance_path: str,
        parameter_name: str,
        texture_path: str,
        association: str = "",
    ) -> dict:
        """Sets a texture parameter override on a material instance. texture_path is the
        UTexture asset. association as in material_set_scalar_param. Returns {"ok": true}."""
        if not instance_path:
            raise ValueError("instance_path is required")
        if not parameter_name:
            raise ValueError("parameter_name is required")
        if not texture_path:
            raise ValueError("texture_path is required")
        params: dict = {
            "instance_path": instance_path,
            "parameter_name": parameter_name,
            "texture_path": texture_path,
        }
        if association:
            params["association"] = association
        return conn.call("material.set_texture_param", params)

    @mcp.tool()
    def material_set_static_switch_param(
        instance_path: str,
        parameter_name: str,
        value: bool,
        association: str = "",
    ) -> dict:
        """Sets a static switch parameter on a material instance. Static switches change which
        branch of the graph compiles, so this produces a new shader permutation — call
        material_update_instance afterwards. Returns {"ok": true}."""
        if not instance_path:
            raise ValueError("instance_path is required")
        if not parameter_name:
            raise ValueError("parameter_name is required")
        params: dict = {
            "instance_path": instance_path,
            "parameter_name": parameter_name,
            "value": value,
        }
        if association:
            params["association"] = association
        return conn.call("material.set_static_switch_param", params)

    @mcp.tool()
    def material_set_parameter_override(
        instance_path: str,
        parameter_name: str,
        override: bool,
        association: str = "",
    ) -> dict:
        """Turns a material instance's per-parameter override checkbox on or off. Setting it
        false makes the parameter inherit from the parent again without losing the stored
        value. Returns {"ok": true}."""
        if not instance_path:
            raise ValueError("instance_path is required")
        if not parameter_name:
            raise ValueError("parameter_name is required")
        params: dict = {
            "instance_path": instance_path,
            "parameter_name": parameter_name,
            "override": override,
        }
        if association:
            params["association"] = association
        return conn.call("material.set_parameter_override", params)

    @mcp.tool()
    def material_set_instance_parent(instance_path: str, parent_path: str) -> dict:
        """Reparents a material instance to another material or instance. Overrides whose
        parameter names still exist on the new parent are kept. Returns {"ok": true}."""
        if not instance_path:
            raise ValueError("instance_path is required")
        if not parent_path:
            raise ValueError("parent_path is required")
        return conn.call("material.set_instance_parent", {
            "instance_path": instance_path,
            "parent_path": parent_path,
        })

    @mcp.tool()
    def material_clear_instance_parameters(instance_path: str) -> dict:
        """Clears every parameter override on a material instance, returning it to the parent's
        values. Returns {"ok": true}."""
        if not instance_path:
            raise ValueError("instance_path is required")
        return conn.call("material.clear_instance_parameters", {"instance_path": instance_path})

    @mcp.tool()
    def material_update_instance(instance_path: str) -> dict:
        """Recompiles a material instance's shaders after parameter edits. Parameter writes
        only change stored values; without this the viewport keeps the old shader.
        Returns {"ok": true}."""
        if not instance_path:
            raise ValueError("instance_path is required")
        return conn.call("material.update_instance", {"instance_path": instance_path})

    # ----------------------------------------------------------- graph editing

    @mcp.tool()
    def material_add_expression(
        expression_class: str,
        material_path: str = "",
        function_path: str = "",
        pos_x: int = 0,
        pos_y: int = 0,
        asset_path: str = "",
    ) -> dict:
        """Adds an expression node to a material or material function graph. expression_class is
        a full script path ("/Script/Engine.MaterialExpressionAdd") or a bare class name
        ("MaterialExpressionAdd", assumed to be in /Script/Engine).

        asset_path optionally seeds the node from an asset — pass a texture to a
        MaterialExpressionTextureSample and it comes out already pointing at it, the same as
        dragging the texture into the graph. Returns {"guid": ...}, the handle every other
        graph command takes."""
        if not expression_class:
            raise ValueError("expression_class is required")
        params = _graph_host(material_path, function_path)
        params.update({
            "expression_class": expression_class,
            "pos_x": pos_x,
            "pos_y": pos_y,
        })
        if asset_path:
            params["asset_path"] = asset_path
        return conn.call("material.add_expression", params)

    @mcp.tool()
    def material_set_expression_property(
        expression_guid: str,
        property: str,
        value: str,
        material_path: str = "",
        function_path: str = "",
    ) -> dict:
        """Sets a property on an expression node by reflection — a Constant's "R", a
        ScalarParameter's "ParameterName" / "DefaultValue", a TextureSample's "SamplerType".
        value is Unreal import text: "0.5", "true", "MyParam", "(R=1,G=0,B=0,A=1)".
        Use material_list_expressions to find the guid. Returns {"ok": true}."""
        if not expression_guid:
            raise ValueError("expression_guid is required")
        if not property:
            raise ValueError("property is required")
        params = _graph_host(material_path, function_path)
        params.update({
            "expression_guid": expression_guid,
            "property": property,
            "value": value,
        })
        return conn.call("material.set_expression_property", params)

    @mcp.tool()
    def material_connect_expressions(
        from_guid: str,
        to_guid: str,
        from_output: str = "",
        to_input: str = "",
        material_path: str = "",
        function_path: str = "",
    ) -> dict:
        """Wires one expression's output pin to another's input pin. Empty pin names use the
        node's first pin, which is what most single-output nodes want; material_list_expressions
        reports the real pin names. Returns {"ok": true}."""
        if not from_guid:
            raise ValueError("from_guid is required")
        if not to_guid:
            raise ValueError("to_guid is required")
        params = _graph_host(material_path, function_path)
        params.update({
            "from_guid": from_guid,
            "to_guid": to_guid,
            "from_output": from_output,
            "to_input": to_input,
        })
        return conn.call("material.connect_expressions", params)

    @mcp.tool()
    def material_connect_property(
        material_path: str,
        from_guid: str,
        property: str,
        from_output: str = "",
    ) -> dict:
        """Wires an expression output into one of the material's own output pins. property is an
        EMaterialProperty name with or without the MP_ prefix: "BaseColor", "Metallic",
        "Roughness", "Normal", "EmissiveColor", "Opacity", "WorldPositionOffset", ...
        Material functions have no property inputs, so this takes material_path only.
        Returns {"ok": true}."""
        if not material_path:
            raise ValueError("material_path is required")
        if not from_guid:
            raise ValueError("from_guid is required")
        if not property:
            raise ValueError("property is required")
        return conn.call("material.connect_property", {
            "material_path": material_path,
            "from_guid": from_guid,
            "property": property,
            "from_output": from_output,
        })

    @mcp.tool()
    def material_disconnect_expression(
        to_guid: str,
        to_input: str,
        material_path: str = "",
        function_path: str = "",
    ) -> dict:
        """Clears one input pin on an expression node. Returns {"disconnected": bool} — false
        means the pin was already empty, which is not an error."""
        if not to_guid:
            raise ValueError("to_guid is required")
        if not to_input:
            raise ValueError("to_input is required")
        params = _graph_host(material_path, function_path)
        params.update({"to_guid": to_guid, "to_input": to_input})
        return conn.call("material.disconnect_expression", params)

    @mcp.tool()
    def material_disconnect_property(material_path: str, property: str) -> dict:
        """Clears one of the material's output pins (property name as in
        material_connect_property). Returns {"disconnected": bool}."""
        if not material_path:
            raise ValueError("material_path is required")
        if not property:
            raise ValueError("property is required")
        return conn.call("material.disconnect_property", {
            "material_path": material_path,
            "property": property,
        })

    @mcp.tool()
    def material_delete_expression(
        expression_guid: str,
        material_path: str = "",
        function_path: str = "",
    ) -> dict:
        """Deletes one expression node and the connections touching it. Returns {"ok": true}."""
        if not expression_guid:
            raise ValueError("expression_guid is required")
        params = _graph_host(material_path, function_path)
        params["expression_guid"] = expression_guid
        return conn.call("material.delete_expression", params)

    @mcp.tool()
    def material_delete_all_expressions(material_path: str = "", function_path: str = "") -> dict:
        """Deletes every expression node in the graph — the "start over" button. Comment boxes
        are a separate collection and survive. Returns {"ok": true}."""
        return conn.call("material.delete_all_expressions", _graph_host(material_path, function_path))

    @mcp.tool()
    def material_delete_unused_expressions(material_path: str) -> dict:
        """Deletes nodes that no material output can reach — the leftovers of an edited graph.
        Materials only (a function's nodes are reachable through its output node).
        Returns {"ok": true}."""
        if not material_path:
            raise ValueError("material_path is required")
        return conn.call("material.delete_unused_expressions", {"material_path": material_path})

    @mcp.tool()
    def material_layout_expressions(material_path: str = "", function_path: str = "") -> dict:
        """Runs the engine's own auto-arrange over the graph. Coarser than material_arrange_grid
        but needs no node list. Returns {"ok": true}."""
        return conn.call("material.layout_expressions", _graph_host(material_path, function_path))

    # ------------------------------------------------------- named reroutes

    @mcp.tool()
    def material_add_named_reroute_declaration(
        name: str,
        material_path: str = "",
        function_path: str = "",
        pos_x: int = 0,
        pos_y: int = 0,
        node_color: list[float] | None = None,
    ) -> dict:
        """Creates a Named Reroute Declaration — a graph-local named variable. Feed it by
        connecting an expression to its input (material_connect_expressions with an empty
        to_input), then read it anywhere in the graph with material_add_named_reroute_usage
        instead of dragging a wire across the whole sheet.

        node_color is optional [r,g,b] / [r,g,b,a]; material_add_channel_reroutes creates the
        project's standard PBR channel set with its fixed colors in one call.
        Returns {"guid","variable_guid","name"}."""
        if not name:
            raise ValueError("name is required")
        params = _graph_host(material_path, function_path)
        params.update({"name": name, "pos_x": pos_x, "pos_y": pos_y})
        if node_color:
            params["node_color"] = node_color
        return conn.call("material.add_named_reroute_declaration", params)

    @mcp.tool()
    def material_add_named_reroute_usage(
        declaration_guid: str,
        material_path: str = "",
        function_path: str = "",
        pos_x: int = 0,
        pos_y: int = 0,
    ) -> dict:
        """Creates a Named Reroute Usage reading a declaration, then wire its output onward with
        material_connect_expressions. declaration_guid is the declaration NODE's guid (the
        "guid" field, not "variable_guid"). Returns {"guid": ...}."""
        if not declaration_guid:
            raise ValueError("declaration_guid is required")
        params = _graph_host(material_path, function_path)
        params.update({
            "declaration_guid": declaration_guid,
            "pos_x": pos_x,
            "pos_y": pos_y,
        })
        return conn.call("material.add_named_reroute_usage", params)

    @mcp.tool()
    def material_set_named_reroute_name(
        expression_guid: str,
        name: str,
        material_path: str = "",
        function_path: str = "",
    ) -> dict:
        """Renames a Named Reroute Declaration. Its usages link by GUID, so they follow the new
        name automatically and no rewiring is needed. Returns {"ok": true}."""
        if not expression_guid:
            raise ValueError("expression_guid is required")
        if not name:
            raise ValueError("name is required")
        params = _graph_host(material_path, function_path)
        params.update({"expression_guid": expression_guid, "name": name})
        return conn.call("material.set_named_reroute_name", params)

    @mcp.tool()
    def material_list_named_reroutes(material_path: str = "", function_path: str = "") -> dict:
        """Lists the graph's Named Reroute declarations and usages with their linkage. Returns
        {"declarations":[{guid,name,variable_guid,node_color}],
        "usages":[{guid,declaration_name,declaration_guid,declaration_variable_guid}]} —
        an empty declaration_guid on a usage means its declaration is missing."""
        return conn.call("material.list_named_reroutes", _graph_host(material_path, function_path))

    # ------------------------------------------------------------- comments

    @mcp.tool()
    def material_add_comment(
        text: str,
        material_path: str = "",
        function_path: str = "",
        pos_x: int = 0,
        pos_y: int = 0,
        size_x: int = 400,
        size_y: int = 200,
        font_size: int = 18,
        color: list[float] | None = None,
    ) -> dict:
        """Adds a free-standing comment box at an explicit position and size. To box in nodes
        you already placed, use material_add_group_comment instead — it computes the bounds.
        color is optional [r,g,b] / [r,g,b,a]. Returns {"guid": ...}."""
        if not text:
            raise ValueError("text is required")
        params = _graph_host(material_path, function_path)
        params.update({
            "text": text,
            "pos_x": pos_x,
            "pos_y": pos_y,
            "size_x": size_x,
            "size_y": size_y,
            "font_size": font_size,
        })
        if color:
            params["color"] = color
        return conn.call("material.add_comment", params)

    @mcp.tool()
    def material_list_comments(material_path: str = "", function_path: str = "") -> dict:
        """Lists the graph's comment boxes. Comments live in their own collection, so they do
        not appear in material_list_expressions. Returns
        {"comments":[{guid,text,pos_x,pos_y,size_x,size_y,font_size,color}]}."""
        return conn.call("material.list_comments", _graph_host(material_path, function_path))

    @mcp.tool()
    def material_set_comment_text(
        comment_guid: str,
        text: str,
        material_path: str = "",
        function_path: str = "",
    ) -> dict:
        """Replaces a comment box's text. Assigned directly rather than through import text, so
        commas and newlines survive intact. Returns {"ok": true}."""
        if not comment_guid:
            raise ValueError("comment_guid is required")
        params = _graph_host(material_path, function_path)
        params.update({"comment_guid": comment_guid, "text": text})
        return conn.call("material.set_comment_text", params)

    @mcp.tool()
    def material_delete_comment(
        comment_guid: str,
        material_path: str = "",
        function_path: str = "",
    ) -> dict:
        """Removes a comment box. The nodes inside it are untouched. Returns {"ok": true}."""
        if not comment_guid:
            raise ValueError("comment_guid is required")
        params = _graph_host(material_path, function_path)
        params["comment_guid"] = comment_guid
        return conn.call("material.delete_comment", params)

    # --------------------------------------------------- base properties / build

    @mcp.tool()
    def material_set_property(object_path: str, property: str, value: str) -> dict:
        """Sets a property on the material / instance / function object itself rather than on a
        graph node — "BlendMode" ("BLEND_Translucent"), "ShadingModel"
        ("MSM_DefaultLit"), "TwoSided" ("true"), "bUsedWithSkeletalMesh". value is Unreal
        import text. Returns {"ok": true}."""
        if not object_path:
            raise ValueError("object_path is required")
        if not property:
            raise ValueError("property is required")
        return conn.call("material.set_property", {
            "object_path": object_path,
            "property": property,
            "value": value,
        })

    @mcp.tool()
    def material_recompile(material_path: str) -> dict:
        """Recompiles a material after graph edits and reports what the compiler said. Graph
        writes alone leave the old shader in place, so call this before judging a change.
        Returns {"compiled": bool, "errors": [...]}."""
        if not material_path:
            raise ValueError("material_path is required")
        return conn.call("material.recompile", {"material_path": material_path})

    @mcp.tool()
    def material_update_function(function_path: str) -> dict:
        """Propagates material function edits to every material that calls it, recompiling them.
        Without this, callers keep the pre-edit version. Returns {"ok": true}."""
        if not function_path:
            raise ValueError("function_path is required")
        return conn.call("material.update_function", {"function_path": function_path})

    # ------------------------------------------------------ placement / layout

    @mcp.tool()
    def material_set_node_position(
        expression_guid: str,
        pos_x: int,
        pos_y: int,
        material_path: str = "",
        function_path: str = "",
        snap: bool = True,
    ) -> dict:
        """Moves one expression node. snap rounds to the 16px graph grid (the editor's own
        alignment step) — leave it on unless you are matching an existing off-grid layout.
        Returns the position actually used: {"pos_x","pos_y"}."""
        if not expression_guid:
            raise ValueError("expression_guid is required")
        params = _graph_host(material_path, function_path)
        params.update({
            "expression_guid": expression_guid,
            "pos_x": pos_x,
            "pos_y": pos_y,
            "snap": snap,
        })
        return conn.call("material.set_node_position", params)

    @mcp.tool()
    def material_arrange_grid(
        columns: list[list[str]],
        material_path: str = "",
        function_path: str = "",
        origin_x: int = 0,
        origin_y: int = 0,
        col_step: int = 256,
        row_step: int = 80,
        snap: bool = True,
    ) -> dict:
        """Places many nodes at once on a left-to-right column / top-to-bottom row grid — the
        fast way to lay out a graph you just built. columns is a list of columns, each a list
        of expression guids: columns[i][j] lands at
        (origin_x + i*col_step, origin_y + j*row_step), snapped to 16px.

        Nothing is moved if any guid fails to resolve. Returns
        {"placed": n, "positions":[{guid,pos_x,pos_y}]}."""
        if not columns:
            raise ValueError("columns is required")
        params = _graph_host(material_path, function_path)
        params.update({
            "columns": columns,
            "origin_x": origin_x,
            "origin_y": origin_y,
            "col_step": col_step,
            "row_step": row_step,
            "snap": snap,
        })
        return conn.call("material.arrange_grid", params)

    @mcp.tool()
    def material_add_channel_reroutes(
        material_path: str = "",
        function_path: str = "",
        channels: list[str] | None = None,
        origin_x: int = 0,
        origin_y: int = 0,
        row_step: int = 80,
    ) -> dict:
        """Creates the project's standard PBR channel Named Reroute declarations in one call,
        stacked vertically with their fixed channel colors: AO, Diffuse, F0, Roughness, Normal,
        Emissive, UVs (the convention from /Game/02_Geo/MasterMaterial).

        Pass channels to create a subset or a different order; a name outside the standard set
        is still created, just black. Returns {"declarations":[{name,guid,variable_guid}]}."""
        params = _graph_host(material_path, function_path)
        params.update({"origin_x": origin_x, "origin_y": origin_y, "row_step": row_step})
        if channels:
            params["channels"] = channels
        return conn.call("material.add_channel_reroutes", params)

    @mcp.tool()
    def material_add_group_comment(
        text: str,
        node_guids: list[str],
        material_path: str = "",
        function_path: str = "",
        padding: int = 48,
        font_size: int = 18,
        color: list[float] | None = None,
    ) -> dict:
        """Draws a group box around a set of nodes, sized from their positions plus padding and
        styled with the project's group look (dark gray, font 18). Place the nodes first — the
        box is computed once and does not follow later moves.

        Node extents are approximated (200x128), since real node sizes only exist while the
        graph is open in an editor. Returns {"guid","pos_x","pos_y","size_x","size_y"}."""
        if not text:
            raise ValueError("text is required")
        if not node_guids:
            raise ValueError("node_guids (non-empty) is required")
        params = _graph_host(material_path, function_path)
        params.update({
            "text": text,
            "node_guids": node_guids,
            "padding": padding,
            "font_size": font_size,
        })
        if color:
            params["color"] = color
        return conn.call("material.add_group_comment", params)
