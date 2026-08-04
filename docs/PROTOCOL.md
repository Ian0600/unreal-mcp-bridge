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

Example usage: `-32000` for "Editor not ready", `-32001` for "Asset not found".

## Method Catalogue

The tables below cover the core domains in detail. The bridge also serves `animgraph.*`,
`animnotify.*`, `statetree.*`, `camerarig.*`, `camvar.*`, `material.*` and `niagara.*`, which are
documented by the docstrings on their `@mcp.tool()` wrappers in
`Python/unreal_mcp_bridge/tools/`. The authoritative list of methods is the set of
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
| `asset.export_fbx`      | `path: string, object_path?: string, object_paths?: [string], ascii?: bool, include_collision?: bool, include_lods?: bool, include_morph_targets?: bool` (def. true) | `exported: [{object_path, path, class, size_bytes}], count: int, skipped: [{object_path, reason}], from_content_browser_selection: bool` | Exports individual mesh assets, one FBX each       |
| `asset.import_fbx`      | `path: string, destination_path: string, replace_existing?: bool` (def. true)`, import_as_skeletal?: bool, skeleton_path?: string, combine_meshes?: bool, generate_lightmap_uvs?: bool` (def. true)`, auto_generate_collision?: bool, save?: bool` (def. true)`, use_interchange?: bool` | `imported: [string], count: int, saved: bool`                                     | Imports an FBX into the content browser            |
| `level.spawn_asset`     | `object_path: string, location?: [x,y,z], rotation?: [pitch,yaw,roll], scale?: [x,y,z], label?: string` | `actor_name: string, class: string, actor_path: string`                           | Places an imported asset in the current level      |

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
