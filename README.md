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

## Requirements

- Unreal Engine 5.3+
- Python 3.10+
- A running Unreal Editor instance with the plugin enabled

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
   - **Host**: `127.0.0.1` (loopback only recommended)

> **You must set `Enabled = true` (and a valid `Port`) to use the bridge.**
> While disabled it does nothing and MCP clients get *connection refused*.

Changes apply **immediately** — toggling **Enabled** or changing the **Port** in
Project Settings starts/restarts the listener on the spot, **no editor restart
required**. (The listener also auto-starts on editor launch whenever it is enabled.)

The settings live in the plugin's own config branch, not in `DefaultEngine.ini`.
The default layer ships with the plugin, disabled:

```ini
; Plugin/UnrealMCPBridge/Config/DefaultUnrealMCPBridge.ini
[/Script/UnrealMCPBridge.MCPSettings]
bEnabled=False
Host=127.0.0.1
Port=0
```

Turn it on either in Project Settings, or by editing one of the layers below —
for example `bEnabled=True` and `Port=30100`.

Layers, lowest priority first:

| Layer | Path | Written by |
|---|---|---|
| Plugin default | `Plugins/UnrealMCPBridge/Config/DefaultUnrealMCPBridge.ini` | by hand |
| Project override (optional) | `<Project>/Config/DefaultUnrealMCPBridge.ini` | by hand |
| Per user | `<Project>/Saved/Config/<Platform>/UnrealMCPBridge.ini` | Project Settings |

Editing a value in Project Settings writes it to the `Saved` file; values equal to
the plugin default are not written at all. The engine never writes back into the
plugin folder.

> Keep `Config/DefaultUnrealMCPBridge.ini` in the plugin. A plugin config branch
> with no static layer is dropped during load, after which the settings fall back
> to a no-save config file and changes silently stop persisting.

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
| `UMCP_MAX_LINE_BYTES`  | `65536`        | Maximum JSON line size in bytes      |

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
