from __future__ import annotations

import time
from typing import Any

from fastmcp import FastMCP

from ..connection import UnrealConnection


def _wait_job(conn: UnrealConnection, job: int, timeout: float) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        state = conn.call("player.job_status", {"job": job})
        if state.get("done") or time.monotonic() >= deadline:
            return state
        time.sleep(0.1)


def register_player_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def input_inject(
        action: str,
        value: Any = None,
        seconds: float = 0.0,
        release: bool = False,
        player: int = 0,
    ) -> dict:
        """Feeds an Enhanced Input action to the PIE player, as if its key were used.

        action: asset name ("IA_GunAttack") or full path. value: bool, number, [x, y] or [x, y, z]
        (default = pressed). seconds: 0 = one frame; > 0 = hold for that many game seconds;
        < 0 = hold until input_inject(action, release=True).
        The action only fires while a mapping context that holds it is active. IFSR: IA_Move,
        IA_Look, IA_Sprint, IA_Dodge (Space = mantle / dodge), IA_Reload, IA_Aim are in IMC_Default;
        IA_GunAttack is only in IMC_Aim, which the game adds while aiming - hold IA_Aim first.

        Returns {action, value_type, mode}."""
        params: dict = {"action": action, "seconds": seconds, "release": release, "player": player}
        if value is not None:
            params["value"] = value
        return conn.call("input.inject", params)

    @mcp.tool()
    def player_aim_at(
        target: list[float] | None = None,
        actor: str = "",
        deg_per_sec: float = 360.0,
        error_deg: float = 0.0,
        timeout: float = 5.0,
        wait: bool = True,
        player: int = 0,
    ) -> dict:
        """Turns the PIE player's view toward a point (or an actor's bounds center) at deg_per_sec,
        settling error_deg off in a random direction - a person aiming, not a snap.

        Rotation runs on game time. wait: return when it settles (or after timeout game seconds);
        otherwise return {job} and read player_job_status later.

        Returns {job, done, reached, seconds, error_deg (final angle between view and target)}."""
        params: dict = {"deg_per_sec": deg_per_sec, "error_deg": error_deg, "timeout": timeout, "player": player}
        if target is not None:
            params["target"] = target
        if actor:
            params["actor"] = actor
        started = conn.call("player.aim_at", params)
        return _wait_job(conn, started["job"], timeout * 4 + 5) if wait else started

    @mcp.tool()
    def player_teleport(
        location: list[float],
        yaw: float | None = None,
        wait_streaming: bool = True,
        timeout: float = 20.0,
        tolerance: float = 100.0,
        settle_seconds: float = 0.5,
        wait: bool = True,
        player: int = 0,
    ) -> dict:
        """Moves the PIE player and verifies it stayed there.

        The pawn is held in place (no falling) until World Partition cells have streamed in and a
        ground trace under the target hits, then stood on that ground; after settle_seconds its
        real position is measured. ok is false when it ends more than tolerance cm (2D) away.

        Returns {job, done, ok, target, location (actual), distance, streaming_completed,
        ground_found, seconds, error}."""
        params: dict = {
            "location": location, "wait_streaming": wait_streaming, "timeout": timeout,
            "tolerance": tolerance, "settle_seconds": settle_seconds, "player": player,
        }
        if yaw is not None:
            params["yaw"] = yaw
        started = conn.call("player.teleport", params)
        return _wait_job(conn, started["job"], timeout * 4 + 10) if wait else started

    @mcp.tool()
    def view_is_visible(target: list[float] | None = None, actor: str = "", player: int = 0) -> dict:
        """Read-only. Is a point (or an actor's bounds center) on the PIE player's screen and not hidden?

        Returns {on_screen, in_front, inside_viewport, screen [x, y] px, viewport [w, h], occluded,
        occluder {actor, component, location} | null, target, distance}."""
        params: dict = {"player": player}
        if target is not None:
            params["target"] = target
        if actor:
            params["actor"] = actor
        return conn.call("view.is_visible", params)

    @mcp.tool()
    def player_job_status(job: int) -> dict:
        """Read-only. State of a player_aim_at / player_teleport job started with wait=False."""
        return conn.call("player.job_status", {"job": job})
