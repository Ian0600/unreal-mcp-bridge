from __future__ import annotations

import json
import time

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_pie_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def pie_start(mode: str = "PIE", wait_ready: bool = False, timeout: float = 90.0) -> dict:
        """Starts a play session. The plugin only queues it for the next tick.

        wait_ready: poll pie_status until the world has begun play and player 0 has a pawn (or until
        timeout seconds), and return that status as "status". Without it the call returns at once and
        the world may not exist yet. World Partition cells near the player may still be loading
        when ready turns true - player teleports should check the ground themselves."""
        result = conn.call("pie.start", {"mode": mode})
        if not wait_ready:
            return result
        deadline = time.monotonic() + timeout
        status: dict = {}
        while time.monotonic() < deadline:
            status = conn.call("pie.status")
            if status.get("ready"):
                break
            time.sleep(0.5)
        result["status"] = status
        result["ready"] = bool(status.get("ready"))
        result["waited_seconds"] = round(timeout - max(0.0, deadline - time.monotonic()), 2)
        return result

    @mcp.tool()
    def pie_status(player: int = 0) -> dict:
        """Read-only. Whether PIE runs and is ready to drive (begun play + player pawn).

        Returns {running, world, map, begun_play, game_seconds, real_seconds, paused, time_dilation,
        has_controller, controller_class, pawn, pawn_class, pawn_location, ready}."""
        return conn.call("pie.status", {"player": player})

    @mcp.tool()
    def events_poll(since: int | None = None, names: list[str] | None = None, max: int = 500, since_mark: str = "") -> dict:
        """Read-only. Game events as structured values instead of log lines.

        The game writes each event as one JSON line in the LogIFSREvent category (IFSR: turn them on
        with pie_exec_console("ifsr.Debug.Events 1"); ifsr.Motherlord.Telegraph 1 / ifsr.Combat.LogHits 1
        also emit their own). Events: Motherlord.Telegraph {pattern, tentacle, shape, point, size,
        impact_at, lead}, Motherlord.PatternStart / PatternEnd {pattern, last, stage}, Motherlord.Stage
        {from, to, core_hp, core_pct, finale_left}, Combat.Hit {victim, player, source, pattern,
        tentacle_hit, damage, applied, location, from}, Npc.TargetChanged {companion, from, to, reason,
        core_open}. Every event has "event" and "t" (game seconds).

        since: cursor from the previous call's next (omit = the latest max events); since_mark: a
        pie_log_mark name. names: keep only events whose name starts with one of these.

        Returns {events: [...], next, dropped}."""
        params: dict = {"category": "LogIFSREvent", "max": max}
        if since is not None:
            params["since"] = since
        if since_mark:
            params["since_mark"] = since_mark
        raw = conn.call("pie.read_log", params)
        events = []
        for line in raw.get("lines", []):
            try:
                event = json.loads(line["text"])
            except (ValueError, KeyError):
                continue
            if names and not any(str(event.get("event", "")).startswith(n) for n in names):
                continue
            event["seq"] = line.get("seq")
            events.append(event)
        return {"events": events, "next": raw.get("next"), "dropped": raw.get("dropped", 0)}

    @mcp.tool()
    def pie_log_mark(name: str) -> dict:
        """Writes "MCP mark: <name>" to the log and remembers where it is, so pie_read_log(since_mark=name)
        later returns everything logged after it (e.g. one test round). Reusing a name moves the mark.

        Returns {name, cursor}."""
        return conn.call("pie.log_mark", {"name": name})

    @mcp.tool()
    def debug_line_of_sight(
        start: list[float],
        end: list[float],
        channel: str = "Visibility",
        world: str = "",
        complex: bool = False,
        ignore: list[str] | None = None,
        draw_seconds: float = 3.0,
        ignore_player: bool | None = None,
    ) -> dict:
        """Read-only (draws a debug line). Traces start -> end and reports the first thing that blocks it.

        channel: Visibility (default), Camera, ECC_* or a project trace channel name. world: pie
        (default while PIE runs) or editor. ignore: actor names/labels to skip. ignore_player:
        skip player 0's pawn (default true in PIE). draw_seconds: green line to the hit, red past
        it; 0 = no drawing.

        Returns {world, channel, blocked, hit: {actor, actor_class, actor_path, component,
        component_class, collision_profile, response, bone, location, distance} | null,
        touches: [...overlaps before the hit], length}."""
        params: dict = {"from": start, "to": end, "channel": channel, "complex": complex, "draw_seconds": draw_seconds}
        if world:
            params["world"] = world
        if ignore:
            params["ignore"] = ignore
        if ignore_player is not None:
            params["ignore_player"] = ignore_player
        return conn.call("debug.line_of_sight", params)

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
        since_mark: str = "",
    ) -> dict:
        """Read-only. The editor log as captured by the bridge since the editor started (last 20000 lines).

        since: cursor from a previous call (next) or from pie_exec_console (log_cursor); omit for the last
        max lines. since_mark: start at a pie_log_mark name instead. contains: substring. category:
        exact (e.g. LogIFSRQuest). verbosity: most verbose level kept — Error, Warning, Display, Log or Verbose.

        Returns {lines: [{seq, category, verbosity, text}], next, dropped}."""
        params: dict = {"max": max}
        if since is not None:
            params["since"] = since
        if since_mark:
            params["since_mark"] = since_mark
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
