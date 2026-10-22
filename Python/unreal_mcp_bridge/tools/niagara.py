from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection

# Every module/input tool addresses one script stack. "stack" names the execution phase and
# doubles as the system/emitter distinction: SystemSpawn and SystemUpdate live on the system
# and take no emitter_name; the rest live on an emitter and require one.
_STACKS = "SystemSpawn, SystemUpdate, EmitterSpawn, EmitterUpdate, ParticleSpawn, ParticleUpdate, ParticleEvent"


def register_niagara_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    def _stack_params(system_path: str, stack: str, emitter_name: str) -> dict:
        if not system_path:
            raise ValueError("system_path is required")
        if not stack:
            raise ValueError(f"stack is required — one of {_STACKS}")
        params: dict = {"system_path": system_path, "stack": stack}
        if emitter_name:
            params["emitter_name"] = emitter_name
        return params

    @mcp.tool()
    def niagara_get_info(system_path: str) -> dict:
        """Lists a Niagara system's emitters and the script stacks on each, with a module count
        per stack. Start here to learn the emitter names that the other niagara_* tools need.
        Returns {"system", "system_stacks": [...], "emitters": [{"name", "id", "enabled",
        "stacks"}]}."""
        if not system_path:
            raise ValueError("system_path is required")
        return conn.call("niagara.get_info", {"system_path": system_path})

    @mcp.tool()
    def niagara_module_list(system_path: str, stack: str, emitter_name: str = "") -> dict:
        """Lists the modules on one script stack in execution order. Returns
        {"modules": [{"name", "display_name", "index", "enabled", "inherited",
        "can_move_and_delete", "script"}]}. "name" is what the other module tools match on.
        Modules with can_move_and_delete false are required by the stack or inherited from a
        parent emitter and cannot be removed."""
        return conn.call("niagara.module_list", _stack_params(system_path, stack, emitter_name))

    @mcp.tool()
    def niagara_module_add(
        system_path: str,
        stack: str,
        module_script_path: str,
        emitter_name: str = "",
        target_index: int = -1,
        suggested_name: str = "",
    ) -> dict:
        """Inserts a module script onto a script stack. module_script_path points at a
        UNiagaraScript whose usage is Module (e.g.
        "/Niagara/Modules/Update/Forces/Drag.Drag"). target_index -1 appends; any other value
        inserts at that position. Returns {"module_name", "index"} — pass module_name to the
        input tools. Call niagara_compile afterwards to rebuild the system."""
        if not module_script_path:
            raise ValueError("module_script_path is required")
        params = _stack_params(system_path, stack, emitter_name)
        params["module_script_path"] = module_script_path
        if target_index >= 0:
            params["target_index"] = target_index
        if suggested_name:
            params["suggested_name"] = suggested_name
        return conn.call("niagara.module_add", params)

    @mcp.tool()
    def niagara_module_remove(
        system_path: str, stack: str, module_name: str, emitter_name: str = ""
    ) -> dict:
        """Deletes a module from a script stack. Fails cleanly if the module is required by the
        stack or inherited from a parent emitter — check can_move_and_delete in
        niagara_module_list first. To silence a module without losing its settings use
        niagara_module_set_enabled instead."""
        if not module_name:
            raise ValueError("module_name is required")
        params = _stack_params(system_path, stack, emitter_name)
        params["module_name"] = module_name
        return conn.call("niagara.module_remove", params)

    @mcp.tool()
    def niagara_module_set_enabled(
        system_path: str, stack: str, module_name: str, enabled: bool, emitter_name: str = ""
    ) -> dict:
        """Enables or disables a module in place, keeping its input values. This is the
        reversible alternative to niagara_module_remove."""
        if not module_name:
            raise ValueError("module_name is required")
        params = _stack_params(system_path, stack, emitter_name)
        params["module_name"] = module_name
        params["enabled"] = enabled
        return conn.call("niagara.module_set_enabled", params)

    @mcp.tool()
    def niagara_input_list(
        system_path: str, stack: str, module_name: str, emitter_name: str = ""
    ) -> dict:
        """Lists a module's inputs with their current values. Returns
        {"inputs": [{"name", "handle", "type", "value_mode", "value", "can_reset"}]}.
        value_mode tells you how the input is currently driven — Local (a constant), Linked
        (another parameter), Dynamic (a dynamic input script), Data (a data interface), or a
        default. "type" is the Niagara type name (NiagaraFloat, NiagaraBool, NiagaraInt32,
        Vector, Position, ...). For Local inputs "value" comes back in the same form
        input_set_local accepts, so it can be written straight back. Use "name" with the
        input_set_* tools."""
        if not module_name:
            raise ValueError("module_name is required")
        params = _stack_params(system_path, stack, emitter_name)
        params["module_name"] = module_name
        return conn.call("niagara.input_list", params)

    @mcp.tool()
    def niagara_input_set_local(
        system_path: str,
        stack: str,
        module_name: str,
        input_name: str,
        value: str,
        emitter_name: str = "",
    ) -> dict:
        """Sets a module input to a constant. value accepts a bare scalar ("8", "true"/"false"
        for a bool), a comma-separated list matching the input's field order ("1,0,0.5" for a
        vector, R,G,B,A for a color), or Unreal import text when the type needs it
        ("(X=1,Y=2,Z=0)"). Check the input's "type" via niagara_input_list first — the value is
        parsed against it."""
        if not module_name:
            raise ValueError("module_name is required")
        if not input_name:
            raise ValueError("input_name is required")
        if value == "":
            raise ValueError("value is required")
        params = _stack_params(system_path, stack, emitter_name)
        params["module_name"] = module_name
        params["input_name"] = input_name
        params["value"] = value
        return conn.call("niagara.input_set_local", params)

    @mcp.tool()
    def niagara_input_set_linked(
        system_path: str,
        stack: str,
        module_name: str,
        input_name: str,
        parameter: str,
        emitter_name: str = "",
    ) -> dict:
        """Drives a module input from another parameter, e.g. parameter="Particles.Velocity" or
        a user-exposed "User.SpawnRate". The parameter must have the same type as the input."""
        if not module_name:
            raise ValueError("module_name is required")
        if not input_name:
            raise ValueError("input_name is required")
        if not parameter:
            raise ValueError("parameter is required")
        params = _stack_params(system_path, stack, emitter_name)
        params["module_name"] = module_name
        params["input_name"] = input_name
        params["parameter"] = parameter
        return conn.call("niagara.input_set_linked", params)

    @mcp.tool()
    def niagara_input_set_dynamic(
        system_path: str,
        stack: str,
        module_name: str,
        input_name: str,
        dynamic_input_script_path: str,
        emitter_name: str = "",
        suggested_name: str = "",
    ) -> dict:
        """Drives a module input with a dynamic input script (a UNiagaraScript whose usage is
        DynamicInput), e.g. a Random Range Float feeding a spawn rate. The dynamic input's own
        inputs then appear in niagara_input_list under the module."""
        if not module_name:
            raise ValueError("module_name is required")
        if not input_name:
            raise ValueError("input_name is required")
        if not dynamic_input_script_path:
            raise ValueError("dynamic_input_script_path is required")
        params = _stack_params(system_path, stack, emitter_name)
        params["module_name"] = module_name
        params["input_name"] = input_name
        params["dynamic_input_script_path"] = dynamic_input_script_path
        if suggested_name:
            params["suggested_name"] = suggested_name
        return conn.call("niagara.input_set_dynamic", params)

    @mcp.tool()
    def niagara_input_reset(
        system_path: str,
        stack: str,
        module_name: str,
        input_name: str,
        emitter_name: str = "",
    ) -> dict:
        """Drops a module input's override and restores its default. Fails if the input is
        already at its default — can_reset in niagara_input_list tells you which are set."""
        if not module_name:
            raise ValueError("module_name is required")
        if not input_name:
            raise ValueError("input_name is required")
        params = _stack_params(system_path, stack, emitter_name)
        params["module_name"] = module_name
        params["input_name"] = input_name
        return conn.call("niagara.input_reset", params)

    @mcp.tool()
    def niagara_renderer_list(
        system_path: str, emitter_name: str, include_properties: bool = False
    ) -> dict:
        """Lists an emitter's renderers in draw order. Returns
        {"renderers": [{"index", "class", "display_name", "enabled", "inherited"}]}. "index" is
        what the other renderer tools address — renderers have no stable name of their own, so
        call this again after adding or removing one. include_properties adds a "properties"
        map of every editable property to its current value, which is how you discover what a
        renderer exposes and confirm a niagara_renderer_set_property write landed; it is off by
        default because a sprite renderer alone has dozens."""
        if not system_path:
            raise ValueError("system_path is required")
        if not emitter_name:
            raise ValueError("emitter_name is required")
        return conn.call("niagara.renderer_list", {
            "system_path": system_path,
            "emitter_name": emitter_name,
            "include_properties": include_properties,
        })

    @mcp.tool()
    def niagara_renderer_set_property(
        system_path: str, emitter_name: str, renderer_index: int, property: str, value: str
    ) -> dict:
        """Sets a property on a renderer via reflection. value is Unreal import text, matching
        the property's type — "true", "2.5", "/Game/M_Spark.M_Spark" for an object reference,
        "(X=1,Y=1,Z=1)" for a struct. Use niagara_renderer_list to find renderer_index and the
        renderer's class, e.g. NiagaraSpriteRendererProperties (SubImageSize, Material,
        Alignment) or NiagaraMeshRendererProperties (Meshes, bOverrideMaterials)."""
        if not system_path:
            raise ValueError("system_path is required")
        if not emitter_name:
            raise ValueError("emitter_name is required")
        if not property:
            raise ValueError("property is required")
        return conn.call("niagara.renderer_set_property", {
            "system_path": system_path,
            "emitter_name": emitter_name,
            "renderer_index": renderer_index,
            "property": property,
            "value": value,
        })

    @mcp.tool()
    def niagara_renderer_set_enabled(
        system_path: str, emitter_name: str, renderer_index: int, enabled: bool
    ) -> dict:
        """Enables or disables one renderer on an emitter, keeping its settings."""
        if not system_path:
            raise ValueError("system_path is required")
        if not emitter_name:
            raise ValueError("emitter_name is required")
        return conn.call("niagara.renderer_set_enabled", {
            "system_path": system_path,
            "emitter_name": emitter_name,
            "renderer_index": renderer_index,
            "enabled": enabled,
        })

    @mcp.tool()
    def niagara_emitter_add(
        system_path: str, emitter_asset_path: str, emitter_name: str = ""
    ) -> dict:
        """Adds an emitter asset to a system. emitter_asset_path points at a UNiagaraEmitter;
        its exposed version is used. emitter_name renames the new handle — leave it empty to
        keep the asset's name. Returns {"name", "id"}. Call niagara_compile afterwards."""
        if not system_path:
            raise ValueError("system_path is required")
        if not emitter_asset_path:
            raise ValueError("emitter_asset_path is required")
        params: dict = {"system_path": system_path, "emitter_asset_path": emitter_asset_path}
        if emitter_name:
            params["emitter_name"] = emitter_name
        return conn.call("niagara.emitter_add", params)

    @mcp.tool()
    def niagara_emitter_remove(system_path: str, emitter_name: str) -> dict:
        """Removes an emitter from a system, along with its whole module stack. Use
        niagara_get_info for the emitter names."""
        if not system_path:
            raise ValueError("system_path is required")
        if not emitter_name:
            raise ValueError("emitter_name is required")
        return conn.call("niagara.emitter_remove", {
            "system_path": system_path,
            "emitter_name": emitter_name,
        })

    @mcp.tool()
    def niagara_compile(system_path: str, wait: bool = True) -> dict:
        """Recompiles a Niagara system so stack edits take effect. wait blocks until the CPU
        scripts finish (GPU shaders keep compiling in the background). Call this after a batch
        of module/input edits, then asset_save to persist."""
        if not system_path:
            raise ValueError("system_path is required")
        return conn.call("niagara.compile", {"system_path": system_path, "wait": wait})
