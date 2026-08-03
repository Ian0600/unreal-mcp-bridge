from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection

# The seven script stacks a Niagara system exposes. SystemSpawn/SystemUpdate are system-level
# and take no emitter_name; the rest live on an emitter and require one.
STACKS = (
    "SystemSpawn",
    "SystemUpdate",
    "EmitterSpawn",
    "EmitterUpdate",
    "ParticleSpawn",
    "ParticleUpdate",
    "ParticleEvent",
)


def _stack_params(system_path: str, stack: str, emitter_name: str) -> dict:
    """Builds the (system, stack, emitter) triple every stack command takes.

    emitter_name is omitted rather than sent empty: the C++ side distinguishes the two, and
    rejects a system-level stack that carries one."""
    if not system_path:
        raise ValueError("system_path is required")
    if not stack:
        raise ValueError(f"stack is required; one of {', '.join(STACKS)}")
    params: dict = {"system_path": system_path, "stack": stack}
    if emitter_name:
        params["emitter_name"] = emitter_name
    return params


def register_niagara_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    # ------------------------------------------------------------- inspection

    @mcp.tool()
    def niagara_get_info(system_path: str) -> dict:
        """Overview of a Niagara system: its emitters and which script stacks each one has.
        Start here — the emitter names it returns are what every other niagara tool takes, and
        module_count tells you which stacks are worth listing. Returns
        {"system", "system_stacks":[{stack,module_count}],
        "emitters":[{name,id,enabled,stacks:[{stack,module_count}]}]}."""
        if not system_path:
            raise ValueError("system_path is required")
        return conn.call("niagara.get_info", {"system_path": system_path})

    @mcp.tool()
    def niagara_module_list(system_path: str, stack: str, emitter_name: str = "") -> dict:
        """Lists one script stack's modules in execution order — module order is the program, so
        read this before inserting anything. stack is one of SystemSpawn, SystemUpdate,
        EmitterSpawn, EmitterUpdate, ParticleSpawn, ParticleUpdate, ParticleEvent;
        emitter_name is required for all but the two System stacks.

        "name" is what the other tools match on. can_move_and_delete false means the module is
        required by the stack or inherited from a parent emitter. Returns
        {"modules":[{name,display_name,index,enabled,inherited,can_move_and_delete,script}]}."""
        return conn.call("niagara.module_list", _stack_params(system_path, stack, emitter_name))

    @mcp.tool()
    def niagara_input_list(
        system_path: str,
        stack: str,
        module_name: str,
        emitter_name: str = "",
    ) -> dict:
        """Lists a module's inputs with their current values. value_mode says which of the
        input_set_* tools applies: Local (a constant), Linked (reads another parameter),
        Dynamic (driven by a dynamic input script), Data / ObjectAsset. "value" comes back in
        the same form niagara_input_set_local accepts, so it can be written straight back.
        can_reset false means the input is already at its default. Returns
        {"inputs":[{name,handle,type,value_mode,value,can_reset}]}."""
        if not module_name:
            raise ValueError("module_name is required")
        params = _stack_params(system_path, stack, emitter_name)
        params["module_name"] = module_name
        return conn.call("niagara.input_list", params)

    # ---------------------------------------------------------------- modules

    @mcp.tool()
    def niagara_module_add(
        system_path: str,
        stack: str,
        module_script_path: str,
        emitter_name: str = "",
        target_index: int = -1,
        suggested_name: str = "",
    ) -> dict:
        """Inserts a module script onto a script stack. module_script_path must be a module
        script (e.g. "/Niagara/Modules/Update/Forces/Drag.Drag") — dynamic input scripts go
        through niagara_input_set_dynamic instead, and are rejected here.

        target_index is the 0-based slot in execution order; -1 appends. Modules run in order,
        so a force added after the solver has no effect that frame — check niagara_module_list
        first. Call niagara_compile when done. Returns {"module_name", "index"}."""
        if not module_script_path:
            raise ValueError("module_script_path is required")
        params = _stack_params(system_path, stack, emitter_name)
        params["module_script_path"] = module_script_path
        params["target_index"] = target_index
        if suggested_name:
            params["suggested_name"] = suggested_name
        return conn.call("niagara.module_add", params)

    @mcp.tool()
    def niagara_module_remove(
        system_path: str,
        stack: str,
        module_name: str,
        emitter_name: str = "",
    ) -> dict:
        """Deletes a module from a script stack. Modules the stack requires or that are
        inherited from a parent emitter cannot be deleted — disable them with
        niagara_module_set_enabled instead. Returns {"ok": true}."""
        if not module_name:
            raise ValueError("module_name is required")
        params = _stack_params(system_path, stack, emitter_name)
        params["module_name"] = module_name
        return conn.call("niagara.module_remove", params)

    @mcp.tool()
    def niagara_module_set_enabled(
        system_path: str,
        stack: str,
        module_name: str,
        enabled: bool,
        emitter_name: str = "",
    ) -> dict:
        """Enables or disables a module without removing it — the way to switch off an inherited
        or required module, and the safe way to bisect an effect. Returns {"ok": true}."""
        if not module_name:
            raise ValueError("module_name is required")
        params = _stack_params(system_path, stack, emitter_name)
        params.update({"module_name": module_name, "enabled": enabled})
        return conn.call("niagara.module_set_enabled", params)

    # ----------------------------------------------------------------- inputs

    @mcp.tool()
    def niagara_input_set_local(
        system_path: str,
        stack: str,
        module_name: str,
        input_name: str,
        value: str,
        emitter_name: str = "",
    ) -> dict:
        """Sets a module input to a constant. value takes either a bare scalar or
        comma-separated list — "8", "true", "1,0,0.5" — or Unreal import text starting with
        '(' — "(X=1,Y=2,Z=0)" — which is the form to use for enums and nested structs.
        Returns {"ok": true}."""
        if not module_name:
            raise ValueError("module_name is required")
        if not input_name:
            raise ValueError("input_name is required")
        params = _stack_params(system_path, stack, emitter_name)
        params.update({"module_name": module_name, "input_name": input_name, "value": value})
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
        """Points a module input at another parameter instead of a constant — e.g.
        "Particles.Age", "Emitter.LoopedAge", "User.Intensity". The link is created with the
        input's own type, so the parameter must carry that type or the stack rejects it.
        Returns {"ok": true}."""
        if not module_name:
            raise ValueError("module_name is required")
        if not input_name:
            raise ValueError("input_name is required")
        if not parameter:
            raise ValueError("parameter is required")
        params = _stack_params(system_path, stack, emitter_name)
        params.update({
            "module_name": module_name,
            "input_name": input_name,
            "parameter": parameter,
        })
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
        """Drives a module input with a dynamic input script — a curve over particle age, a
        random range, a float-from-vector. dynamic_input_script_path must be a dynamic input
        script (module scripts are rejected; those go through niagara_module_add). The script's
        own inputs then show up in niagara_input_list under this module. Returns {"ok": true}."""
        if not module_name:
            raise ValueError("module_name is required")
        if not input_name:
            raise ValueError("input_name is required")
        if not dynamic_input_script_path:
            raise ValueError("dynamic_input_script_path is required")
        params = _stack_params(system_path, stack, emitter_name)
        params.update({
            "module_name": module_name,
            "input_name": input_name,
            "dynamic_input_script_path": dynamic_input_script_path,
        })
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
        """Drops a module input's override and returns it to the script's default. Errors if the
        input is already at its default (can_reset in niagara_input_list).
        Returns {"ok": true}."""
        if not module_name:
            raise ValueError("module_name is required")
        if not input_name:
            raise ValueError("input_name is required")
        params = _stack_params(system_path, stack, emitter_name)
        params.update({"module_name": module_name, "input_name": input_name})
        return conn.call("niagara.input_reset", params)

    # -------------------------------------------------------------- renderers

    @mcp.tool()
    def niagara_renderer_list(
        system_path: str,
        emitter_name: str,
        include_properties: bool = False,
    ) -> dict:
        """Lists an emitter's renderers in draw order. Renderers have no stable name, so the
        index this returns is how niagara_renderer_set_property / _set_enabled address them.

        include_properties dumps every editable property with its current value — off by
        default because one sprite renderer carries dozens, but it is the only way to see what
        a renderer exposes or to confirm a write landed. Returns
        {"renderers":[{index,class,display_name,enabled,inherited,properties?}]}."""
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
        system_path: str,
        emitter_name: str,
        renderer_index: int,
        property: str,
        value: str,
    ) -> dict:
        """Sets a property on a renderer by reflection — "Material"
        ("/Game/FX/M_Smoke.M_Smoke"), "SortMode" ("ENiagaraSortMode::ViewDepth"), "Alignment",
        "SubImageSize" ("(X=4,Y=4)"). value is Unreal import text; run niagara_renderer_list
        with include_properties to see the exact names and current values.
        Returns {"ok": true}."""
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
        system_path: str,
        emitter_name: str,
        renderer_index: int,
        enabled: bool,
    ) -> dict:
        """Enables or disables one renderer on an emitter, by its niagara_renderer_list index.
        Returns {"ok": true}."""
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

    # --------------------------------------------------------------- emitters

    @mcp.tool()
    def niagara_emitter_add(
        system_path: str,
        emitter_asset_path: str,
        emitter_name: str = "",
    ) -> dict:
        """Adds an emitter asset to a system. emitter_name optionally renames the new handle;
        without it the emitter keeps the asset's name, which collides awkwardly when the same
        asset is added twice. Returns {"name","id"} — use "name" for the other tools."""
        if not system_path:
            raise ValueError("system_path is required")
        if not emitter_asset_path:
            raise ValueError("emitter_asset_path is required")
        params: dict = {
            "system_path": system_path,
            "emitter_asset_path": emitter_asset_path,
        }
        if emitter_name:
            params["emitter_name"] = emitter_name
        return conn.call("niagara.emitter_add", params)

    @mcp.tool()
    def niagara_emitter_remove(system_path: str, emitter_name: str) -> dict:
        """Removes an emitter from a system, with its stacks and renderers.
        Returns {"ok": true}."""
        if not system_path:
            raise ValueError("system_path is required")
        if not emitter_name:
            raise ValueError("emitter_name is required")
        return conn.call("niagara.emitter_remove", {
            "system_path": system_path,
            "emitter_name": emitter_name,
        })

    # ---------------------------------------------------------------- compile

    @mcp.tool()
    def niagara_compile(system_path: str, wait: bool = True) -> dict:
        """Recompiles a system after stack edits — nothing you changed runs until this does.
        wait blocks until the CPU scripts finish, which is what you want before reading results
        back or taking a screenshot; pass false to fire and forget. Call asset_save afterwards
        to persist. Returns {"requested": bool, "waited": bool}."""
        if not system_path:
            raise ValueError("system_path is required")
        return conn.call("niagara.compile", {"system_path": system_path, "wait": wait})
