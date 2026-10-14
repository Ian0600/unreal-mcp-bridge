from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_editor_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def editor_get_status() -> dict:
        return conn.call("editor.get_status")

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

    @mcp.tool()
    def editor_console_exec(command: str) -> dict:
        """Execute an editor console command (e.g. "stat fps", "obj list class=Blueprint", CVar set/get).
        command: Console command line.
        Returns: JSON: {"success":bool,"output":string} with the text the command printed.

        Built-in port of BlueprintInspector's BPIEditorToolset::ExecuteConsoleCommand; no other plugin needed."""
        return conn.call("editor.console_exec", {"command": command})
