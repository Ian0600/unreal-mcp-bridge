# unreal-mcp-bridge

JSON-RPC 2.0 over TCP bridge between Unreal Editor and MCP (Model Context Protocol) clients.

## Architecture

```
MCP Client (Claude, etc.)
        |  stdio
  Python MCP Server (FastMCP)
        |  TCP JSON-RPC 2.0
  UE Editor Plugin (UnrealMCPBridge)
        |  GameThread dispatch
  UE Editor APIs
```

- The UE plugin hosts a TCP listener in a dedicated worker thread.
- Each newline-delimited JSON-RPC request is dispatched to the GameThread for safe Editor API access.
- The Python server wraps that TCP connection as MCP tools via FastMCP stdio transport.

## What It Covers

308 editor methods, each exposed as exactly one MCP tool. The point of most of them is not
convenience — it is that the equivalent edit is either unreachable from Unreal's Python API or
quietly wrong when done there, because the Python route bypasses the editor utilities that keep
an asset's parallel structures in agreement.

| Domain            | Methods | What it authors                                                              |
|-------------------|---------|------------------------------------------------------------------------------|
| `widget.*`        | 70      | Widget Blueprints: tree, slots, property bindings, UI components, UMG animation |
| `material.*`      | 53      | Material and material-function graphs, instances, comments, named reroutes    |
| `niagara.*`       | 37      | Niagara systems: emitters, modules, module inputs, renderers                   |
| `statetree.*`     | 15      | StateTree states, tasks, conditions, evaluators, transitions, bindings         |
| `animgraph.*`     | 24      | AnimBlueprint graphs: state machines, states, transitions, players, slots      |
| `asset.*`         | 8       | Asset listing and metadata, saving, FBX import/export, Content Browser selection |
| `montage.*`       | 11      | AnimMontage segments and sections                                              |
| `blueprint.*`     | 36      | Blueprint assets, variables, function graphs                                   |
| `animnotify.*`    | 7       | Anim notifies and notify states: place, retime, reconfigure, remove            |
| `editor.*`        | 5       | Editor status, actor listing and spawning, Python exec                         |
| `camvar.*`        | 4       | Camera variable collections                                                    |
| `skeletalmesh.*`  | 4       | Attachment sockets on a mesh or its skeleton, with preview assets              |
| `camerarig.*`     | 8       | Camera rig assets: node tree, wiring, node add/remove, exposed parameter defaults |
| `level.*`         | 3       | Level FBX export, placing assets in the level, the level selection             |
| `pie.*`           | 2       | Play-In-Editor start/stop                                                      |
| `compile.*`       | 2       | Live Coding trigger and status                                                 |
| `object.*`        | 3       | Any UObject's properties by reflection - the escape hatch for what no domain handler covers |
| `behaviortree.*`  | 9       | Behavior Trees and Blackboards: create, nodes, aux nodes, properties, keys (forwarded to BlueprintInspector) |
| `blendspace.*`    | 7       | BlendSpace samples and axes, with the resample step the Python route skips (forwarded to BlueprintInspector) |

A few examples of the difference this makes:

- **Any property at all.** The editor's Python API only wraps what the engine chose to expose, so
  private and protected `UPROPERTY`s and whole `UCLASS(MinimalAPI)` types answer "failed to find
  property" or "is protected and cannot be read"; the console `set` command is refused outright in
  the editor. `object.*` reads and writes them by reflection, which ignores C++ access specifiers.

- **Anim notifies.** `UAnimSequenceBase::Notifies` is script-protected, so a montage's hit window
  cannot be authored from Python at all.
- **Montage segments.** A montage's length is derived from its segments and `sequence_length` is
  read-only from Python, so a Python-built multi-segment montage keeps the length of its first
  segment and everything after it is cut off mid-playback.
- **Material attribute pins.** The pin array of a Get/Set Material Attributes node is resized from
  a `PreEditChange` snapshot that scripted property writes never fill; write the arrays directly
  and the asset asserts on load from then on.
- **Widget trees.** Renaming a widget from Python leaves every graph node pointing at a name that
  no longer exists, and the blueprint only says so at the next compile.
- **UMG animation bindings.** A movie scene possessable GUID is meaningless to UMG on its own —
  the parallel `FWidgetAnimationBinding` array is what maps it back to a widget, and both halves
  have to be written together or the animation plays over nothing.

### Commands forwarded to BlueprintInspector

124 of those methods are thin forwards to the **BlueprintInspector** plugin's functions (Blueprint graph editing, AnimBP / BlendSpace / Montage authoring, Behavior Trees, MVVM, Niagara script graphs, material contracts and MPC migration). They are added to the domain they belong to (`blueprint.*`, `animgraph.*`, `niagara.*`, ...), plus the new `behaviortree.*` and `blendspace.*`.

BlueprintInspector is optional. The bridge reaches it through UObject reflection, so there is no build or load dependency; until the plugin is loaded those methods answer with an `InternalError` saying so, and everything else works unchanged. The transport is not involved — these are ordinary registry commands, one MCP tool each.

See [docs/PROTOCOL.md](docs/PROTOCOL.md) for the method catalogue. Each method's full parameter
list lives in the docstring of its `@mcp.tool()` wrapper under `Python/unreal_mcp_bridge/tools/`,
which is also what an MCP client reads.

## Requirements

- Unreal Engine 5.8 (what the plugin is developed and tested against)
- Python 3.10+
- A running Unreal Editor instance with the plugin enabled

Earlier engine versions are not supported by the full command set. The hard floors are
UE 5.5 for `camerarig.*` (the Gameplay Camera System) and UE 5.6 for `widget.*`
(`FWidgetBlueprintOperationUtils`, which is what makes headless widget tree editing possible);
some movie scene calls behind `widget.anim_*` follow 5.8 signatures. Dropping the corresponding
command `.cpp` files and their `Build.cs` dependencies is enough to build the rest further back.

## UE Plugin Installation

1. Copy (or symlink) `Plugin/UnrealMCPBridge` into your project's `Plugins/` folder:
   ```
   <YourProject>/Plugins/UnrealMCPBridge/
   ```
2. Regenerate project files and build.
3. Enable the plugin in **Edit → Plugins → Editor → Unreal MCP Bridge**.
4. Configure the listener in **Edit → Project Settings → Plugins → Unreal MCP Bridge**:
   - **Enabled**: `true` — **required**. The bridge is **disabled by default**; the
     listener will not start until you turn this on.
   - **Port**: e.g. `30100` (must be > 0 to start the listener)
   - **Additional Ports**: extra ports to listen on, one entry per additional MCP client
   - **Host**: `127.0.0.1` (loopback only recommended)

> **You must set `Enabled = true` (and a valid `Port`) to use the bridge.**
> While disabled it does nothing and MCP clients get *connection refused*.

Changes apply **immediately** — toggling **Enabled** or changing the **Port** in
Project Settings starts/restarts the listener on the spot, **no editor restart
required**. (The listener also auto-starts on editor launch whenever it is enabled.)

The corresponding `DefaultEngine.ini` section is:
```ini
[/Script/UnrealMCPBridge.MCPSettings]
bEnabled=True
Port=30100
+AdditionalPorts=30101
```

### Several clients at once

A listener serves every connection on its own thread, so any number of MCP clients (Claude
Code and Codex, say) can use the same port at the same time. Requests from all clients still
run one at a time on the GameThread. The Python server does not hold a connection open: each
tool call connects, sends its request, reads the reply and disconnects, so an idle server
occupies nothing. Set `UMCP_PERSISTENT=true` to go back to one reused connection.

`AdditionalPorts` still starts extra listener threads on other ports, sharing the same command
registry; it is no longer needed for multiple clients. Duplicate and non-positive entries are
ignored.

## Python Package Installation

```bash
cd Python
pip install -e .
```

Or from PyPI once published:
```bash
pip install unreal-mcp-bridge
```

## Environment Variables

All settings are prefixed `UMCP_` and can be placed in a `.env` file next to the server or exported in the shell:

| Variable               | Default       | Description                          |
|------------------------|---------------|--------------------------------------|
| `UMCP_HOST`            | `127.0.0.1`   | UE plugin listener host              |
| `UMCP_PORT`            | `30100`        | UE plugin listener port              |
| `UMCP_CONNECT_TIMEOUT` | `5.0`          | TCP connect timeout (seconds)        |
| `UMCP_REQUEST_TIMEOUT` | `30.0`         | Per-request response timeout (sec)   |
| `UMCP_MAX_LINE_BYTES`  | `16777216`     | Maximum JSON line size in bytes      |
| `UMCP_PERSISTENT`      | `false`        | Keep one connection open instead of connecting per call |

Example `.env`:
```
UMCP_HOST=127.0.0.1
UMCP_PORT=30100
```

## Running the MCP Server

```bash
unreal-mcp-bridge
```

Or directly:
```bash
python -m unreal_mcp_bridge.server
```

The server connects to the running UE Editor and exposes MCP tools over stdio for any MCP-compatible client.

## Claude Desktop Integration

Add to `claude_desktop_config.json`:
```json
{
  "mcpServers": {
    "unreal": {
      "command": "unreal-mcp-bridge",
      "env": {
        "UMCP_PORT": "30100"
      }
    }
  }
}
```

## Protocol

See [docs/PROTOCOL.md](docs/PROTOCOL.md) for the full JSON-RPC 2.0 message specification and method catalogue.

## License

MIT — see [LICENSE](LICENSE).
