# JSON-RPC 2.0 Protocol Specification

## Transport

- **Protocol**: JSON-RPC 2.0 over TCP
- **Framing**: Single newline character (`\n`) as message delimiter
- **Encoding**: UTF-8
- **Line size limit**: 64 KB (65536 bytes). Requests exceeding this limit receive an `InvalidRequest` error and the connection is closed.

## Message Formats

### Request

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "editor.get_status",
  "params": {}
}
```

- `id`: string, number, or null. Omitting `id` (or setting to null) makes the request a **notification** — no response is sent.
- `params`: optional object.

### Success Response

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "level_name": "MainMap"
  }
}
```

### Error Response

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -32601,
    "message": "Method not found",
    "data": null
  }
}
```

- `result` and `error` are mutually exclusive. If both are present, `error` takes precedence.

## Standard Error Codes

| Code    | Name             | Meaning                                      |
|---------|------------------|----------------------------------------------|
| -32700  | ParseError       | Invalid JSON received                        |
| -32600  | InvalidRequest   | JSON-RPC structure is invalid                |
| -32601  | MethodNotFound   | Method does not exist                        |
| -32602  | InvalidParams    | Invalid method parameters                    |
| -32603  | InternalError    | Internal error (e.g. GameThread timeout)     |

## User-Defined Error Codes

| Range            | Purpose                              |
|------------------|--------------------------------------|
| -32000 to -32099 | Server-defined application errors    |

Example usage: `-32000` for "Editor not ready", `-32001` for "Asset not found". `-32010` is returned by `compile.*` when the LiveCoding module is not loaded or cannot be enabled.

## Conventions

These hold across the whole catalogue unless a method says otherwise.

- **Asset paths** are Unreal object paths: `/Game/Folder/Asset.Asset`. A package path (`/Game/Folder`) is only used where a method creates a new asset.
- **Class paths** are script paths: `/Script/Engine.PointLight`, `/Script/IFSR.STEval_SenseTarget`.
- **Import text** — parameters described as "Unreal import text" go through `FProperty::ImportText_Direct`, so they use Unreal's own literal syntax: `true`, `1.5`, `(R=1,G=0,B=0,A=1)`, `(TagName="Ability.Monster.Melee")`.
- **Editing marks dirty, it does not save.** Every mutating method calls `MarkPackageDirty`; persist with `asset.save`.
- **Compilation is explicit.** Graph edits do not recompile on their own — call `blueprint.compile`, `material.recompile`, `statetree.compile` or `niagara.compile` when a batch of edits is done.
- **`material_path` OR `function_path`** — material graph methods act on either a `UMaterial` or a `UMaterialFunction`. Supply exactly one.
- **All methods run on the game thread.** A request that arrives while the editor is blocked (compiling, modal dialog) waits rather than failing.

## Method Catalogue

Methods by family (the families added on 2026-10-27 are documented at the end).

| Family | Count | Scope |
|--------|-------|-------|
| [`editor.*`](#editor) | 4 | World state, actor spawning, Python passthrough |
| [`asset.*`](#asset) | 3 | Asset registry queries and saving |
| [`compile.*`](#compile) | 2 | Live Coding |
| [`pie.*`](#pie) | 2 | Play-In-Editor |
| [`blueprint.*`](#blueprint) | 4 | Blueprint assets and graph inspection |
| [`animgraph.*`](#animgraph) | 10 | AnimGraph state machines, players, transitions |
| [`animnotify.*`](#animnotify) | 2 | Animation notify inspection and retiming |
| [`statetree.*`](#statetree) | 9 | StateTree authoring |
| [`material.*`](#material) | 40 | Material graph authoring and instances |
| [`niagara.*`](#niagara) | 16 | Niagara module stacks, renderers and emitters |
| [`camerarig.*`](#camerarig) | 3 | Gameplay Camera System rigs |
| [`camvar.*`](#camvar) | 4 | Camera variable collections |
| [`widget.*`](#widget) | 52 | Widget Blueprint (UMG) trees, slots, bindings and animations |
| [`scene.*`](#scene) | 8 | Photo tools: never-saved temp actors, posed characters, ground height, game-view capture, shot lists |
| [`asset.*` search](#asset-search) | 3 | Name search, mesh bounds, animations for a skeleton |
| [`level.*` data](#level-data) | 7 | Actor queries, saved placement, batch property writes with dry run, instanced arrays, dirty files, splines, volumes |
| [`nav.*`](#nav) | 2 | Navmesh projection and path tests |
| [`datatable.*`](#datatable) | 2 | Row reads and JSON reimport with a row diff |
| [`pie.*` run](#pie-run), `automation.*` | 3 | Console commands in PIE, the captured editor log, automation tests |

---

<a name="editor"></a>
### editor.*

| Method                  | Params                                               | Result fields                                                                             | Description                                      |
|-------------------------|------------------------------------------------------|-------------------------------------------------------------------------------------------|--------------------------------------------------|
| `editor.get_status`     | —                                                    | `current_level: string, selected_actor_count: int, total_actor_count: int, is_pie_running: bool` | Returns editor world state summary    |
| `editor.list_actors`    | `filter_class?: string, max_count?: int` (def. 1000) | `actors: [{name, class, location: [x,y,z]}]`                                              | Lists actors in the current editor world         |
| `editor.spawn_actor`    | `class_path: string, location?: [x,y,z], rotation?: [pitch,yaw,roll]` | `actor_name: string, actor_path: string`                               | Spawns an actor by UClass object path            |
| `editor.python_exec`    | `script: string, mode?: string` (`ExecuteFile` def., `ExecuteStatement`, `EvaluateStatement`) | `success: bool, result: string, log: [{type, output}]`      | Runs Python in the editor via IPythonScriptPlugin |

**editor.get_status example**
```json
// request
{ "method": "editor.get_status" }
// result
{
  "current_level": "SPCombatTestMap",
  "selected_actor_count": 2,
  "total_actor_count": 147,
  "is_pie_running": false
}
```

**editor.list_actors example**
```json
// request
{ "method": "editor.list_actors", "params": { "filter_class": "StaticMeshActor", "max_count": 50 } }
// result
{
  "actors": [
    { "name": "SM_Rock_01", "class": "StaticMeshActor", "location": [100.0, 200.0, 0.0] }
  ]
}
```

**editor.spawn_actor example**
```json
// request
{ "method": "editor.spawn_actor", "params": { "class_path": "/Script/Engine.PointLight", "location": [0,0,300] } }
// result
{ "actor_name": "PointLight_1", "actor_path": "/Game/Maps/SPCombatTestMap.SPCombatTestMap:PersistentLevel.PointLight_1" }
```

**editor.python_exec example**
```json
// request
{ "method": "editor.python_exec", "params": { "script": "unreal.EditorLevelLibrary.get_selected_level_actors()", "mode": "EvaluateStatement" } }
// result
{ "success": true, "result": "[]", "log": [] }
```

---

<a name="asset"></a>
### asset.*

| Method                  | Params                                              | Result fields                                     | Description                                        |
|-------------------------|-----------------------------------------------------|---------------------------------------------------|----------------------------------------------------|
| `asset.list`            | `package_path: string, recursive?: bool` (def. false) | `assets: [{name, class, object_path}]`          | Lists assets under a content package path          |
| `asset.get_metadata`    | `object_path: string`                               | `class: string, tags: {key: value}`               | Returns AssetRegistry metadata for an asset        |
| `asset.save`            | `object_path: string`                               | `saved: bool`                                     | Saves an asset package to disk                     |

**asset.list example**
```json
// request
{ "method": "asset.list", "params": { "package_path": "/Game/Meshes", "recursive": true } }
// result
{
  "assets": [
    { "name": "SM_Rock", "class": "StaticMesh", "object_path": "/Game/Meshes/SM_Rock.SM_Rock" }
  ]
}
```

**asset.get_metadata example**
```json
// request
{ "method": "asset.get_metadata", "params": { "object_path": "/Game/Meshes/SM_Rock.SM_Rock" } }
// result
{ "class": "StaticMesh", "tags": { "Vertices": "1024", "Triangles": "512" } }
```

**asset.save example**
```json
// request
{ "method": "asset.save", "params": { "object_path": "/Game/Meshes/SM_Rock.SM_Rock" } }
// result
{ "saved": true }
```

---

<a name="compile"></a>
### compile.*

| Method                          | Params | Result fields                          | Description                                           |
|---------------------------------|--------|----------------------------------------|-------------------------------------------------------|
| `compile.trigger_live_coding`   | —      | `triggered: bool`                      | Triggers a Live Coding compile                        |
| `compile.status`                | —      | `is_compiling: bool, is_enabled: bool` | Returns current Live Coding state                     |

**compile.trigger_live_coding example**
```json
// result
{ "triggered": true }
```

**compile.status example**
```json
// result
{ "is_compiling": false, "is_enabled": true }
```

Live Coding cannot add new module dependencies or new source files to a module — those need a full rebuild with the editor closed.

---

<a name="pie"></a>
### pie.*

| Method      | Params                           | Result fields     | Description                              |
|-------------|----------------------------------|-------------------|------------------------------------------|
| `pie.start` | `mode?: string` ("PIE" or "SIE") | `queued: bool`    | Queues a Play-In-Editor start (deferred to next tick) |
| `pie.stop`  | —                                | `queued: bool`    | Queues an end-play request (deferred to next tick) |

**pie.start example**
```json
// request
{ "method": "pie.start", "params": { "mode": "PIE" } }
// result
{ "queued": true }
```

Both methods return as soon as the request is queued — `queued: true` means the editor accepted it, not that play has started. Poll `editor.get_status` for `is_pie_running`.

---

<a name="blueprint"></a>
### blueprint.*

| Method                       | Params                                                                 | Result fields                                          | Description                                    |
|------------------------------|------------------------------------------------------------------------|--------------------------------------------------------|------------------------------------------------|
| `blueprint.create`           | `package_path: string, asset_name: string, parent_class_path: string` | `blueprint_path: string`                               | Creates a new Blueprint asset                  |
| `blueprint.compile`          | `blueprint_path: string`                                               | `success: bool, error_count: int, warning_count: int`  | Compiles a Blueprint asset                     |
| `blueprint.list_variables`   | `blueprint_path: string`                                               | `variables: [{name, type, category}]`                  | Lists NewVariables declared in a Blueprint     |
| `blueprint.get_graph_nodes`  | `blueprint_path: string, graph_name?: string`                          | `graphs: [{name, nodes: [{class, title, pos_x, pos_y, pins: [{name, direction, type, subtype, subtype_object, default_value, connected_to}]}]}]` | Dumps nodes and pin links from ubergraph / function / macro graphs |

**blueprint.create example**
```json
// request
{ "method": "blueprint.create", "params": { "package_path": "/Game/Blueprints", "asset_name": "BP_MyActor", "parent_class_path": "/Script/Engine.Actor" } }
// result
{ "blueprint_path": "/Game/Blueprints/BP_MyActor.BP_MyActor" }
```

**blueprint.compile example**
```json
// request
{ "method": "blueprint.compile", "params": { "blueprint_path": "/Game/Blueprints/BP_MyActor.BP_MyActor_C" } }
// result
{ "success": true, "error_count": 0, "warning_count": 0 }
```

**blueprint.list_variables example**
```json
// request
{ "method": "blueprint.list_variables", "params": { "blueprint_path": "/Game/Blueprints/BP_MyActor.BP_MyActor_C" } }
// result
{ "variables": [ { "name": "Health", "type": "float", "category": "Stats" } ] }
```

> **Known gap:** the Python layer exposes a `blueprint_list_functions` tool, but `blueprint.list_functions` has no handler on the C++ side and returns `MethodNotFound`. Use `blueprint.get_graph_nodes` until it is implemented.

---

<a name="animgraph"></a>
### animgraph.*

Node-level authoring inside an Animation Blueprint. `graph_name` defaults to `"AnimGraph"`; for nodes that live inside a state, it resolves state bound graphs by state name. All methods take `blueprint_path` and return the created node's `node_name`, which the other methods use to address it.

| Method                            | Params                                                                                                        | Result fields         | Description                                                        |
|-----------------------------------|---------------------------------------------------------------------------------------------------------------|-----------------------|--------------------------------------------------------------------|
| `animgraph.add_state_machine`     | `blueprint_path, graph_name?, pos_x?, pos_y?`                                                                 | `node_name: string`   | Adds a State Machine node to an AnimGraph                          |
| `animgraph.add_state`             | `blueprint_path, state_machine_node, state_name, graph_name?, pos_x?, pos_y?`                                 | `node_name: string`   | Adds a state to a state machine's inner graph                      |
| `animgraph.add_sequence_player`   | `blueprint_path, state_machine_node, state_name, sequence_path, loop?` (def. true)`, graph_name?, pos_x?, pos_y?` | `node_name: string`   | Adds a SequencePlayer inside a state, wired to its pose sink       |
| `animgraph.add_blend_space_player`| `blueprint_path, state_machine_node, state_name, blend_space_path, graph_name?, pos_x?, pos_y?`               | `node_name: string`   | Adds a BlendSpacePlayer inside a state; axis pins left unwired     |
| `animgraph.add_transition`        | `blueprint_path, state_machine_node, from_state, to_state, condition_property?, negate_condition?, graph_name?` | `node_name: string`   | Adds a transition between two states                               |
| `animgraph.add_slot`              | `blueprint_path, slot_name, graph_name?, pos_x?, pos_y?`                                                      | `node_name: string`   | Adds a Slot node (montage layering point) to the top-level AnimGraph |
| `animgraph.add_variable_get`      | `blueprint_path, property_name, graph_name?, pos_x?, pos_y?`                                                  | `node_name: string`   | Adds a Get <property> node; its output pin is named after the property |
| `animgraph.connect_pins`          | `blueprint_path, graph_name, src_node, src_pin, dst_node, dst_pin`                                            | `connected: bool`     | Wires an output pin to an input pin within one graph               |
| `animgraph.disconnect_pins`       | `blueprint_path, graph_name, node_a, pin_a, node_b, pin_b`                                                    | `disconnected: bool`  | Removes a specific link between two pins                           |
| `animgraph.remove_node`           | `blueprint_path, graph_name, node_name`                                                                       | `removed: bool`       | Removes a node from the named graph                                |

A Slot node's pins are `"Source"` (in) and `"Pose"` (out) — wire them with `animgraph.connect_pins`.

**Typical locomotion build**
```json
{ "method": "animgraph.add_state_machine", "params": { "blueprint_path": "/Game/Anim/ABP_Monster.ABP_Monster", "pos_x": 0, "pos_y": 0 } }
{ "method": "animgraph.add_state", "params": { "blueprint_path": "/Game/Anim/ABP_Monster.ABP_Monster", "state_machine_node": "AnimGraphNode_StateMachine_0", "state_name": "Idle" } }
{ "method": "animgraph.add_blend_space_player", "params": { "blueprint_path": "/Game/Anim/ABP_Monster.ABP_Monster", "state_machine_node": "AnimGraphNode_StateMachine_0", "state_name": "Idle", "blend_space_path": "/Game/Anim/BS_Locomotion.BS_Locomotion" } }
{ "method": "animgraph.add_transition", "params": { "blueprint_path": "/Game/Anim/ABP_Monster.ABP_Monster", "state_machine_node": "AnimGraphNode_StateMachine_0", "from_state": "Idle", "to_state": "Run", "condition_property": "bIsMoving" } }
```

---

<a name="animnotify"></a>
### animnotify.*

| Method                  | Params                                                              | Result fields                                                        | Description                                  |
|-------------------------|---------------------------------------------------------------------|----------------------------------------------------------------------|----------------------------------------------|
| `animnotify.list`       | `animation_path: string, notify_class?: string`                     | `notifies: [{index, class, start_time, duration, track}]`            | Lists notify events; the filter matches subclasses too |
| `animnotify.set_window` | `animation_path: string, index: int, start_time?: number, duration?: number` | `ok: bool, class: string, start_time: number, duration: number` | Retimes an existing notify event in place    |

`index` comes from `animnotify.list`. `duration` applies to notify **states** only.

Retiming is done in place on purpose: removing and re-adding the event would rebuild its `Instanced` sub-objects from class defaults, silently discarding whatever the animator configured on them.

**animnotify.list example**
```json
// request
{ "method": "animnotify.list", "params": { "animation_path": "/Game/Anim/A_Attack.A_Attack", "notify_class": "AnimNotify_SpawnNiagaraEffect" } }
// result
{ "notifies": [ { "index": 2, "class": "AnimNotify_SpawnNiagaraEffect", "start_time": 0.35, "duration": 0.0, "track": "VFX" } ] }
```

---

<a name="statetree"></a>
### statetree.*

All methods take `tree_path`. States, nodes and transitions are addressed by the GUID strings returned when they are created.

| Method                          | Params                                                                          | Result fields          | Description                                                   |
|---------------------------------|----------------------------------------------------------------------------------|------------------------|---------------------------------------------------------------|
| `statetree.add_state`           | `tree_path, state_name, parent_state_id?` (empty = under Root)                  | `state_id: string`     | Adds a state; the Root subtree is created if missing           |
| `statetree.remove_state`        | `tree_path, state_id`                                                            | `removed: bool`        | Removes a state and its subtree                                |
| `statetree.add_evaluator`       | `tree_path, struct_path`                                                         | `node_id: string`      | Adds a tree-level Evaluator that runs every tick               |
| `statetree.add_task`            | `tree_path, state_id, struct_path`                                               | `node_id: string`      | Adds a Task that runs while the state is active                |
| `statetree.add_transition`      | `tree_path, from_state_id, to_state_id, trigger?`                                | `transition_id: string`| Adds a transition between states                               |
| `statetree.add_condition`       | `tree_path, transition_id` **or** `state_id`, `struct_path?`                     | `node_id: string`      | Gates a transition, or a state's selection (enter condition)   |
| `statetree.add_binding`         | `tree_path, source_node_id, source_path, target_node_id, target_path`            | `ok: bool`             | Binds a source node output to a target node input              |
| `statetree.set_node_property`   | `tree_path, node_id, property, value` (Unreal import text)                       | `ok: bool`             | Reflection write on any evaluator/task/condition instance data |
| `statetree.compile`             | `tree_path`                                                                      | `compiled: bool`       | Compiles the tree from its editor data                         |

`trigger` accepts `OnTick` (default), `OnStateCompleted`, `OnStateSucceeded`, `OnEvent`.

`statetree.add_condition` defaults `struct_path` to `FStateTreeCompareBoolCondition`. The typical wiring is: add the condition, bind an evaluator's bool output to its `bLeft` input via `statetree.add_binding`, then set the compared constant with `statetree.set_node_property` on `bRight`.

**Gating a transition on an evaluator output**
```json
{ "method": "statetree.add_evaluator", "params": { "tree_path": "/Game/AI/ST_Monster.ST_Monster", "struct_path": "/Script/IFSR.STEval_SenseTarget" } }
// -> { "node_id": "A1B2..." }
{ "method": "statetree.add_transition", "params": { "tree_path": "/Game/AI/ST_Monster.ST_Monster", "from_state_id": "...", "to_state_id": "...", "trigger": "OnTick" } }
// -> { "transition_id": "C3D4..." }
{ "method": "statetree.add_condition", "params": { "tree_path": "/Game/AI/ST_Monster.ST_Monster", "transition_id": "C3D4..." } }
// -> { "node_id": "E5F6..." }
{ "method": "statetree.add_binding", "params": { "tree_path": "...", "source_node_id": "A1B2...", "source_path": "bHasTarget", "target_node_id": "E5F6...", "target_path": "bLeft" } }
{ "method": "statetree.set_node_property", "params": { "tree_path": "...", "node_id": "E5F6...", "property": "bRight", "value": "true" } }
{ "method": "statetree.compile", "params": { "tree_path": "/Game/AI/ST_Monster.ST_Monster" } }
```

---

<a name="material"></a>
### material.*

The largest family. Graph methods take **`material_path` OR `function_path`**; instance methods take `instance_path`; a few take a generic `object_path`. Expression nodes are addressed by GUID, returned when created and listed by `material.list_expressions`.

#### Asset creation and inspection

| Method                        | Params                                              | Result fields                                                                                   | Description                                        |
|-------------------------------|-----------------------------------------------------|--------------------------------------------------------------------------------------------------|----------------------------------------------------|
| `material.create`             | `package_path, asset_name`                          | `object_path: string`                                                                            | Creates a `UMaterial`                              |
| `material.create_instance`    | `package_path, asset_name, parent_path`             | `object_path: string`                                                                            | Creates a `UMaterialInstanceConstant`              |
| `material.create_function`    | `package_path, asset_name`                          | `object_path: string`                                                                            | Creates a `UMaterialFunction`                      |
| `material.get_info`           | `object_path`                                       | `class, object_path, parent, num_expressions`                                                    | Works for materials and instances                  |
| `material.list_parameters`    | `object_path`                                       | `scalars, vectors, textures, static_switches` — each `[{name, value, overridden}]`               | Default values for a material, instance values + override flags for an instance |
| `material.get_statistics`     | `object_path`                                       | `vertex_shader_instructions, pixel_shader_instructions, samplers, vertex_texture_samples, pixel_texture_samples, virtual_texture_samples` | Shader cost counters |
| `material.set_property`       | `object_path, property, value` (import text)        | `ok: bool`                                                                                       | Reflection write on the asset itself (BlendMode, ShadingModel, TwoSided…) |

#### Instance parameters

| Method                                | Params                                                                       | Result fields | Description                                   |
|---------------------------------------|-------------------------------------------------------------------------------|---------------|-----------------------------------------------|
| `material.set_scalar_param`           | `instance_path, parameter_name, value: number, association?`                 | `ok: bool`    | Sets a scalar override                        |
| `material.set_vector_param`           | `instance_path, parameter_name, value: [r,g,b,a], association?`              | `ok: bool`    | Sets a vector override                        |
| `material.set_texture_param`          | `instance_path, parameter_name, texture_path, association?`                  | `ok: bool`    | Sets a texture override                       |
| `material.set_static_switch_param`    | `instance_path, parameter_name, value: bool, association?`                   | `ok: bool`    | Sets a static switch override                 |
| `material.set_parameter_override`     | `instance_path, parameter_name, override: bool, association?`                | `ok: bool`    | Toggles the override checkbox without changing the value |
| `material.clear_instance_parameters`  | `instance_path`                                                              | `ok: bool`    | Clears all overrides                          |
| `material.set_instance_parent`        | `instance_path, parent_path`                                                 | `ok: bool`    | Reparents the instance                        |
| `material.update_instance`            | `instance_path`                                                              | `ok: bool`    | Recompiles shaders after instance edits       |

`association` accepts `GlobalParameter` (default), `LayerParameter`, `BlendParameter`.

#### Graph nodes and wiring

| Method                              | Params                                                                                     | Result fields                                          | Description                                            |
|-------------------------------------|---------------------------------------------------------------------------------------------|--------------------------------------------------------|--------------------------------------------------------|
| `material.list_expressions`         | `material_path` \| `function_path`                                                          | `expressions: [{guid, class, pos_x, pos_y, size_x, size_y, inputs, outputs}], sizes_available: bool` | Node inventory with pin names |
| `material.add_expression`           | `material_path` \| `function_path`, `expression_class, pos_x?, pos_y?, asset_path?`         | `guid: string`                                         | Adds a node; `asset_path` seeds texture/function nodes |
| `material.set_expression_property`  | `material_path` \| `function_path`, `expression_guid, property, value` (import text)        | `ok: bool`                                             | Reflection write on a node                             |
| `material.connect_expressions`      | `material_path` \| `function_path`, `from_guid, to_guid, from_output?, to_input?`           | `ok: bool`                                             | Wires node to node; empty pin names use the first pin  |
| `material.connect_property`         | `material_path, from_guid, property, from_output?`                                          | `ok: bool`                                             | Wires a node output to a material property (`BaseColor` / `MP_BaseColor`) |
| `material.disconnect_expression`    | `material_path` \| `function_path`, `to_guid, to_input`                                     | `disconnected: bool`                                   | Clears an input pin                                    |
| `material.disconnect_property`      | `material_path, property`                                                                   | `disconnected: bool`                                   | Clears a material property input                       |
| `material.delete_expression`        | `material_path` \| `function_path`, `expression_guid`                                       | `ok: bool`                                             | Deletes one node                                       |
| `material.delete_all_expressions`   | `material_path` \| `function_path`                                                          | `ok: bool`                                             | Clears the graph                                       |
| `material.delete_unused_expressions`| `material_path`                                                                             | `ok: bool`                                             | Removes nodes unreachable from the outputs             |

`size_x`/`size_y` in `material.list_expressions` come from the live graph panel, so they are only filled when the asset editor is open — `sizes_available` says whether they are trustworthy.

#### Named reroutes

Named Reroutes are graph-local named variables: a **declaration** holds the value, **usages** read it. Usages resolve by GUID, so renaming a declaration updates them automatically.

| Method                                     | Params                                                                                            | Result fields                        | Description                                    |
|--------------------------------------------|----------------------------------------------------------------------------------------------------|--------------------------------------|------------------------------------------------|
| `material.add_named_reroute_declaration`   | `material_path` \| `function_path`, `name, pos_x?, pos_y?, node_color?: [r,g,b,a]`                 | `guid, variable_guid, name`          | Creates a declaration; feed it via `connect_expressions` with an empty `to_input` |
| `material.add_named_reroute_usage`         | `material_path` \| `function_path`, `declaration_guid, pos_x?, pos_y?`                            | `guid: string`                       | Creates a usage that reads a declaration       |
| `material.set_named_reroute_name`          | `material_path` \| `function_path`, `expression_guid, name`                                       | `ok: bool`                           | Renames a declaration                          |
| `material.list_named_reroutes`             | `material_path` \| `function_path`                                                                 | `declarations: [...], usages: [...]` | Lists both with their linkage                  |
| `material.add_channel_reroutes`            | `material_path` \| `function_path`, `channels?: [string], origin_x?, origin_y?, row_step?` (80)   | `declarations: [{name, guid, variable_guid}]` | Creates the standard PBR channel declarations with the project's fixed colors, stacked vertically |

`material.add_channel_reroutes` defaults to the seven standard channels: AO, Diffuse, F0, Roughness, Normal, Emissive, UVs.

#### Comments

| Method                        | Params                                                                                                       | Result fields                              | Description                                  |
|-------------------------------|---------------------------------------------------------------------------------------------------------------|--------------------------------------------|----------------------------------------------|
| `material.add_comment`        | `material_path` \| `function_path`, `text, pos_x?, pos_y?, size_x?` (400)`, size_y?` (200)`, font_size?` (18)`, color?: [r,g,b,a]` | `guid: string`         | Adds a free-floating comment box             |
| `material.add_group_comment`  | `material_path` \| `function_path`, `text, node_guids: [string], padding?` (48)`, font_size?` (18)`, color?`  | `guid, pos_x, pos_y, size_x, size_y`       | Comment box auto-sized around a set of nodes, in the project's standard group style |
| `material.list_comments`      | `material_path` \| `function_path`                                                                            | `comments: [{guid, text, pos_x, pos_y, size_x, size_y, font_size}]` | Lists comment boxes |
| `material.set_comment_text`   | `material_path` \| `function_path`, `comment_guid, text`                                                      | `ok: bool`                                 | Direct assignment, so commas and newlines survive |
| `material.delete_comment`     | `material_path` \| `function_path`, `comment_guid`                                                            | `ok: bool`                                 | Removes a comment box                        |

#### Layout and compilation

| Method                          | Params                                                                                                                  | Result fields                                        | Description                                        |
|---------------------------------|--------------------------------------------------------------------------------------------------------------------------|------------------------------------------------------|----------------------------------------------------|
| `material.set_node_position`    | `material_path` \| `function_path`, `expression_guid, pos_x, pos_y, snap?` (true, 16px)                                  | `pos_x, pos_y`                                       | Moves one node                                     |
| `material.arrange_grid`         | `material_path` \| `function_path`, `columns: [[guid]], origin_x?, origin_y?, col_step?` (256)`, row_step?` (80)`, snap?` | `placed: int, positions: [{guid, pos_x, pos_y}]`     | Bulk placement: `columns[i][j]` → `(origin_x + i*col_step, origin_y + j*row_step)` |
| `material.layout_expressions`   | `material_path` \| `function_path`                                                                                       | `ok: bool`                                           | Arranges nodes in a generic grid                   |
| `material.recompile`            | `material_path`                                                                                                          | `compiled: bool, errors: [string]`                   | Recompiles after graph edits                       |
| `material.update_function`      | `function_path`                                                                                                          | `ok: bool`                                           | Recompiles materials that use the function         |

`material.arrange_grid` is the fast path for laying out a whole graph — one call instead of N `set_node_position` calls.

**Building a two-node graph**
```json
{ "method": "material.create", "params": { "package_path": "/Game/Materials", "asset_name": "M_Test" } }
{ "method": "material.add_expression", "params": { "material_path": "/Game/Materials/M_Test.M_Test", "expression_class": "MaterialExpressionVectorParameter", "pos_x": -400, "pos_y": 0 } }
// -> { "guid": "AABB..." }
{ "method": "material.set_expression_property", "params": { "material_path": "/Game/Materials/M_Test.M_Test", "expression_guid": "AABB...", "property": "ParameterName", "value": "BaseTint" } }
{ "method": "material.connect_property", "params": { "material_path": "/Game/Materials/M_Test.M_Test", "from_guid": "AABB...", "property": "BaseColor" } }
{ "method": "material.recompile", "params": { "material_path": "/Game/Materials/M_Test.M_Test" } }
{ "method": "asset.save", "params": { "object_path": "/Game/Materials/M_Test.M_Test" } }
```

---

<a name="niagara"></a>
### niagara.*

Module stack, renderer and emitter authoring for Niagara systems. Every module and input method addresses one **script stack**, named by `stack`:

| `stack` value    | Lives on | `emitter_name` |
|------------------|----------|----------------|
| `SystemSpawn`    | system   | must be omitted |
| `SystemUpdate`   | system   | must be omitted |
| `EmitterSpawn`   | emitter  | required        |
| `EmitterUpdate`  | emitter  | required        |
| `ParticleSpawn`  | emitter  | required        |
| `ParticleUpdate` | emitter  | required        |
| `ParticleEvent`  | emitter  | required        |

The enum-name form (`ParticleSpawnScript`) is accepted too. Modules are addressed by `module_name` — the function name from `niagara.module_list`, falling back to the display name. Inputs are addressed by short name or full parameter handle.

| Method                          | Params                                                                                                   | Result fields                                                                            | Description                                          |
|---------------------------------|-----------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------|------------------------------------------------------|
| `niagara.get_info`              | `system_path`                                                                                             | `system, system_stacks: [{stack, module_count}], emitters: [{name, id, enabled, stacks}]` | Emitter inventory — start here to learn emitter names |
| `niagara.module_list`           | `system_path, stack, emitter_name?`                                                                       | `modules: [{name, display_name, index, enabled, inherited, can_move_and_delete, script}]` | Modules in execution order                           |
| `niagara.module_add`            | `system_path, stack, emitter_name?, module_script_path, target_index?` (-1 = append)`, suggested_name?`   | `module_name: string, index: int`                                                        | Inserts a module script onto the stack               |
| `niagara.module_remove`         | `system_path, stack, emitter_name?, module_name`                                                          | `ok: bool`                                                                               | Deletes a module                                     |
| `niagara.module_set_enabled`    | `system_path, stack, emitter_name?, module_name, enabled: bool`                                           | `ok: bool`                                                                               | Toggles a module, keeping its input values           |
| `niagara.input_list`            | `system_path, stack, emitter_name?, module_name`                                                          | `inputs: [{name, handle, type, value_mode, value, can_reset}]`                            | Inputs with their current values                     |
| `niagara.input_set_local`       | `system_path, stack, emitter_name?, module_name, input_name, value`                                       | `ok: bool`                                                                               | Sets an input to a constant                          |
| `niagara.input_set_linked`      | `system_path, stack, emitter_name?, module_name, input_name, parameter`                                   | `ok: bool`                                                                               | Points an input at another parameter                 |
| `niagara.input_set_dynamic`     | `system_path, stack, emitter_name?, module_name, input_name, dynamic_input_script_path, suggested_name?`  | `ok: bool`                                                                               | Drives an input with a dynamic input script          |
| `niagara.input_reset`           | `system_path, stack, emitter_name?, module_name, input_name`                                              | `ok: bool`                                                                               | Drops an override back to the default                |
| `niagara.renderer_list`         | `system_path, emitter_name, include_properties?` (def. false)                                             | `renderers: [{index, class, display_name, enabled, inherited, properties?}]`              | An emitter's renderers, in draw order                |
| `niagara.renderer_set_property` | `system_path, emitter_name, renderer_index, property, value` (import text)                                | `ok: bool`                                                                               | Reflection write on a renderer's properties object   |
| `niagara.renderer_set_enabled`  | `system_path, emitter_name, renderer_index, enabled: bool`                                                | `ok: bool`                                                                               | Toggles one renderer, keeping its settings           |
| `niagara.emitter_add`           | `system_path, emitter_asset_path, emitter_name?` (rename)                                                 | `name: string, id: string`                                                               | Adds an emitter asset at its exposed version         |
| `niagara.emitter_remove`        | `system_path, emitter_name`                                                                               | `ok: bool`                                                                               | Removes an emitter and its whole module stack        |
| `niagara.compile`               | `system_path, wait?` (def. true)                                                                          | `requested: bool, waited: bool`                                                          | Recompiles the system                                |

`type` is the Niagara type name — `NiagaraFloat`, `NiagaraInt32`, `NiagaraBool`, `Vector`, `Position`, `LinearColor`, and so on.

`value_mode` reports how an input is currently driven: `Local`, `Linked`, `LinkedShared`, `Dynamic`, `Data`, `ObjectAsset`, `Expression`, `DefaultFunction`, `InvalidOverride`, `UnsupportedDefault`, `None`.

`niagara.input_set_local` accepts three value forms:

- a bare scalar — `"8"`, `"true"` / `"false"`
- a comma-separated list in the type's field order — `"1,0,0.5"` for a vector, `"R,G,B,A"` for a color
- Unreal import text, for anything the first two cannot express — `"(X=1,Y=2,Z=0)"`

For a `Local` input, `niagara.input_list` reports `value` in the comma form, so a value read out can be written straight back. Booleans read and write as `true`/`false`; the `-1`/`0` encoding the Niagara VM requires internally is not exposed.

`module_script_path` must point at a `UNiagaraScript` whose usage is `Module`; `dynamic_input_script_path` at one whose usage is `DynamicInput`. Modules with `can_move_and_delete: false` are required by the stack or inherited from a parent emitter and cannot be removed — disable them instead.

Renderers have no stable name, so they are addressed by `renderer_index` — their position in `niagara.renderer_list`. Re-list after adding or removing one. `include_properties` adds a `properties` map of every editable property to its exported value; it is off by default because a sprite renderer alone carries ~56. That map is how you discover what a renderer class exposes and how you confirm a `renderer_set_property` write landed.

Each call builds a throwaway system view model in data-processing mode (no preview world, no sequencer, no undo registration), so edits are safe with the asset editor closed. Reordering an existing module is not supported; insert at the position you want with `target_index`.

**Adding a drag module and tuning it**
```json
{ "method": "niagara.get_info", "params": { "system_path": "/Game/VFX/NS_Blood.NS_Blood" } }
// -> { "emitters": [ { "name": "Spray", ... } ] }
{ "method": "niagara.module_add", "params": { "system_path": "/Game/VFX/NS_Blood.NS_Blood", "emitter_name": "Spray", "stack": "ParticleUpdate", "module_script_path": "/Niagara/Modules/Update/Forces/Drag.Drag" } }
// -> { "module_name": "Drag", "index": 3 }
{ "method": "niagara.input_list", "params": { "system_path": "/Game/VFX/NS_Blood.NS_Blood", "emitter_name": "Spray", "stack": "ParticleUpdate", "module_name": "Drag" } }
// -> { "inputs": [ { "name": "Drag", "handle": "Module.Drag", "type": "NiagaraFloat", "value_mode": "Local", "value": "1.0", "can_reset": false }, ... ] }
{ "method": "niagara.input_set_local", "params": { "system_path": "/Game/VFX/NS_Blood.NS_Blood", "emitter_name": "Spray", "stack": "ParticleUpdate", "module_name": "Drag", "input_name": "Drag", "value": "2.5" } }
{ "method": "niagara.compile", "params": { "system_path": "/Game/VFX/NS_Blood.NS_Blood" } }
{ "method": "asset.save", "params": { "object_path": "/Game/VFX/NS_Blood.NS_Blood" } }
```

**Tuning a renderer**
```json
{ "method": "niagara.renderer_list", "params": { "system_path": "/Game/VFX/NS_Blood.NS_Blood", "emitter_name": "Spray", "include_properties": true } }
// -> { "renderers": [ { "index": 0, "class": "NiagaraSpriteRendererProperties", "enabled": true, "properties": { "SortOrderHint": "0", "SubImageSize": "(X=1.000000,Y=1.000000)", ... } } ] }
{ "method": "niagara.renderer_set_property", "params": { "system_path": "/Game/VFX/NS_Blood.NS_Blood", "emitter_name": "Spray", "renderer_index": 0, "property": "SubImageSize", "value": "(X=4.0,Y=4.0)" } }
```

---

<a name="camerarig"></a>
### camerarig.*

| Method                        | Params                                                                              | Result fields                                     | Description                                        |
|-------------------------------|--------------------------------------------------------------------------------------|---------------------------------------------------|----------------------------------------------------|
| `camerarig.get_info`          | `rig_path`                                                                           | `root_node_class: string, nodes: [{class, name}]` | Lists the rig's camera nodes                       |
| `camerarig.set_node_property` | `rig_path, node_class` (short class name)`, property_name, value` (import text)`, node_index?` (def. 0) | `ok: bool, node: string`   | Reflection write on a node found by class          |
| `camerarig.build`             | `rig_path`                                                                           | `ok: bool`                                        | Rebuilds the rig (validate + allocation info)      |

`node_index` picks which match to edit when several nodes share a class. Camera parameters wrap their value in a `Value` field:

```json
{ "method": "camerarig.set_node_property", "params": {
    "rig_path": "/Game/Camera/CR_Combat.CR_Combat",
    "node_class": "BoomArmCameraNode",
    "property_name": "BoomOffset",
    "value": "(Value=(X=-200.0,Y=60.0,Z=60.0))"
} }
```

---

<a name="camvar"></a>
### camvar.*

Camera variables are typed values shared across rigs — bind a rig node's `CameraParameter.Variable` to one so an offset or FOV transfers between rigs.

| Method                     | Params                                                              | Result fields                                              | Description                                     |
|----------------------------|----------------------------------------------------------------------|------------------------------------------------------------|-------------------------------------------------|
| `camvar.create_collection` | `package_path, name`                                                | `ok: bool, path: string`                                   | Creates an empty `UCameraVariableCollection`    |
| `camvar.add`               | `collection_path, var_type, name, default_value?` (import text)     | `ok, name, type, guid, object`                             | Adds a typed variable; `name` becomes DisplayName |
| `camvar.list`              | `collection_path`                                                   | `variables: [{index, name, type, guid, default_value}]`    | Lists variables in a collection                 |
| `camvar.set_default`       | `collection_path, name, value` (import text)                        | `ok: bool`                                                 | Sets a variable's default, matched by DisplayName |

`var_type` is the short type name: `Boolean`, `Integer32`, `Float`, `Double`, `Vector2f`, `Vector2d`, `Vector3f`, `Vector3d`, `Vector4f`, `Vector4d`, `Rotator3f`, `Rotator3d`, `Transform3f`, `Transform3d`. A leading `U` or trailing `CameraVariable` is tolerated.

Authoring camera variables is otherwise editor-UI-only — there is no scripting API for it in the engine.

**camvar.add example**
```json
{ "method": "camvar.add", "params": {
    "collection_path": "/Game/Camera/CVC_Combat.CVC_Combat",
    "var_type": "Vector3d",
    "name": "ShoulderOffset",
    "default_value": "(X=0,Y=45,Z=-5)"
} }
// result
{ "ok": true, "name": "ShoulderOffset", "type": "Vector3d", "guid": "...", "object": "..." }
```

---

<a name="widget"></a>
### widget.*

Widget Blueprint (UMG) authoring: the widget tree, widget and slot properties, named slots, property bindings, event graph bindings, UI components and animations.

Everything addresses one asset through `widget_path` (`/Game/UI/WBP_Menu.WBP_Menu`), and widgets within it by their tree name. **Widget and slot classes are addressed by class path** rather than having a method per type — `/Script/UMG.Button` for native classes, `/Game/UI/WBP_Card.WBP_Card` for widget blueprints (the `_C` class form is accepted too). UMG ships 46 widget classes and 18 slot classes and projects add more, so the class surface is data, not API.

Widget trees, animations and bindings are all compile-time constructs: call `widget.compile` after a batch of edits before testing in PIE.

#### Blueprint level

| Method                              | Params                                                          | Result fields                                                                                                       | Description                                                   |
|-------------------------------------|-----------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------|
| `widget.create`                     | `package_path, asset_name, parent_class?, root_widget_class?`   | `object_path, parent_class, root_widget?`                                                                            | New Widget Blueprint; `parent_class` defaults to `UserWidget`  |
| `widget.get_info`                   | `widget_path`                                                   | `parent_class, palette_category, widget_count, animation_count, binding_count, inherited_named_slots, exposed_named_slots, root_widget?` | Asset-level summary                  |
| `widget.list_blueprint_properties`  | `widget_path, target?` (`blueprint`\|`cdo`)                     | `target_class, properties: [{name, type, value, category, read_only}]`                                               | Editable properties on the asset or its CDO                    |
| `widget.set_property`               | `widget_path, property, value, target?`                         | `ok`                                                                                                                 | Reflection write on the asset or its CDO                       |
| `widget.set_desired_focus`          | `widget_path, widget_name?` (empty clears)                      | `ok`                                                                                                                 | Sets the widget focused on activation                          |
| `widget.compile`                    | `widget_path`                                                   | `compiled, has_warnings, status`                                                                                     | Compiles the blueprint                                         |
| `widget.validate`                   | `widget_path`                                                   | `valid, issues: [string]`                                                                                            | Circular references and inherited name conflicts               |
| `widget.palette_list`               | `include_blueprints?, name_filter?`                             | `count, classes: [{name, class, category, is_panel, is_user_widget, is_blueprint}]`                                   | Placeable widget classes                                       |

#### Widget tree

| Method                            | Params                                                                   | Result fields                                                                                                                       | Description                                                          |
|-----------------------------------|--------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------|
| `widget.tree_list`                | `widget_path, start_widget?, max_depth?` (-1 unlimited)                  | `tree_depth, count, widgets: [{name, class, label, depth, parent?, slot_class?, named_slot?, child_count?, is_variable, is_panel}]`   | Depth-first hierarchy walk incl. named slot content                   |
| `widget.get_widget`               | `widget_path, widget_name`                                               | `name, class, parent?, slot_class?, is_root, children?, slot_class_for_children?, can_have_multiple_children?, named_slots?`          | One widget in detail                                                  |
| `widget.add`                      | `widget_path, widget_class, parent_widget?, child_index?, widget_name?`  | `name, class, is_root, ...`                                                                                                          | Creates a widget and inserts it; sets the root when the tree is empty  |
| `widget.remove`                   | `widget_path, widget_name`                                               | `ok`                                                                                                                                 | Deletes a widget and its subtree, cleaning graph references            |
| `widget.rename`                   | `widget_path, widget_name, new_name`                                     | `ok, name`                                                                                                                           | Renames and retargets the blueprint variable                          |
| `widget.move`                     | `widget_path, widget_name, new_parent, child_index?`                     | `ok`                                                                                                                                 | Reparents or reorders; `new_parent` must be a panel                    |
| `widget.replace`                  | `widget_path, widget_name, widget_class`                                 | `ok`                                                                                                                                 | Swaps class, keeping compatible references                            |
| `widget.replace_with_child`       | `widget_path, widget_name`                                               | `ok`                                                                                                                                 | Collapses a single-child panel into its child                         |
| `widget.replace_with_named_slot`  | `widget_path, widget_name, slot_name`                                    | `ok`                                                                                                                                 | Collapses a named slot host into that slot's content                   |
| `widget.wrap`                     | `widget_path, widget_names: [string], widget_class`                      | `ok, wrappers: [...]`                                                                                                                | Wraps widgets in a new panel                                          |
| `widget.set_is_variable`          | `widget_path, widget_name, is_variable`                                  | `ok, is_variable`                                                                                                                    | Exposes the widget as a blueprint variable                            |
| `widget.set_root`                 | `widget_path, widget_name?` (empty clears)                               | `ok`                                                                                                                                 | Makes a parentless widget the tree root                               |
| `widget.duplicate`                | `widget_path, widget_name, parent_widget?`                               | `ok, widgets: [...]`                                                                                                                 | Clones a subtree with fresh names                                     |

#### Widget and slot properties

Values use Unreal's property text form — structs as `(X=10,Y=20)`, enums as their entry name, bools as `true`/`false`. The value strings the `list_*` methods report are accepted back verbatim by the matching `set_*`.

Layout **inside the parent panel** lives on the slot, not the widget: position and size go through `widget.set_slot_property`, appearance through `widget.set_widget_property`. The root widget and widgets in named slots have no slot.

| Method                          | Params                                       | Result fields                                                           | Description                             |
|---------------------------------|----------------------------------------------|-------------------------------------------------------------------------|-----------------------------------------|
| `widget.list_properties`        | `widget_path, widget_name, editable_only?`   | `widget, class, properties: [{name, type, value, category, read_only}]`  | Properties of one widget                 |
| `widget.set_widget_property`    | `widget_path, widget_name, property, value`  | `ok`                                                                    | Reflection write on a widget             |
| `widget.list_slot_properties`   | `widget_path, widget_name, editable_only?`   | `widget, slot_class, properties: [...]`                                 | Layout properties of the widget's slot   |
| `widget.set_slot_property`      | `widget_path, widget_name, property, value`  | `ok`                                                                    | Reflection write on the widget's slot    |

#### Named slots

Only three things host named slots: a **nested user widget** placed in this tree (it offers the slots its own blueprint exposes), an **ExpandableArea**, and the **blueprint itself** — omit `host_widget` for that, which addresses the slots inherited from the parent class.

A `NamedSlot` widget is **not** a host. It *declares* a slot for blueprints that consume this one, and locally it is an ordinary content widget: its content goes in with `widget.add` / `widget.move`. `widget.get_info` reports what a blueprint declares as `exposed_named_slots`, and what it can fill as `inherited_named_slots`.

| Method                            | Params                                                    | Result fields                                    | Description                                                         |
|-----------------------------------|-----------------------------------------------------------|--------------------------------------------------|---------------------------------------------------------------------|
| `widget.named_slot_list`          | `widget_path, host_widget?`                               | `host, slots: [{name, content, content_class}]`   | Named slots and their content                                        |
| `widget.named_slot_set_content`   | `widget_path, slot_name, content_widget, host_widget?`    | `ok`                                             | Places a widget into a slot, detaching it from its current parent    |
| `widget.named_slot_clear`         | `widget_path, slot_name, host_widget?, delete_content?`   | `ok, previous_content`                           | Empties a slot                                                       |

#### Property bindings and events

| Method                    | Params                                                                        | Result fields                                                                                     | Description                                              |
|---------------------------|-------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------|----------------------------------------------------------|
| `widget.binding_list`     | `widget_path`                                                                 | `count, bindings: [{widget_name, property, kind, function_name, source_property, target_exists}]`   | Property bindings on the blueprint                        |
| `widget.binding_add`      | `widget_path, widget_name, property, source, kind?` (`function`\|`property`)  | `ok`                                                                                              | Binds a widget property to a function or variable         |
| `widget.binding_remove`   | `widget_path, widget_name, property`                                          | `ok, removed`                                                                                     | Drops a property binding                                  |
| `widget.list_events`      | `widget_path, widget_name`                                                    | `widget, class, events: [{name, category, signature}]`                                            | Events the widget exposes to the graph                    |
| `widget.bind_event`       | `widget_path, widget_name, event_name`                                        | `ok`                                                                                              | Creates the event graph node; widget must be a variable    |

`widget.binding_add` requires the blueprint to have been compiled at least once so the bound member exists on the skeleton class. Property bindings re-evaluate every frame — prefer setting values from an event where the design allows.

#### UI components

| Method                     | Params                                                                       | Result fields   | Description                              |
|----------------------------|------------------------------------------------------------------------------|-----------------|------------------------------------------|
| `widget.component_add`     | `widget_path, widget_name, component_class`                                  | `ok, widget`    | Attaches a `UUIComponent` to a widget     |
| `widget.component_remove`  | `widget_path, widget_name, component_class`                                  | `ok`            | Detaches a UI component                   |
| `widget.component_move`    | `widget_path, widget_name, component_class, relative_to_class, move_after?`  | `ok`            | Reorders components on the same widget    |

#### Clipboard text

| Method                | Params                                  | Result fields                       | Description                                          |
|-----------------------|-----------------------------------------|-------------------------------------|------------------------------------------------------|
| `widget.export_text`  | `widget_path, widget_names: [string]`   | `text`                              | Serialises widgets to the editor's clipboard format   |
| `widget.import_text`  | `widget_path, text, parent_widget?`     | `ok, imported_count, roots: [...]`  | Recreates widgets from clipboard text                 |

#### Animations

A `UWidgetAnimation` is a `UMovieSceneSequence`, so authoring follows a fixed chain — each step depends on the one before it, and skipping one leaves an animation that plays but changes nothing:

```
anim_create -> anim_bind_widget -> anim_track_add -> anim_key_add
```

`bind_slot` decides *what* gets animated. `false` binds the widget itself (colour, opacity, visibility, render transform); `true` binds the widget's slot in its parent panel, which is how position and size are animated (a `CanvasPanelSlot`'s `LayoutData`, a box slot's `Padding`). A widget can carry both bindings.

Times cross the wire in **seconds**. The track type is inferred from the property's type and covers everything UMG animates; `track_class` overrides it for a custom track.

| Method                        | Params                                                                                                                        | Result fields                                                                                                | Description                                       |
|-------------------------------|-------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------|---------------------------------------------------|
| `widget.anim_list`            | `widget_path`                                                                                                                 | `count, animations: [{name, display_label, start_time, end_time, binding_count, display_rate, track_count}]`   | Animations on the blueprint                        |
| `widget.anim_create`          | `widget_path, animation_name, start_time?, end_time?, frame_rate?`                                                            | `ok, name`                                                                                                    | New animation; becomes a blueprint variable        |
| `widget.anim_remove`          | `widget_path, animation_name`                                                                                                 | `ok`                                                                                                          | Deletes an animation and its variable              |
| `widget.anim_rename`          | `widget_path, animation_name, new_name`                                                                                       | `ok, name`                                                                                                    | Renames and retargets graph references             |
| `widget.anim_set_range`       | `widget_path, animation_name, start_time?, end_time?, frame_rate?`                                                            | `ok, start_time, end_time, display_rate`                                                                      | Playback range and display rate                    |
| `widget.anim_binding_list`    | `widget_path, animation_name`                                                                                                 | `count, bindings: [{widget_name, slot_widget_name, guid, is_root_widget, track_count}]`                        | Widgets the animation possesses                    |
| `widget.anim_bind_widget`     | `widget_path, animation_name, widget_name, bind_slot?`                                                                        | `ok, guid, already_bound?`                                                                                    | Makes a widget or its slot animatable              |
| `widget.anim_unbind_widget`   | `widget_path, animation_name, widget_name`                                                                                    | `ok`                                                                                                          | Removes a binding and its tracks                   |
| `widget.anim_track_list`      | `widget_path, animation_name, widget_name?`                                                                                   | `count, tracks: [{widget_name, guid, track_class, track_name, property, sections: [{index, row_index, start_time, end_time, channels}]}]` | Tracks, sections and channels |
| `widget.anim_track_add`       | `widget_path, animation_name, widget_name, property, track_class?, create_section?`                                           | `ok, track_class, property, section_index?, channels?, already_existed?`                                      | Adds a property track, plus a section by default   |
| `widget.anim_track_remove`    | `widget_path, animation_name, widget_name, property`                                                                          | `ok, removed`                                                                                                 | Deletes a track and its keys                       |
| `widget.anim_section_add`     | `widget_path, animation_name, widget_name, property, start_time, end_time, row_index?`                                        | `ok, section_index, channels`                                                                                 | Extra section on an existing track                 |
| `widget.anim_key_add`         | `widget_path, animation_name, widget_name, property, time, value, channel?, channel_index?, section_index?, interpolation?`    | `ok, frame, key_count`                                                                                        | Writes a keyframe                                  |
| `widget.anim_key_list`        | `widget_path, animation_name, widget_name, property, channel?, channel_index?, section_index?`                                 | `count, keys: [{frame, time}]`                                                                                | Keyframe times on one channel                      |

A track has one channel per animatable number: `RenderOpacity` has a single channel, a colour has four (`Color.R`..`Color.A`), a 2D transform has seven (`Translation.X`, `Translation.Y`, `Angle`, `Scale.X`, `Scale.Y`, `Shear.X`, `Shear.Y`). Name the one you want with `channel`, using the `name` field `anim_track_list` reports; the leaf on its own resolves when unambiguous (`R` works, `X` on a transform does not). Leave it empty and use `channel_index` for single-channel tracks. The `display_name` field is localised and is not matched. `interpolation` is `cubic` (default), `linear` or `constant`; discrete channels — bool, int, enum — ignore it. A key placed outside the section's range extends the section to include it.

**Fade-in animation, end to end**
```json
{ "method": "widget.anim_create",      "params": { "widget_path": "/Game/UI/WBP_Menu.WBP_Menu", "animation_name": "FadeIn", "end_time": 0.4 } }
{ "method": "widget.anim_bind_widget", "params": { "widget_path": "/Game/UI/WBP_Menu.WBP_Menu", "animation_name": "FadeIn", "widget_name": "RootPanel" } }
{ "method": "widget.anim_track_add",   "params": { "widget_path": "/Game/UI/WBP_Menu.WBP_Menu", "animation_name": "FadeIn", "widget_name": "RootPanel", "property": "RenderOpacity" } }
{ "method": "widget.anim_key_add",     "params": { "widget_path": "/Game/UI/WBP_Menu.WBP_Menu", "animation_name": "FadeIn", "widget_name": "RootPanel", "property": "RenderOpacity", "time": 0.0, "value": 0.0 } }
{ "method": "widget.anim_key_add",     "params": { "widget_path": "/Game/UI/WBP_Menu.WBP_Menu", "animation_name": "FadeIn", "widget_name": "RootPanel", "property": "RenderOpacity", "time": 0.4, "value": 1.0 } }
{ "method": "widget.compile",          "params": { "widget_path": "/Game/UI/WBP_Menu.WBP_Menu" } }
```

---

<a name="scene"></a>
### scene.*

The "temp" promise is enforced in C++: temp actors are `RF_Transient` with no external package (so even a World Partition map gets no `__ExternalActors__` file), nothing calls `Modify` on a real level actor, and nothing is saved. Every mutating command returns `level_dirty` (true when any package became dirty during the call), `dirtied_packages` and `map_dirty`. Every temp actor carries the tag `MCPTemp`; `scene.clear_temp` scans the world for it.

| Method | Params | Result fields | Description |
|--------|--------|---------------|-------------|
| `scene.capture_view` | `location, look_at? or rotation?, fov? (60), width?/height? (1600x900), ortho_width?, path?, marks?, game_view? (true), warmup? (4), hide_actors?, respect_editor_hidden? (true), stream_textures? (true), stream_seconds? (2)` | `ok, path, file_size, width, height, location, rotation, fov, marks_px: [{name, x, y, in_view, behind_camera}], hidden_actor_count` | Renders a game view with a scene capture in the same call and returns once the file is on disk. `path` must be under `<Project>/Saved` (relative = under Saved) or a `CaptureOutputRoots` folder |
| `scene.spawn_temp` | `asset? or size_m?, location?, rotation?, scale?, material? or color?, properties?, tag?, label?, snap_to_ground?, z_offset?, items?` | `actors: [{name, label, path, class, location, rotation, bounds, tags}], level_dirty, ...` | Never-saved actors: meshes, allow-listed actor Blueprints (`TempSpawnBlueprintAllowList`), `PointLight`/`SpotLight`/`RectLight`/`DirectionalLight`/`CameraActor`, or a blockout box (`size_m` in metres). `items` is all-or-nothing |
| `scene.clear_temp` | `tag?` | `removed_count, removed, reverted_overrides, level_dirty, ...` | Removes temp actors (all or by tag) and reverts `temp_override` changes |
| `scene.pose_character` | `meshes? or preset? (+ preset_file?), anim?, time? or fraction?, yaw_offset?, weapon?, socket?, weapon_location?, weapon_rotation?, location?, rotation?, tag?, label?, snap_to_ground?` | actor fields + `parts, anim, time, length, facing_yaw, yaw_offset, warnings, level_dirty, ...` | Multi-part character (first mesh leads, the rest use leader pose) frozen on one frame; editor animation update on, pose evaluated before returning. Presets come from a JSON file (`PosePresetFile`, default `Saved/MCPBridge/pose_presets.json`): `{"presets": {"Name": {"meshes": [...], "weapon": "...", "socket": "...", "yaw_offset": -90}}}` |
| `scene.temp_override` | `actor, hide?, anim?, time? or fraction?, component?, tag?, revert?` | `actor, hidden, anim_overridden, time, tag, cleaned_dirty_flags, level_dirty, ...` | Borrows a real level actor: editor-temporary hide (never `bHidden`) and/or a frozen animation frame. Reverted by `revert`, `scene.clear_temp`, and automatically just before the actor's package is saved |
| `scene.get_ground_height` | `points: [[x, y]], ignore? (["volumes", "temp"]; "roofs" = lowest), layer? (0 top, -1 lowest), top_z?, bottom_z?, include_layers? (true)` | `points: [{x, y, hit, z, actor, layers: [{z, normal_z, actor, actor_path, component}]}], layer` | Read-only. Visibility-channel trace straight down; each component counts once, so roof, floor and terrain are separate layers |
| `scene.run_shots` | `shots: [{id, spawn?, pose?, override?, camera?, path?, settle_frames?, keep?}], settle_frames? (4), timeout_s? (60), on_fail? ("continue" or "stop")` | `job_id, total` | Starts a shot list worked on the core ticker, one step per editor frame; settle frames are real frames rendered into the capture. Each shot's temp actors are cleared after its capture unless `keep` |
| `scene.shots_status` | `job_id` | `job_id, done, index, total, failed, elapsed_seconds, results: [{id, ok, path, capture, error, setup, seconds}]` | Read-only. Finished jobs are kept 10 minutes |

<a name="asset-search"></a>
### asset.* (search)

| Method | Params | Result fields | Description |
|--------|--------|---------------|-------------|
| `asset.find` | `query? (space-separated words, all must match), class?, path? ("/Game"), max? (100), include_size?` | `assets: [{name, class, object_path, package_path, skeleton?, disk_size?}], total, truncated` | Read-only asset registry search; `class` includes subclasses |
| `asset.bounds` | `asset or assets` | `bounds: [{asset, class, center, extent, size_m, min, max, sphere_radius, pivot_to_bottom} or {asset, error}]` | Static / skeletal mesh size (skeletal = reference pose) |
| `asset.anims_for_skeleton` | `skeleton or mesh, classes? (["AnimSequence"]), query?, path?, max? (500)` | `skeleton, anims: [{name, class, object_path, skeleton, frames, frame_rate, length}], total, truncated` | Read-only; honours compatible skeletons |

<a name="level-data"></a>
### level.* (data authoring)

Writes are saved level data: one `FScopedTransaction` per call (one undo step), the same property path rules as `object.*` (`A.B`, `A[2]`, component hops), and nothing is saved. Values are ImportText strings, numbers or bools; number and bool text is validated before import (ImportText alone would read `"abc"` as 0).

| Method | Params | Result fields | Description |
|--------|--------|---------------|-------------|
| `level.query_actors` | `class?, exact_class?, folder?, label? (substring or * wildcard), tag?, sphere? {center, radius}, box? {min, max}, include_temp?, properties?, include_bounds?, max? (500)` | `actors: [{object_path, name, label, class, folder, location, rotation, external_package, file, distance?, bounds?, properties?}], total, truncated` | Read-only |
| `level.actor_spawn` | `class or asset, location?, rotation?, scale?, label?, folder?, properties?, snap_to_ground?, z_offset?, items?, dry_run?` | `actors: [{..., external_package, file, bounds}], level_dirty, ...` or `dry_run, plan` | Saved placement; `dry_run` checks class and properties against the class defaults |
| `level.actors_set_properties` | `changes: [{actor, properties}] or actors + properties, dry_run?` | `dry_run, changed_count, diff: [{actor, actor_path, property, old, new, changed}], level_dirty?, ...` | Every write is parsed before any is made |
| `level.actor_add_instanced` | `actor, property, class, properties?, index?, dry_run?` | `actor, property, index, object_path, class, count, properties, level_dirty, ...` | Adds an instanced subobject to an array property (e.g. `TriggerActions`) |
| `level.dirty_files` | `include_assets?` | `files: [{package, file, kind, exists_on_disk, read_only, p4, actor?, actor_class?}], counts` | Read-only. The map and `__ExternalActors__` / `__ExternalObjects__` files a save would write; `p4` is an add / edit / delete hint |
| `level.spline_set_points` | `actor, points, component?, space? ("world"), closed_loop?, point_type?, dry_run?` | `actor, component, old_points, new_points, closed_loop, level_dirty, ...` | Marks the spline edited so a construction script keeps it |
| `level.volume_set_extent` | `actor, extent or size_m, component?, dry_run?` | `actor, method, component, old_extent, new_extent, level_dirty, ...` | Box extent, sphere radius, capsule size, or brush-volume scale |

<a name="nav"></a>
### nav.*

| Method | Params | Result fields | Description |
|--------|--------|---------------|-------------|
| `nav.project_point` | `points, extent? ([50, 50, 250]), world? ("editor" or "pie")` | `points: [{input, ok, location, distance}], nav_data` | Read-only |
| `nav.test_path` | `start + end or pairs: [{start, end}], extent?, world?` | `paths: [{start, end, projected, ok, partial, length, points}], nav_data` | Read-only; both ends are projected first |

<a name="datatable"></a>
### datatable.*

| Method | Params | Result fields | Description |
|--------|--------|---------------|-------------|
| `datatable.get_rows` | `table, rows?, columns?, max? (500)` | `table, row_struct, source_file, row_count, columns, rows: [{name, values}], missing_rows, unknown_columns, truncated` | Read-only; values are the table's export text |
| `datatable.reimport` | `table, json_path?, dry_run? (true)` | `table, source_file, dry_run, applied, added, removed, changed: [{row, columns: [{column, old, new}]}], unchanged_count, problems` | Re-reads the JSON source through `UDataTable::CreateTableFromJSONString` (no modal dialogs); a JSON with no rows is refused. Not saved |

There is no row write: tables are produced by Excel -> DataGen -> JSON, and a hand-written row would be overwritten by the next run.

<a name="pie-run"></a>
### pie.* (run and check), automation.*

| Method | Params | Result fields | Description |
|--------|--------|---------------|-------------|
| `pie.exec_console` | `command, world? ("pie" while PIE runs, else "editor"), player? (0)` | `world, command, output, log_lines, log_cursor` | Runs through the PIE player's controller (Exec cheats, cheat manager) or the editor world |
| `pie.read_log` | `since?, contains?, category?, verbosity?, max? (200)` | `lines: [{seq, category, verbosity, text}], next, dropped` | Read-only. The editor log captured by the bridge since startup (last 20000 lines) |
| `automation.run_test` | `filter, max_tests? (50), latent_seconds? (5)` | `tests: [{name, ok, errors, warnings, seconds, messages, latent_timeout?}], passed, failed` | Runs tests whose full path starts with `filter`; tests that wait on frames report `latent_timeout` |
