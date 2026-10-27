from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_pie_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def pie_start(mode: str = "PIE") -> dict:
        return conn.call("pie.start", {"mode": mode})

    @mcp.tool()
    def pie_stop() -> dict:
        return conn.call("pie.stop")

    @mcp.tool()
    def pie_exec_console(command: str, world: str = "", player: int = 0) -> dict:
        """Runs a console command and returns what it printed plus the log lines it caused.

        world: "pie" = through the PIE player's controller, so Exec cheats and the cheat manager answer
        (ifsr.Quest.Give, ifsr.Quest.CompleteObjective, ifsr.Quest.Dump, SpawnMonster, ...); "editor" =
        the editor world. Default: pie while PIE runs, else editor. player: PIE player index.
        Note: IFSR's CVar cheats also write Config/DefaultDebug.ini when toggled.

        Returns {world, command, output, log_lines: [{seq, category, verbosity, text}], log_cursor}
        (log_cursor = since for the next pie_read_log)."""
        params: dict = {"command": command, "player": player}
        if world:
            params["world"] = world
        return conn.call("pie.exec_console", params, timeout=120.0)

    @mcp.tool()
    def pie_read_log(
        since: int | None = None,
        contains: str = "",
        category: str = "",
        verbosity: str = "",
        max: int = 200,
    ) -> dict:
        """Read-only. The editor log as captured by the bridge since the editor started (last 20000 lines).

        since: cursor from a previous call (next) or from pie_exec_console (log_cursor); omit for the last
        max lines. contains: substring. category: exact (e.g. LogIFSRQuest). verbosity: most verbose
        level kept — Error, Warning, Display, Log or Verbose.

        Returns {lines: [{seq, category, verbosity, text}], next, dropped}."""
        params: dict = {"max": max}
        if since is not None:
            params["since"] = since
        if contains:
            params["contains"] = contains
        if category:
            params["category"] = category
        if verbosity:
            params["verbosity"] = verbosity
        return conn.call("pie.read_log", params)

    @mcp.tool()
    def run_automation_test(filter: str, max_tests: int = 50, latent_seconds: float = 5.0) -> dict:
        """Runs registered automation tests whose full name starts with filter (e.g. "IFSR.Quest",
        "IFSR.Quest.Validation.ProjectData") and returns their results in the same call.

        Tests that wait on frames (latent commands) cannot finish inside one call and come back with
        latent_timeout after latent_seconds; run those from the Session Frontend.

        Returns {tests: [{name, ok, errors, warnings, seconds, messages: [{type, message}]}], passed, failed}."""
        return conn.call("automation.run_test", {
            "filter": filter,
            "max_tests": max_tests,
            "latent_seconds": latent_seconds,
        }, timeout=300.0)
