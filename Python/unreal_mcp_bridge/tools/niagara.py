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

    @mcp.tool()
    def niagara_inspect_function_calls(system_asset_path: str, emitter_name: str) -> dict:
        """지정 이미터 GraphSource의 FunctionCall 이름, 스크립트 경로, 노출 입력을 읽기 전용으로 반환한다.

        Forwarded to BlueprintInspector (BlueprintInspectorLibrary::InspectNiagaraFunctionCalls); requires that plugin to be loaded."""
        return conn.call("niagara.inspect_function_calls", {"system_asset_path": system_asset_path, "emitter_name": emitter_name})

    @mcp.tool()
    def niagara_inspect_module_graph(module_script_path: str) -> dict:
        """InspectNiagaraModuleGraph

        Forwarded to BlueprintInspector (BlueprintInspectorLibrary::InspectNiagaraModuleGraph); requires that plugin to be loaded."""
        return conn.call("niagara.inspect_module_graph", {"module_script_path": module_script_path})

    @mcp.tool()
    def niagara_script_create(asset_path: str, usage: str) -> dict:
        """Niagara script graph authoring that the official Niagara toolsets lack: create module / dynamic input / function
        script assets, edit script metadata, module inputs, parameter map reads/writes, Op / function call / Custom HLSL nodes,
        pin links and defaults, compile standalone scripts, and create scratch pad scripts owned by a system or emitter.
        Stack-level work (adding the module to an emitter, setting stack inputs, compile state, stack issues) stays with the
        official NiagaraToolset_System (AddModule also accepts a scratch pad script object path, see CreateScratchPadScript).
        
        Paths and ids:
         - ScriptPath = asset path ("/Game/FX/NM_MyModule") or full object path. Scratch pad scripts are sub-objects:
           "/Game/FX/NS_Fire.NS_Fire:MyScratch" (system) or "/Game/FX/NE_Fire.NE_Fire:<container>.MyScratch" (emitter asset);
           CreateScratchPadScript and DumpScriptGraph return the exact path to use.
         - NodeId = UEdGraphNode::NodeGuid (32 hex digits, any FGuid text form accepted). See DumpScriptGraph.
         - Pin = pin id (UEdGraphPin::PinId or PersistentGuid) or pin name; prefix "in:" / "out:" to force the direction,
           e.g. "out:Particles.Color". Names are matched exactly (FName, case-insensitive).
         - Type = float, int, bool, vec2, vec3, vec4, color, quat, position, matrix, map (parameter map), a registered enum or
           struct path ("/Script/Niagara.ENiagaraExecutionState", "/Script/Niagara.NiagaraID", "/Game/Enums/E_MyEnum"),
           or a data interface class path ("/Script/Niagara.NiagaraDataInterfaceCurve").
         - Values use the Niagara pin default format of the type: float "0.5", int "3", bool "true", vec3 "1,2,3",
           color "(R=1,G=0,B=0,A=1)", quat "0,0,0,1", enum entry name.
        Edits are transacted, mark the graph as needing recompile and dirty the package like the script editor. Nothing is saved
        (save with the official asset tools). Edit the script while its asset editor / the owning system editor is closed:
        open editors work on a copy and will not see (or will overwrite) these changes.
        /
        UCLASS(BlueprintType)
        class BLUEPRINTINSPECTOR_API UBPINiagaraScriptToolset : public UToolsetDefinition
        {
        	GENERATED_BODY()
        
        public:
        	virtual FString GetToolsetVersion() const override { return TEXT("1.0"); }
        
        	// ---- scripts ----
        
        	/**
        Create a Niagara script asset exactly like the content browser (UNiagaraModule/Function/DynamicInputScriptFactory):
        a copy of the project's default module/function/dynamic input script, or the procedural default graph
        (input map -> Map Get -> Map Set -> output) when that default is missing.
        asset_path: Package path of the new asset, e.g. "/Game/FX/NM_Tint". Fails if it exists.
        usage: Module, DynamicInput or Function.
        Returns: JSON {"path","usage","nodes":n,"compileStatus"} or "ERROR|...". Not compiled unless the factory compiled it.

        Forwarded to BlueprintInspector (BPINiagaraScriptToolset::CreateNiagaraScript); requires that plugin to be loaded."""
        return conn.call("niagara.script_create", {"asset_path": asset_path, "usage": usage})

    @mcp.tool()
    def niagara_script_set_metadata(script_path: str, description: str, category: str, library_visibility: str, module_usage_bitmask: str, keywords: str) -> dict:
        """Set script metadata on the latest script version (FVersionedNiagaraScriptData). Empty arguments are left unchanged;
        "<none>" clears a text field.
        script_path: Script asset or scratch pad object path.
        description: Description shown in the module picker and tooltips.
        category: Library category, e.g. "Color".
        library_visibility: Exposed (alias Library), Unexposed or Hidden.
        module_usage_bitmask: Usages the module may be added to: an integer bitmask, or names separated by ',' or '|',
         e.g. "ParticleSpawn,ParticleUpdate" (the "Script" suffix is optional). "System" / "Emitter" / "Particle" add the
         spawn+update pair of that level.
        keywords: Space separated search keywords.
        Returns: JSON with the resulting metadata (same shape as DumpScriptGraph "metadata") or "ERROR|...".

        Forwarded to BlueprintInspector (BPINiagaraScriptToolset::SetScriptMetadata); requires that plugin to be loaded."""
        return conn.call("niagara.script_set_metadata", {"script_path": script_path, "description": description, "category": category, "library_visibility": library_visibility, "module_usage_bitmask": module_usage_bitmask, "keywords": keywords})

    @mcp.tool()
    def niagara_script_compile(script_path: str, force: bool, refresh_referencers: bool) -> dict:
        """Compile a standalone script (module / function / dynamic input, asset or scratch pad) synchronously, like the
        script editor's Compile button (UNiagaraScript::RequestCompile waits for the compile job).
        script_path: Script asset or scratch pad object path.
        force: Recompile even if the script and graph are in sync.
        refresh_referencers: Refresh every loaded function call node that calls this script (system/emitter stacks,
         other scripts) and mark them for recompile, like Apply in the script / scratch pad editor. Needed after editing a
         scratch pad script before the owning system compiles. Dirties the referencing packages.
        Returns: JSON {"status","synchronized","errors","warnings","errorMessage","events":[{"severity","message","shortDescription","nodeId","pinId"}],
         "refreshedReferencers":[node paths]} or "ERROR|...". "status" is the ENiagaraScriptCompileStatus name (NCS_UpToDate on success).

        Forwarded to BlueprintInspector (BPINiagaraScriptToolset::CompileNiagaraScript); requires that plugin to be loaded."""
        return conn.call("niagara.script_compile", {"script_path": script_path, "force": force, "refresh_referencers": refresh_referencers})

    @mcp.tool()
    def niagara_script_dump_graph(script_path: str) -> dict:
        """Dump the script graph with ids usable by these tools.
        Returns: JSON {"path","usage","isScratchPad","owner","versionGuid",
         "metadata":{"description","category","keywords","libraryVisibility","moduleUsageBitmask","moduleUsages":[...],"suggested","deprecated","experimental"},
         "compile":{"status","synchronized","errorMessage","events":[...]},
         "inputs":[{"name","type","defaultMode","default","description","pinIds":[...]}] (module inputs = Module.* reads),
         "writes":[{"name","type","nodeId","pinId"}] (parameter map writes = Map Set inputs),
         "nodes":[{"id","class","title","x","y","op"?,"functionScript"?,"functionName"?,"hlsl"?,"input"?,"outputUsage"?,"outputs"?,
           "pins":[{"id","name","dir","type","default","add"?,"hidden"?,"orphaned"?,"links":[{"node","pin","pinName"}]}]}]} or "ERROR|...".

        Forwarded to BlueprintInspector (BPINiagaraScriptToolset::DumpScriptGraph); requires that plugin to be loaded."""
        return conn.call("niagara.script_dump_graph", {"script_path": script_path})

    @mcp.tool()
    def niagara_script_add_input(script_path: str, name: str, type_: str, default_value: str, map_get_node_id: str) -> dict:
        """Add a module input: a "Module.<Name>" read on the script's Map Get node, registered as a graph parameter
        (like adding it in the Parameters panel), with an optional default value (default mode Value).
        name: Input name; "Module." is prepended when no namespace is given. Must be in the Module namespace.
        type: See class comment (float, vec3, color, ...).
        default_value: Default in Niagara pin format (empty = type default).
        map_get_node_id: Map Get node to add the read to. Empty = the Map Get fed by the input map (first one).
        Returns: JSON {"name","type","default","nodeId","pinId"} or "ERROR|...".

        Forwarded to BlueprintInspector (BPINiagaraScriptToolset::AddScriptInput); requires that plugin to be loaded."""
        return conn.call("niagara.script_add_input", {"script_path": script_path, "name": name, "type": type_, "default_value": default_value, "map_get_node_id": map_get_node_id})

    @mcp.tool()
    def niagara_script_remove_input(script_path: str, name: str) -> dict:
        """Remove a module input: removes its reads from every Map Get node (links are broken) and its graph parameter.
        name: Input name, "Module." optional.
        Returns: JSON {"removedPins":n,"removedParameter":bool} or "ERROR|...".

        Forwarded to BlueprintInspector (BPINiagaraScriptToolset::RemoveScriptInput); requires that plugin to be loaded."""
        return conn.call("niagara.script_remove_input", {"script_path": script_path, "name": name})

    @mcp.tool()
    def niagara_script_set_input_default(script_path: str, name: str, default_value: str) -> dict:
        """Set the default value of an existing module input (default mode becomes Value).
        name: Input name, "Module." optional.
        default_value: Value in Niagara pin format.
        Returns: JSON {"name","type","default"} (value read back) or "ERROR|...".

        Forwarded to BlueprintInspector (BPINiagaraScriptToolset::SetScriptInputDefault); requires that plugin to be loaded."""
        return conn.call("niagara.script_set_input_default", {"script_path": script_path, "name": name, "default_value": default_value})

    @mcp.tool()
    def niagara_script_add_map_get_pin(script_path: str, node_id: str, parameter_name: str, type_: str) -> dict:
        """Add a parameter read pin (output) to a Map Get node, e.g. an attribute "Particles.Color" or "Engine.DeltaTime".
        node_id: Map Get node id. Empty = the first Map Get node.
        parameter_name: Full name including namespace, e.g. "Particles.Color".
        type: Parameter type (must match the type the parameter already has in this graph).
        Returns: JSON {"nodeId","pinId","name","type"} or "ERROR|...".

        Forwarded to BlueprintInspector (BPINiagaraScriptToolset::AddMapGetPin); requires that plugin to be loaded."""
        return conn.call("niagara.script_add_map_get_pin", {"script_path": script_path, "node_id": node_id, "parameter_name": parameter_name, "type": type_})

    @mcp.tool()
    def niagara_script_add_map_set_pin(script_path: str, node_id: str, parameter_name: str, type_: str) -> dict:
        """Add a parameter write pin (input) to a Map Set node, e.g. "Particles.Color".
        node_id: Map Set node id. Empty = the first Map Set node.
        parameter_name: Full name including namespace.
        type: Parameter type.
        Returns: JSON {"nodeId","pinId","name","type"} or "ERROR|...".

        Forwarded to BlueprintInspector (BPINiagaraScriptToolset::AddMapSetPin); requires that plugin to be loaded."""
        return conn.call("niagara.script_add_map_set_pin", {"script_path": script_path, "node_id": node_id, "parameter_name": parameter_name, "type": type_})

    @mcp.tool()
    def niagara_script_add_node(script_path: str, node_kind: str, arg: str, pos_x: int, pos_y: int) -> dict:
        """Add a node to the script graph (same steps as the graph context menu action).
        node_kind: Op, FunctionCall, CustomHlsl, ParameterMapGet, ParameterMapSet or Reroute.
        arg: Op: op name, e.g. "Numeric::Add", "Numeric::Mul", "Numeric::Normalize", "Numeric::Lerp", "Vector3::Cross".
         FunctionCall: function or dynamic input script path. CustomHlsl: HLSL body (empty = editor placeholder). Others: ignored.
        pos_x: Graph X position.
        pos_y: Graph Y position.
        Returns: JSON node (same shape as DumpScriptGraph "nodes" entries) or "ERROR|...".

        Forwarded to BlueprintInspector (BPINiagaraScriptToolset::AddNode); requires that plugin to be loaded."""
        return conn.call("niagara.script_add_node", {"script_path": script_path, "node_kind": node_kind, "arg": arg, "pos_x": pos_x, "pos_y": pos_y})

    @mcp.tool()
    def niagara_script_remove_node(script_path: str, node_id: str) -> dict:
        """Remove a node (breaks its links). Nodes the editor does not allow deleting (e.g. the output node) are refused.
        Returns: "OK" or "ERROR|...".

        Forwarded to BlueprintInspector (BPINiagaraScriptToolset::RemoveNode); requires that plugin to be loaded."""
        return conn.call("niagara.script_remove_node", {"script_path": script_path, "node_id": node_id})

    @mcp.tool()
    def niagara_script_set_custom_hlsl(script_path: str, node_id: str, code: str) -> dict:
        """Replace the HLSL body of a Custom HLSL node. Inputs and outputs are referenced by pin name in the code,
        e.g. "OutColor = InColor * Tint;".
        Returns: JSON node or "ERROR|...".

        Forwarded to BlueprintInspector (BPINiagaraScriptToolset::SetCustomHlsl); requires that plugin to be loaded."""
        return conn.call("niagara.script_set_custom_hlsl", {"script_path": script_path, "node_id": node_id, "code": code})

    @mcp.tool()
    def niagara_script_add_custom_hlsl_pin(script_path: str, node_id: str, name: str, type_: str, is_output: bool) -> dict:
        """Add a typed input or output pin to a Custom HLSL node (updates the node signature and rebuilds its pins; links are kept).
        name: HLSL identifier ([A-Za-z_][A-Za-z0-9_]*), unique on the node.
        type: See class comment. "map" adds a parameter map pin (to read/write the map inside the HLSL).
        is_output: True for an output pin.
        Returns: JSON node or "ERROR|...".

        Forwarded to BlueprintInspector (BPINiagaraScriptToolset::AddCustomHlslPin); requires that plugin to be loaded."""
        return conn.call("niagara.script_add_custom_hlsl_pin", {"script_path": script_path, "node_id": node_id, "name": name, "type": type_, "is_output": is_output})

    @mcp.tool()
    def niagara_script_remove_custom_hlsl_pin(script_path: str, node_id: str, pin: str) -> dict:
        """Remove an input or output pin of a Custom HLSL node (signature updated, links of that pin broken).
        Returns: JSON node or "ERROR|...".

        Forwarded to BlueprintInspector (BPINiagaraScriptToolset::RemoveCustomHlslPin); requires that plugin to be loaded."""
        return conn.call("niagara.script_remove_custom_hlsl_pin", {"script_path": script_path, "node_id": node_id, "pin": pin})

    @mcp.tool()
    def niagara_script_connect_pins(script_path: str, from_node_id: str, from_pin: str, to_node_id: str, to_pin: str) -> dict:
        """Connect two pins through the Niagara graph schema (UEdGraphSchema_Niagara::TryCreateConnection): type checks,
        replacing an existing input link, numeric resolution and automatic conversion nodes behave like dragging a wire.
        from_pin: Output pin (id or name). to_pin: Input pin (id or name).
        Returns: JSON {"response","message"} or "ERROR|<schema message>".

        Forwarded to BlueprintInspector (BPINiagaraScriptToolset::ConnectPins); requires that plugin to be loaded."""
        return conn.call("niagara.script_connect_pins", {"script_path": script_path, "from_node_id": from_node_id, "from_pin": from_pin, "to_node_id": to_node_id, "to_pin": to_pin})

    @mcp.tool()
    def niagara_script_disconnect_pin(script_path: str, node_id: str, pin: str, other_node_id: str, other_pin: str) -> dict:
        """Break links of a pin: all of them, or only the link to OtherNodeId/OtherPin when given.
        Returns: JSON {"brokenLinks":n} or "ERROR|...".

        Forwarded to BlueprintInspector (BPINiagaraScriptToolset::DisconnectPin); requires that plugin to be loaded."""
        return conn.call("niagara.script_disconnect_pin", {"script_path": script_path, "node_id": node_id, "pin": pin, "other_node_id": other_node_id, "other_pin": other_pin})

    @mcp.tool()
    def niagara_script_set_pin_default(script_path: str, node_id: str, pin: str, value_text: str) -> dict:
        """Set the inline default of an unlinked input pin (e.g. Op node "B", function call inputs, Custom HLSL inputs).
        Module input defaults on Map Get nodes are read-only: use SetScriptInputDefault.
        value_text: Value in Niagara pin format of the pin type.
        Returns: JSON {"pin","default"} (value read back) or "ERROR|...".

        Forwarded to BlueprintInspector (BPINiagaraScriptToolset::SetPinDefault); requires that plugin to be loaded."""
        return conn.call("niagara.script_set_pin_default", {"script_path": script_path, "node_id": node_id, "pin": pin, "value_text": value_text})

    @mcp.tool()
    def niagara_script_create_scratch_pad(owner_path: str, name: str, usage: str, target_stack_usage: str) -> dict:
        """Create a scratch pad script the way the scratch pad panel does ("+ Module" / "+ Dynamic Input"): a copy of the default
        module / dynamic input script owned by the system (UNiagaraSystem::ScratchPadScripts, shared by all its emitters) or,
        for an emitter asset, by the emitter's scratch pad container. Edit it with the other tools using the returned path,
        compile it with CompileNiagaraScript(bRefreshReferencers=true), and add it to a stack with the official
        NiagaraToolset_System.AddModule (ModuleAsset = the returned path; no separate tool is needed).
        owner_path: Niagara system asset path (system scratch pad) or Niagara emitter asset path.
        name: Script name (made unique in the owner). Empty = "ScratchModule" / "ScratchDynamicInput".
        usage: Module or DynamicInput.
        target_stack_usage: Stack the module is meant for, added to its usage bitmask like the editor does, e.g.
         "ParticleUpdate", "ParticleSpawn", "EmitterUpdate", "SystemUpdate". Empty = keep the default bitmask.
        Returns: JSON {"path","name","usage","owner","moduleUsages":[...]} or "ERROR|...".

        Forwarded to BlueprintInspector (BPINiagaraScriptToolset::CreateScratchPadScript); requires that plugin to be loaded."""
        return conn.call("niagara.script_create_scratch_pad", {"owner_path": owner_path, "name": name, "usage": usage, "target_stack_usage": target_stack_usage})

    @mcp.tool()
    def niagara_script_list_scratch_pad(owner_path: str) -> dict:
        """List the scratch pad scripts of a system (or emitter asset) with their object paths.
        owner_path: Niagara system or emitter asset path.
        Returns: JSON {"owner","scripts":[{"path","name","usage","moduleUsages":[...],"compileStatus"}]} or "ERROR|...".

        Forwarded to BlueprintInspector (BPINiagaraScriptToolset::ListScratchPadScripts); requires that plugin to be loaded."""
        return conn.call("niagara.script_list_scratch_pad", {"owner_path": owner_path})
