# JSON-RPC 2.0 Protocol Specification

## Transport

- **Protocol**: JSON-RPC 2.0 over TCP
- **Framing**: Single newline character (`\n`) as message delimiter
- **Encoding**: UTF-8
- **Line size limit**: 16 MiB (16777216 bytes) by default, settable per side (`MaxLineBytes` in the plugin, `UMCP_MAX_LINE_BYTES` in the Python server). Requests exceeding the plugin's limit receive an `InvalidRequest` error and the connection is closed.
- **Concurrent clients**: a listener serves each accepted connection on its own thread, so several clients can share one port; requests are still executed one at a time on the GameThread. A connection may carry any number of requests, or just one — the bundled Python server opens a connection per call and closes it after the reply. `AdditionalPorts` starts extra listener threads (one per port, all sharing one command registry) but is not required for multiple clients.

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

Example usage: `-32000` for "Editor not ready", `-32001` for "Asset not found".

## Method Catalogue

The tables below cover the core domains in detail. The bridge also serves `animgraph.*`,
`animnotify.*`, `behaviortree.*`, `blendspace.*`, `montage.*`, `statetree.*`, `camerarig.*`, `camvar.*`, `material.*`, `niagara.*` and
`object.*`, which are
documented by the docstrings on their `@mcp.tool()` wrappers in
`Python/unreal_mcp_bridge/tools/`. Every registered method has exactly one such wrapper, so the
tool list an MCP client sees and this catalogue are the same set. The authoritative list of methods is the set of
`Registry.Register(TEXT("..."))` calls under
`Plugin/UnrealMCPBridge/Source/UnrealMCPBridge/Private/Commands/`.

### editor.*

| Method                  | Params                                               | Result fields                                                                             | Description                                      |
|-------------------------|------------------------------------------------------|-------------------------------------------------------------------------------------------|--------------------------------------------------|
| `editor.get_status`     | —                                                    | `current_level: string, selected_actor_count: int, total_actor_count: int, is_pie_running: bool` | Returns editor world state summary    |
| `editor.list_actors`    | `filter_class?: string, max_count?: int` (def. 1000) | `actors: [{name, class, location: [x,y,z]}]`                                              | Lists actors in the current editor world         |
| `editor.spawn_actor`    | `class_path: string, location?: [x,y,z], rotation?: [pitch,yaw,roll]` | `actor_name: string, actor_path: string`                               | Spawns an actor by UClass object path            |

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

---

### blueprint.*

| Method                       | Params                                                                 | Result fields                                          | Description                                    |
|------------------------------|------------------------------------------------------------------------|--------------------------------------------------------|------------------------------------------------|
| `blueprint.create`           | `package_path: string, asset_name: string, parent_class_path: string` | `blueprint_path: string`                               | Creates a new Blueprint asset                  |
| `blueprint.compile`          | `blueprint_path: string`                                               | `success: bool, error_count: int, warning_count: int`  | Compiles a Blueprint asset                     |
| `blueprint.list_variables`   | `blueprint_path: string`                                               | `variables: [{name, type, category}]`                  | Lists NewVariables declared in a Blueprint     |
| `blueprint.list_functions`   | `blueprint_path: string, graph_name?: string`                          | `graphs: [{name, graph_type, node_count, inputs?, outputs?}]` | Graph inventory with function signatures |
| `blueprint.get_graph_nodes`  | `blueprint_path: string, graph_name?: string`                          | `graphs: [{name, type, nodes: [{name, class, title, pos_x, pos_y, pins}]}]` | Full node/pin/link dump of a graph |

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

**blueprint.list_functions example**

`inputs` / `outputs` are only present on graphs that have function entry / result nodes — an
event graph has neither, and an empty array there would read as "takes nothing, returns nothing".
```json
// request
{ "method": "blueprint.list_functions", "params": { "blueprint_path": "/Game/Blueprints/BP_MyActor.BP_MyActor" } }
// result
{
  "graphs": [
    { "name": "EventGraph", "graph_type": "ubergraph", "node_count": 24 },
    { "name": "ApplyDamage", "graph_type": "function", "node_count": 7,
      "inputs": [ { "name": "Amount", "type": "float" } ],
      "outputs": [ { "name": "Killed", "type": "bool" } ] }
  ]
}
```

---

### asset.*

| Method                  | Params                                              | Result fields                                     | Description                                        |
|-------------------------|-----------------------------------------------------|---------------------------------------------------|----------------------------------------------------|
| `asset.list`            | `package_path: string, recursive?: bool` (def. false) | `assets: [{name, class, object_path}]`          | Lists assets under a content package path          |
| `asset.get_metadata`    | `object_path: string`                               | `class: string, tags: {key: value}`               | Returns AssetRegistry metadata for an asset        |
| `asset.save`            | `object_path: string`                               | `saved: bool`                                     | Saves an asset package to disk                     |
| `asset.get_selection`   | —                                                   | `count: int, assets: [{name, class, object_path}]` | Returns the Content Browser selection             |
| `asset.set_selection`   | `object_paths: [string], focus?: bool` (def. true)  | `selected: int, not_found: [string]`              | Selects assets in the Content Browser              |

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

### FBX exchange — `level.*` / `asset.*_fbx`

Round-trips geometry with an external DCC (Maya, via [maya-mcp](https://github.com/YomenStyle/maya-mcp)).
Both programs are assumed to be on the same machine: these methods pass **shared folder paths**,
never binary payloads over the protocol.

| Method                  | Params                                                                                 | Result fields                                                                     | Description                                        |
|-------------------------|----------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------|----------------------------------------------------|
| `level.export_selected` | `path: string, selected_only?: bool` (def. true)`, include_collision?: bool` (def. false)`, ascii?: bool` (def. false) | `path, exists: bool, size_bytes: int, actor_count: int, actors: [string], skipped: [string], actors_truncated: bool` | Exports selected level actors to one FBX           |
| `asset.export_fbx`      | `path: string, object_path?: string, object_paths?: [string], ascii?: bool, include_collision?: bool, include_lods?: bool, include_morph_targets?: bool` (def. true)`, export_preview_mesh?: bool` (def. true) | `exported: [{object_path, path, class, size_bytes, sections, cloth_sections_excluded, warning?, preview_mesh_included?}], count: int, skipped: [{object_path, reason}], from_content_browser_selection: bool` | Exports individual mesh assets or anim sequences, one FBX each |
| `asset.import_fbx`      | `path: string, destination_path: string, replace_existing?: bool` (def. true)`, import_as_skeletal?: bool, skeleton_path?: string, combine_meshes?: bool, generate_lightmap_uvs?: bool` (def. true)`, auto_generate_collision?: bool, save?: bool` (def. true)`, use_interchange?: bool` | `imported: [string], count: int, saved: bool`                                     | Imports an FBX into the content browser            |
| `level.spawn_asset`     | `object_path: string, location?: [x,y,z], rotation?: [pitch,yaw,roll], scale?: [x,y,z], label?: string` | `actor_name: string, class: string, actor_path: string`                           | Places an imported asset in the current level      |
| `level.get_selection`   | —                                                                                      | `count: int, actors: [{name, class, location: [x,y,z]}]`                          | Returns the current level (actor) selection        |

**Read the `warning` field on an export before reporting success.** Unreal's own FBX exporter
drops cloth-simulated sections deliberately — `FFbxExporter::CreateMesh` builds its vertex list
from `FSkeletalMeshLODModel::GetNonClothVertices()` — so a character whose body is cloth-driven
exports as whatever sections are left, with a plausible vertex count and no error. `sections` and
`cloth_sections_excluded` report what happened and `warning` names it. There is no
`UFbxExportOption` that turns this off.

Anim sequences export too, and the same caution applies for a different reason: notifies, sync
markers and custom curves have no FBX representation and are lost, and additive animations come
out as raw poses. `export_preview_mesh` (default true) is what keeps the clip from exporting as a
bare skeleton.

**Selection.** `level.get_selection` and `asset.get_selection` answer different questions and are
not interchangeable: picking a SkeletalMesh in the Content Browser leaves `GetSelectedActors`
empty, which reads as "nothing is selected" if only one of the two is consulted.

**Which exporter to use.** `level.export_selected` writes every mesh component of every
selected actor into a single file — selecting a Blueprint character drags all of its skeletal
meshes along. `asset.export_fbx` is the per-asset counterpart: it takes assets rather than
actors and emits one file each, so a single body or head mesh comes out on its own.

`asset.export_fbx` resolves what to export in this order: `object_path`, then `object_paths`,
then the current Content Browser selection. `path` ending in `.fbx` names one output file and
requires exactly one asset to resolve; anything else is treated as a directory receiving one
`<AssetName>.fbx` per asset. Assets that are neither StaticMesh nor SkeletalMesh land in
`skipped` with a reason instead of failing the whole call.

**level.export_selected example**
```json
// request
{ "method": "level.export_selected", "params": { "path": "D:/exchange/props.fbx", "selected_only": true } }
// result
{
  "path": "D:/exchange/props.fbx", "exists": true, "size_bytes": 128400,
  "actor_count": 2, "actors": ["SM_Wall_01", "SM_Wall_02"],
  "skipped": [], "actors_truncated": false
}
```

`actors` is capped at 200 names; `actor_count` is always the true total and
`actors_truncated` says whether the list was cut. `skipped` lists selected actors carrying no
mesh or brush geometry — Blueprint actors *with* mesh components do export normally.

**asset.export_fbx example**
```json
// request — whatever is selected in the Content Browser, one file each
{ "method": "asset.export_fbx", "params": { "path": "D:/exchange/rita" } }
// result
{
  "exported": [
    { "object_path": "/Game/Char/SK_Rita_Body.SK_Rita_Body", "path": "D:/exchange/rita/SK_Rita_Body.fbx",
      "class": "SkeletalMesh", "size_bytes": 2794528 }
  ],
  "count": 1, "skipped": [], "from_content_browser_selection": true
}
```

**asset.import_fbx example**
```json
// request
{ "method": "asset.import_fbx", "params": {
    "path": "D:/exchange/SM_Prop.fbx", "destination_path": "/Game/Props",
    "auto_generate_collision": false } }
// result
{ "imported": ["/Game/Props/SM_Prop.SM_Prop"], "count": 1, "saved": true }
```

Keep `auto_generate_collision` false when the FBX already carries `UBX_`/`UCX_` collision
authored in the DCC, or Unreal adds its own on top and you end up with both. Materials and
textures are never imported — an Unreal material and a Maya shader are different objects, so
only geometry, names and slot layout cross over.

The engine routes FBX through Interchange by default, which ignores the legacy import flags
above. These methods therefore pin the legacy `UFbxFactory` so the documented options take
effect; pass `use_interchange: true` to fall back to the engine's default routing.

**level.spawn_asset example**
```json
// request
{ "method": "level.spawn_asset", "params": {
    "object_path": "/Game/Props/SM_Prop.SM_Prop", "location": [0, 0, 0], "label": "SM_Prop_01" } }
// result
{ "actor_name": "SM_Prop_01", "class": "StaticMeshActor", "actor_path": "/Game/Maps/Main.Main:PersistentLevel.StaticMeshActor_0" }
```

**Axis and units.** Unreal is Z-up, Maya is Y-up; both default to centimetres, so
`scale_factor` 1.0 is correct. Unreal's FBX exporter normally writes Y-up, matching maya-mcp's
`up_axis="y"` default — if an import lands on its side, retry with `up_axis="z"`.

`InvalidParams` covers a missing file, an empty selection, and a `.fbx` `path` given with
multiple assets; `InternalError` covers an export or import that ran but produced nothing.

---

### skeletalmesh.*

Attachment sockets. Re-exporting a character mesh wipes them, so this is the domain that puts a
rig back together after an art drop rather than a one-off convenience.

| Method                        | Params                                                                                                                                              | Result fields                                | Description                                    |
|-------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------|----------------------------------------------|------------------------------------------------|
| `skeletalmesh.list_sockets`   | `mesh_path: string`                                                                                                                                 | `mesh_sockets: [socket], skeleton_sockets: [socket]` | Lists a mesh's own sockets and its skeleton's |
| `skeletalmesh.add_socket`     | `mesh_path, socket_name, bone_name: string, location?: [x,y,z], rotation?: [pitch,yaw,roll], scale?: [x,y,z], to_skeleton?: bool, replace?: bool, preview_mesh?: string` | `ok: bool, replaced: bool, owner: string, socket: socket` | Creates or overwrites a socket     |
| `skeletalmesh.remove_socket`  | `mesh_path, socket_name: string`                                                                                                                     | `ok: bool, removed_from: "mesh" or "skeleton"` | Removes a socket from whichever list holds it |

A `socket` is `{name, bone, location: [x,y,z], rotation: [pitch,yaw,roll], scale: [x,y,z]}`.
Rotation is in details-panel order (pitch, yaw, roll), not `FRotator`'s constructor order.

**The two socket lists are not interchangeable.** A skeleton socket is shared by every mesh built
on that skeleton; a mesh socket is not. At runtime the mesh is consulted first, so a mesh socket
silently shadows a skeleton socket of the same name. `to_skeleton` picks which list to write to —
the skeleton for a character whose body mesh is swapped, the mesh (the default) otherwise.

`add_socket` verifies the bone exists, because nothing downstream does: attaching to a missing bone
is not a runtime error, the component just sits at the mesh origin.

`preview_mesh` is the editor-only asset shown at the socket. It is stored on the skeleton's
`PreviewAttachedAssetContainer`, not on the socket, which is why it survives deleting and
recreating a socket of the same name — and why it is unreachable from Python.

---

### widget.*

Widget Blueprint authoring: the asset, its widget tree, property bindings, UI components, and
UMG animations. 52 methods; each has a full parameter docstring on its `@mcp.tool()` wrapper in
`Python/unreal_mcp_bridge/tools/widget.py` and `widgetanim.py`.

Everything here routes through the editor's own UMG utilities (`FWidgetBlueprintEditorUtils` and
friends). `UWidgetTree` is reachable from Python, but that route skips the name-collision
handling, the reference retargeting, and the blueprint-structure notifications the editor performs
around every edit — so a tree built that way reads back correctly and then compiles into something
else.

**Asset level** — `widget.create`, `get_info`, `set_property`, `list_blueprint_properties`,
`set_desired_focus`, `compile`, `validate`, `palette_list`.

**Widget tree** — `widget.tree_list`, `get_widget`, `add`, `remove`, `rename`, `move`, `replace`,
`replace_with_child`, `replace_with_named_slot`, `wrap`, `set_is_variable`, `set_root`,
`duplicate`.

**Properties** — `widget.list_properties`, `set_widget_property`, `list_slot_properties`,
`set_slot_property`. Values cross the wire in the editor's own text form (`"(X=10,Y=20)"` for a
struct, an enumerator name for an enum), so a listed value is accepted back verbatim.

**Named slots** — `widget.named_slot_list`, `named_slot_set_content`, `named_slot_clear`. Only
three things host named slots: a nested `UUserWidget`, an `ExpandableArea`, and the widget tree
itself. A `NamedSlot` widget is not one of them — it *declares* a slot for consumers of the
blueprint, and locally its content is an ordinary child, so tree methods apply to it instead.

**Bindings and events** — `widget.binding_list`, `binding_add`, `binding_remove`, `list_events`,
`bind_event`.

**UI components** — `widget.component_add`, `component_remove`, `component_move`.

**Clipboard text** — `widget.export_text`, `import_text`. The same format the designer's
copy/paste produces, which is how a laid-out subtree moves between assets.

**Animation** — `widget.anim_list`, `anim_create`, `anim_remove`, `anim_rename`, `anim_set_range`,
`anim_binding_list`, `anim_bind_widget`, `anim_unbind_widget`, `anim_track_list`, `anim_track_add`,
`anim_track_remove`, `anim_section_add`, `anim_key_add`, `anim_key_list`.

Four things are worth knowing before scripting against this domain.

*A widget must be a variable before the graph or an animation can touch it.* `widget.set_is_variable`
is usually the call right after `widget.add` for anything the game references.

*Slot properties are not widget properties.* Anchors, offsets, size rules and padding live on the
slot object owned by the parent panel, so they are listed and written through the `*_slot_property`
methods. A `widget.move` rebuilds the slot for the new parent's slot class, which discards them.

*A widget animation is a `UMovieScene`, and its pieces stack in one order:*

```
widget.anim_create -> widget.anim_bind_widget -> widget.anim_track_add -> widget.anim_key_add
```

*A possessable GUID means nothing to UMG on its own.* The parallel `FWidgetAnimationBinding` array
is what maps it back to a widget name at runtime, so both halves have to be written together —
`anim_bind_widget` and `anim_unbind_widget` do; a hand-built movie scene edit typically does not,
and the animation then plays over nothing.

Animation times cross the wire in seconds, matching the animation panel; frame numbers come back
alongside them. `bind_slot` on `anim_bind_widget` possesses the widget's slot instead of the widget,
which is how layout is animated — sliding a panel on-screen is a slot binding, fading it is a
widget binding.

---

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

Error code `-32010` is returned when the LiveCoding module is not loaded or cannot be enabled.

---

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

**pie.stop example**
```json
// result
{ "queued": true }
```

### Commands forwarded to BlueprintInspector

These methods call a function of the BlueprintInspector plugin in-process (UObject reflection; no module dependency, no extra port). If the plugin is not loaded they fail with `InternalError` (-32603). Params are the function's parameters in snake_case; the result is `{"value": ...}` where `value` is the function's return value (a JSON string return is parsed when it holds an object or array). `FVector` parameters are `[x, y, z]`.

#### animgraph.*

| Method | Params | BlueprintInspector function |
|--------|--------|-----------------------------|
| `animgraph.add_nested_state_machine` | `asset_path`, `parent_sm_node_name`, `parent_state_name`, `nested_machine_name`, `pos_x`, `pos_y` | `BlueprintInspectorLibrary::AddNestedStateMachine` |
| `animgraph.set_all_transitions_automatic` | `asset_path`, `sm_node_name` | `BlueprintInspectorLibrary::SetAllTransitionsAutomatic` |
| `animgraph.fix_blend_list_by_enum_nodes` | `asset_path`, `enum_path` | `BlueprintInspectorLibrary::FixBlendListByEnumNodes` |
| `animgraph.rebuild_blend_list_enum_entries` | `asset_path`, `enum_path`, `entry_names` | `BlueprintInspectorLibrary::RebuildBlendListEnumEntries` |
| `animgraph.set_blend_list_enum_entries_by_index` | `asset_path`, `graph_index`, `node_index`, `entry_names` | `BlueprintInspectorLibrary::SetBlendListEnumEntriesByIndex` |
| `animgraph.dump_blend_list_enum_info` | `asset_path` | `BlueprintInspectorLibrary::DumpBlendListEnumInfo` |
| `animgraph.list_node_properties` | `asset_path`, `graph_index`, `node_index` | `BlueprintInspectorLibrary::ListAnimGraphNodeProperties` |
| `animgraph.get_node_property` | `asset_path`, `graph_index`, `node_index`, `property_path` | `BlueprintInspectorLibrary::GetAnimGraphNodePropertyByIndex` |
| `animgraph.set_node_property` | `asset_path`, `graph_index`, `node_index`, `property_path`, `value_text` | `BlueprintInspectorLibrary::SetAnimGraphNodePropertyByIndex` |
| `animgraph.dump_node_details` | `asset_path` | `BlueprintInspectorLibrary::DumpAnimGraphNodeDetails` |
| `animgraph.dump_state_machine` | `asset_path`, `sm_node_name` | `BlueprintInspectorLibrary::DumpStateMachineStructure` |
| `animgraph.dump_all_state_machines` | `asset_path` | `BlueprintInspectorLibrary::DumpAllStateMachineStructures` |
| `animgraph.get_settings` | `asset_path` | `BlueprintInspectorLibrary::GetAnimBlueprintSettings` |
| `animgraph.dump_linked_layer_usage` | `asset_path` | `BlueprintInspectorLibrary::DumpLinkedAnimLayerUsage` |

#### animnotify.*

| Method | Params | BlueprintInspector function |
|--------|--------|-----------------------------|
| `animnotify.dump_details` | `asset_path` | `BlueprintInspectorLibrary::DumpNotifyDetails` |
| `animnotify.set_event_timing` | `asset_path`, `notify_index`, `new_trigger_time`, `new_duration` | `BlueprintInspectorLibrary::SetNotifyEventTiming` |

#### asset.*

| Method | Params | BlueprintInspector function |
|--------|--------|-----------------------------|
| `asset.fixup_redirectors` | `content_paths` | `BPIEditorToolset::FixupRedirectors` |

#### behaviortree.*

| Method | Params | BlueprintInspector function |
|--------|--------|-----------------------------|
| `behaviortree.create_blackboard` | `asset_path`, `parent_blackboard_path` | `BPIBehaviorTreeToolset::CreateBlackboard` |
| `behaviortree.add_blackboard_key` | `blackboard_path`, `key_name`, `key_type`, `base_class_or_enum_path`, `instance_synced` | `BPIBehaviorTreeToolset::AddBlackboardKey` |
| `behaviortree.remove_blackboard_key` | `blackboard_path`, `key_name` | `BPIBehaviorTreeToolset::RemoveBlackboardKey` |
| `behaviortree.create` | `asset_path`, `blackboard_path` | `BPIBehaviorTreeToolset::CreateBehaviorTree` |
| `behaviortree.add_node` | `tree_path`, `parent_node_id`, `node_class`, `child_index` | `BPIBehaviorTreeToolset::AddBTNode` |
| `behaviortree.add_aux_node` | `tree_path`, `node_id`, `aux_class` | `BPIBehaviorTreeToolset::AddBTAuxNode` |
| `behaviortree.set_node_property` | `tree_path`, `node_id`, `property_path`, `value_text` | `BPIBehaviorTreeToolset::SetBTNodeProperty` |
| `behaviortree.remove_node` | `tree_path`, `node_id` | `BPIBehaviorTreeToolset::RemoveBTNode` |
| `behaviortree.dump` | `tree_path` | `BPIBehaviorTreeToolset::DumpBehaviorTree` |

#### blendspace.*

| Method | Params | BlueprintInspector function |
|--------|--------|-----------------------------|
| `blendspace.dump_structure` | `asset_path` | `BlueprintInspectorLibrary::DumpBlendSpaceStructure` |
| `blendspace.add_sample` | `asset_path`, `sequence_path`, `x`, `y` | `BlueprintInspectorLibrary::AddBlendSpaceSample` |
| `blendspace.move_sample` | `asset_path`, `sample_index`, `x`, `y` | `BlueprintInspectorLibrary::MoveBlendSpaceSample` |
| `blendspace.replace_sample_animation` | `asset_path`, `sample_index`, `sequence_path` | `BlueprintInspectorLibrary::ReplaceBlendSpaceSampleAnimation` |
| `blendspace.remove_sample` | `asset_path`, `sample_index` | `BlueprintInspectorLibrary::RemoveBlendSpaceSample` |
| `blendspace.set_sample_rate_scale` | `asset_path`, `sample_index`, `rate_scale` | `BlueprintInspectorLibrary::SetBlendSpaceSampleRateScale` |
| `blendspace.set_axis` | `asset_path`, `axis_index`, `axis_name`, `min`, `max`, `grid_num`, `snap_to_grid`, `wrap_input` | `BlueprintInspectorLibrary::SetBlendSpaceAxis` |

#### blueprint.*

| Method | Params | BlueprintInspector function |
|--------|--------|-----------------------------|
| `blueprint.connect_pins` | `asset_path`, `source_node_name`, `source_pin_name`, `target_node_name`, `target_pin_name` | `BlueprintInspectorLibrary::ConnectPins` |
| `blueprint.connect_pins_in_graph` | `asset_path`, `graph_name`, `source_node_name`, `source_pin_name`, `target_node_name`, `target_pin_name` | `BlueprintInspectorLibrary::ConnectPinsInGraph` |
| `blueprint.disconnect_pin` | `asset_path`, `node_name`, `pin_name` | `BlueprintInspectorLibrary::DisconnectPin` |
| `blueprint.set_pin_default_value` | `asset_path`, `node_name`, `pin_name`, `new_value` | `BlueprintInspectorLibrary::SetPinDefaultValue` |
| `blueprint.set_pin_default_value_by_graph_index` | `asset_path`, `graph_index`, `node_index`, `pin_name`, `new_value` | `BlueprintInspectorLibrary::SetPinDefaultValueByGraphIndex` |
| `blueprint.remove_node` | `asset_path`, `node_name` | `BlueprintInspectorLibrary::RemoveNode` |
| `blueprint.add_call_function_node` | `asset_path`, `graph_name`, `function_name`, `pos_x`, `pos_y` | `BlueprintInspectorLibrary::AddCallFunctionNode` |
| `blueprint.add_variable_get_node` | `asset_path`, `graph_name`, `var_name`, `pos_x`, `pos_y` | `BlueprintInspectorLibrary::AddVariableGetNode` |
| `blueprint.add_variable_set_node` | `asset_path`, `graph_name`, `var_name`, `pos_x`, `pos_y` | `BlueprintInspectorLibrary::AddVariableSetNode` |
| `blueprint.add_array_get_item_node` | `asset_path`, `graph_name`, `pos_x`, `pos_y` | `BlueprintInspectorLibrary::AddArrayGetItemNode` |
| `blueprint.reconstruct_node` | `asset_path`, `node_name` | `BlueprintInspectorLibrary::ReconstructNode` |
| `blueprint.reconstruct_node_by_graph_index` | `asset_path`, `graph_index`, `node_index` | `BlueprintInspectorLibrary::ReconstructNodeByGraphIndex` |
| `blueprint.get_graphs` | `asset_path` | `BlueprintInspectorLibrary::GetBlueprintGraphs` |
| `blueprint.get_graph_nodes_by_index` | `asset_path`, `graph_index` | `BlueprintInspectorLibrary::GetGraphNodesByIndex` |
| `blueprint.get_all_nodes` | `asset_path` | `BlueprintInspectorLibrary::GetAllNodesInBlueprint` |
| `blueprint.duplicate_with_remap` | `source_asset_path`, `dest_asset_path`, `new_skeleton`, `asset_substitutions` | `BlueprintInspectorLibrary::DuplicateBlueprintWithRemap` |
| `blueprint.reparent_checked` | `blueprint_path`, `expected_old_parent`, `new_parent` | `BlueprintInspectorLibrary::ReparentBlueprintChecked` |
| `blueprint.get_header` | `asset_path` | `BlueprintInspectorLibrary::GetAnimBlueprintHeader` |
| `blueprint.dump_asset_references` | `asset_path` | `BlueprintInspectorLibrary::DumpAssetReferences` |
| `blueprint.dump_cdo_references` | `asset_path` | `BlueprintInspectorLibrary::DumpCDOReferences` |
| `blueprint.dump_function_calls` | `asset_path` | `BlueprintInspectorLibrary::DumpFunctionCalls` |
| `blueprint.dump_variable_usages` | `asset_path` | `BlueprintInspectorLibrary::DumpVariableUsages` |
| `blueprint.dump_event_entries` | `asset_path` | `BlueprintInspectorLibrary::DumpEventEntries` |
| `blueprint.dump_function_signatures` | `asset_path` | `BlueprintInspectorLibrary::DumpFunctionSignatures` |
| `blueprint.dump_mpc_bindings` | `blueprint_path`, `collection_path` | `BlueprintInspectorLibrary::DumpBlueprintMPCBindingsJson` |
| `blueprint.remap_mpc_call_checked` | `blueprint_path`, `graph_name`, `node_guid`, `collection_path`, `expected_source_name`, `retained_target_name`, `expected_shared_guid` | `BlueprintInspectorLibrary::RemapMPCBlueprintCallChecked` |
| `blueprint.create_interface` | `asset_path` | `BPIInterfaceToolset::CreateBlueprintInterface` |
| `blueprint.add_interface_function` | `interface_path`, `function_name`, `inputs`, `outputs` | `BPIInterfaceToolset::AddInterfaceFunction` |
| `blueprint.implement_interface` | `blueprint_path`, `interface_path` | `BPIInterfaceToolset::ImplementInterface` |
| `blueprint.remove_interface` | `blueprint_path`, `interface_path`, `preserve_functions` | `BPIInterfaceToolset::RemoveInterface` |
| `blueprint.list_implemented_interfaces` | `blueprint_path` | `BPIInterfaceToolset::ListImplementedInterfaces` |

#### editor.*

| Method | Params | BlueprintInspector function |
|--------|--------|-----------------------------|
| `editor.console_exec` | `command` | `BPIEditorToolset::ExecuteConsoleCommand` |

#### material.*

| Method | Params | BlueprintInspector function |
|--------|--------|-----------------------------|
| `material.dump_property_connections` | `asset_path` | `BlueprintInspectorLibrary::DumpMaterialPropertyConnections` |
| `material.find_using_collection` | `collection_path` | `BlueprintInspectorLibrary::FindMaterialsUsingCollection` |
| `material.set_root_node_position` | `asset_path`, `pos_x`, `pos_y` | `BlueprintInspectorLibrary::SetMaterialRootNodePosition` |
| `material.set_custom_data_constant` | `asset_path`, `custom_data_index`, `value` | `BlueprintInspectorLibrary::SetMaterialCustomDataConstant` |
| `material.get_custom_data_info` | `asset_path`, `custom_data_index` | `BlueprintInspectorLibrary::GetMaterialCustomDataInfo` |
| `material.dump_function_contract` | `asset_path` | `BlueprintInspectorLibrary::DumpMaterialFunctionContractJson` |
| `material.dump_function_caller_contract` | `caller_path`, `function_paths` | `BlueprintInspectorLibrary::DumpMaterialFunctionCallerContractJson` |
| `material.dump_mpc_contract` | `collection_path` | `BlueprintInspectorLibrary::DumpMPCContractJson` |
| `material.dump_mpc_bindings` | `asset_path`, `collection_path` | `BlueprintInspectorLibrary::DumpMaterialMPCBindingsJson` |
| `material.merge_parameter_collection_checked` | `target_path`, `source_path`, `expected_target_contract_json` | `BlueprintInspectorLibrary::MergeMaterialParameterCollectionAdditiveChecked` |
| `material.reconnect_function_call_checked` | `caller_path`, `node_id`, `expected_function_path`, `expected_function_contract_json`, `reconnect_spec_json` | `BlueprintInspectorLibrary::ReconnectMaterialFunctionCallChecked` |

#### montage.*

| Method | Params | BlueprintInspector function |
|--------|--------|-----------------------------|
| `montage.dump_structure` | `asset_path` | `BlueprintInspectorLibrary::DumpMontageStructure` |
| `montage.create_from_sequence` | `sequence_path`, `dest_asset_path`, `slot_name`, `first_section_name` | `BlueprintInspectorLibrary::CreateMontageFromSequence` |
| `montage.add_slot_track` | `asset_path`, `slot_name` | `BlueprintInspectorLibrary::AddMontageSlotTrack` |
| `montage.set_slot_track_name` | `asset_path`, `track_index`, `slot_name` | `BlueprintInspectorLibrary::SetMontageSlotTrackName` |
| `montage.set_section_next` | `asset_path`, `section_name`, `next_section_name` | `BlueprintInspectorLibrary::SetMontageSectionNext` |

#### niagara.*

| Method | Params | BlueprintInspector function |
|--------|--------|-----------------------------|
| `niagara.inspect_function_calls` | `system_asset_path`, `emitter_name` | `BlueprintInspectorLibrary::InspectNiagaraFunctionCalls` |
| `niagara.inspect_module_graph` | `module_script_path` | `BlueprintInspectorLibrary::InspectNiagaraModuleGraph` |
| `niagara.script_create` | `asset_path`, `usage` | `BPINiagaraScriptToolset::CreateNiagaraScript` |
| `niagara.script_set_metadata` | `script_path`, `description`, `category`, `library_visibility`, `module_usage_bitmask`, `keywords` | `BPINiagaraScriptToolset::SetScriptMetadata` |
| `niagara.script_compile` | `script_path`, `force`, `refresh_referencers` | `BPINiagaraScriptToolset::CompileNiagaraScript` |
| `niagara.script_dump_graph` | `script_path` | `BPINiagaraScriptToolset::DumpScriptGraph` |
| `niagara.script_add_input` | `script_path`, `name`, `type_`, `default_value`, `map_get_node_id` | `BPINiagaraScriptToolset::AddScriptInput` |
| `niagara.script_remove_input` | `script_path`, `name` | `BPINiagaraScriptToolset::RemoveScriptInput` |
| `niagara.script_set_input_default` | `script_path`, `name`, `default_value` | `BPINiagaraScriptToolset::SetScriptInputDefault` |
| `niagara.script_add_map_get_pin` | `script_path`, `node_id`, `parameter_name`, `type_` | `BPINiagaraScriptToolset::AddMapGetPin` |
| `niagara.script_add_map_set_pin` | `script_path`, `node_id`, `parameter_name`, `type_` | `BPINiagaraScriptToolset::AddMapSetPin` |
| `niagara.script_add_node` | `script_path`, `node_kind`, `arg`, `pos_x`, `pos_y` | `BPINiagaraScriptToolset::AddNode` |
| `niagara.script_remove_node` | `script_path`, `node_id` | `BPINiagaraScriptToolset::RemoveNode` |
| `niagara.script_set_custom_hlsl` | `script_path`, `node_id`, `code` | `BPINiagaraScriptToolset::SetCustomHlsl` |
| `niagara.script_add_custom_hlsl_pin` | `script_path`, `node_id`, `name`, `type_`, `is_output` | `BPINiagaraScriptToolset::AddCustomHlslPin` |
| `niagara.script_remove_custom_hlsl_pin` | `script_path`, `node_id`, `pin` | `BPINiagaraScriptToolset::RemoveCustomHlslPin` |
| `niagara.script_connect_pins` | `script_path`, `from_node_id`, `from_pin`, `to_node_id`, `to_pin` | `BPINiagaraScriptToolset::ConnectPins` |
| `niagara.script_disconnect_pin` | `script_path`, `node_id`, `pin`, `other_node_id`, `other_pin` | `BPINiagaraScriptToolset::DisconnectPin` |
| `niagara.script_set_pin_default` | `script_path`, `node_id`, `pin`, `value_text` | `BPINiagaraScriptToolset::SetPinDefault` |
| `niagara.script_create_scratch_pad` | `owner_path`, `name`, `usage`, `target_stack_usage` | `BPINiagaraScriptToolset::CreateScratchPadScript` |
| `niagara.script_list_scratch_pad` | `owner_path` | `BPINiagaraScriptToolset::ListScratchPadScripts` |

#### skeletalmesh.*

| Method | Params | BlueprintInspector function |
|--------|--------|-----------------------------|
| `skeletalmesh.dump_skeleton` | `asset_path` | `BlueprintInspectorLibrary::DumpSkeletonStructure` |

#### statetree.*

| Method | Params | BlueprintInspector function |
|--------|--------|-----------------------------|
| `statetree.list_binding_sources` | `tree_path`, `target_node_id` | `BPIStateTreeToolset::ListStateTreeBindingSources` |
| `statetree.unbind_property` | `tree_path`, `target_node_id`, `target_property_path` | `BPIStateTreeToolset::UnbindStateTreeProperty` |
| `statetree.add_parameter` | `tree_path`, `name`, `type_` | `BPIStateTreeToolset::AddStateTreeParameter` |

#### widget.*

| Method | Params | BlueprintInspector function |
|--------|--------|-----------------------------|
| `widget.mvvm_set_binding_mode` | `widget_path`, `binding_id`, `mode` | `BPIMVVMToolset::SetBindingMode` |
| `widget.mvvm_set_binding_execution_mode` | `widget_path`, `binding_id`, `execution_mode` | `BPIMVVMToolset::SetBindingExecutionMode` |
| `widget.mvvm_set_binding_enabled` | `widget_path`, `binding_id`, `enabled`, `compile` | `BPIMVVMToolset::SetBindingEnabled` |
| `widget.mvvm_update_binding_paths` | `widget_path`, `binding_id`, `source_path`, `destination_path` | `BPIMVVMToolset::UpdateBindingPaths` |
| `widget.mvvm_set_conversion_function_argument` | `widget_path`, `binding_id`, `argument_name`, `value_or_path`, `source_to_destination` | `BPIMVVMToolset::SetConversionFunctionArgument` |
| `widget.mvvm_remove_view_model` | `widget_path`, `view_model_name`, `remove_referencing_bindings` | `BPIMVVMToolset::RemoveViewModelFromWidget` |
| `widget.mvvm_set_view_model_creation_type` | `widget_path`, `view_model_name`, `creation_type`, `argument` | `BPIMVVMToolset::SetViewModelCreationType` |
| `widget.mvvm_set_view_model_options` | `widget_path`, `view_model_name`, `expose_instance_in_editor`, `create_setter_function`, `optional` | `BPIMVVMToolset::SetViewModelOptions` |
| `widget.mvvm_rename_view_model` | `widget_path`, `view_model_name`, `new_name` | `BPIMVVMToolset::RenameViewModel` |
| `widget.mvvm_remove_view_model_property` | `view_model_path`, `property_name` | `BPIMVVMToolset::RemoveViewModelProperty` |
| `widget.mvvm_set_view_model_property_default` | `view_model_path`, `property_name`, `value_text` | `BPIMVVMToolset::SetViewModelPropertyDefault` |
| `widget.mvvm_add_view_event` | `widget_path`, `event_path`, `destination_function_path` | `BPIMVVMToolset::AddViewEvent` |
| `widget.mvvm_set_view_event_argument` | `widget_path`, `event_id`, `argument_name`, `value_or_path` | `BPIMVVMToolset::SetViewEventArgument` |
| `widget.mvvm_remove_view_event` | `widget_path`, `event_id` | `BPIMVVMToolset::RemoveViewEvent` |
| `widget.mvvm_add_view_condition` | `widget_path`, `condition_path`, `operation`, `value`, `max_value`, `destination_function_path` | `BPIMVVMToolset::AddViewCondition` |
| `widget.mvvm_remove_view_condition` | `widget_path`, `condition_id` | `BPIMVVMToolset::RemoveViewCondition` |
| `widget.mvvm_dump` | `widget_path` | `BPIMVVMToolset::DumpWidgetMVVM` |
| `widget.mvvm_compile` | `widget_path` | `BPIMVVMToolset::CompileWidgetMVVM` |
