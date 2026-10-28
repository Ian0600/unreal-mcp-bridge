from __future__ import annotations

import re
from pathlib import Path

from fastmcp import FastMCP

from .. import __version__
from ..connection import UnrealConnection


def _wrapped_methods() -> set[str]:
    """Plugin methods some tool in this package calls by name (conn.call("x.y", ...))."""
    pattern = re.compile(r'conn\.call\(\s*"([a-z_]+\.[a-z_]+)"')
    methods: set[str] = set()
    for source in Path(__file__).parent.glob("*.py"):
        methods.update(pattern.findall(source.read_text(encoding="utf-8")))
    return methods


def register_editor_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def editor_get_status() -> dict:
        """Current level, actor counts, whether PIE runs, and whether this MCP server is current.

        Returns the plugin's fields plus server_version, server_path (the folder this server runs
        from - it must be the workspace's unreal-mcp-bridge/Python, installed with pip install -e)
        and server_missing: plugin commands no tool of this server wraps. A long server_missing list
        (pie.exec_console, pie.status, ...) means the server is an old copy: point pip at the workspace
        folder and restart the Claude session. A few entries are normal (call them with unreal_call).
        The full command list is left out to keep the reply short (command_count stays)."""
        status = conn.call("editor.get_status")
        commands = status.pop("commands", None)
        status["server_version"] = __version__
        status["server_path"] = str(Path(__file__).resolve().parents[2])
        if commands is not None:
            status["server_missing"] = sorted(set(commands) - _wrapped_methods())
        return status

    @mcp.tool()
    def editor_list_actors(
        filter_class: str | None = None,
        max_count: int = 1000,
    ) -> dict:
        params: dict = {"max_count": max_count}
        if filter_class:
            params["filter_class"] = filter_class
        return conn.call("editor.list_actors", params)

    @mcp.tool()
    def editor_spawn_actor(
        class_path: str,
        location: list[float] | None = None,
        rotation: list[float] | None = None,
    ) -> dict:
        if not class_path:
            raise ValueError("class_path is required")
        params: dict = {"class_path": class_path}
        if location is not None:
            params["location"] = location
        if rotation is not None:
            params["rotation"] = rotation
        return conn.call("editor.spawn_actor", params)

    @mcp.tool()
    def editor_python_exec(script: str, mode: str = "ExecuteFile") -> dict:
        """Run arbitrary Python inside the running Unreal Editor.

        mode: "ExecuteFile" (default — multiple statements), "ExecuteStatement",
              or "EvaluateStatement" (returns expression result).
        Returns: {success, result, log[{type, output}]}.
        """
        if not script:
            raise ValueError("script is required")
        return conn.call("editor.python_exec", {"script": script, "mode": mode})
