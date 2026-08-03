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
