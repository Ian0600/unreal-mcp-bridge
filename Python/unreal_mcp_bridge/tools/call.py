from __future__ import annotations

from typing import Any

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_call_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def unreal_call(
        method: str,
        params: dict[str, Any] | None = None,
        timeout: float | None = None,
    ) -> dict:
        """Calls any UnrealMCPBridge JSON-RPC method directly, passing params through unchanged.

        Use it for plugin commands that have no dedicated tool here yet — the plugin gains commands
        faster than this server gets wrappers. Method names are the plugin's dotted names
        (e.g. "sequencer.list_bindings", "skeletalmesh.add_socket"); the parameter contract is the
        comment above each Registry.Register in Plugins/UnrealMCPBridge/.../Commands/*.cpp.

        timeout (seconds) overrides the client-side wait for this call only. The plugin still gives
        up on its side after GameThreadDispatchTimeoutSeconds (30 by default).

        Returns the method's result object; a plugin error is raised with its code, message and data."""
        if not method:
            raise ValueError("method is required")
        return conn.call(method, params if params is not None else {}, timeout=timeout)
