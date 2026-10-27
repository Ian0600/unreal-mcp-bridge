from __future__ import annotations

import time
from typing import Any

from fastmcp import FastMCP

from ..connection import UnrealConnection


def _params(**kwargs: Any) -> dict:
    """Drops unset arguments (None / empty string) so the plugin's own defaults apply."""
    return {k: v for k, v in kwargs.items() if v is not None and v != ""}


def _wait_for_job(conn: UnrealConnection, job_id: int, deadline_s: float) -> dict:
    """Polls scene.shots_status until the job is done or the deadline passes."""
    end = time.monotonic() + deadline_s
    status: dict = {}
    while True:
        status = conn.call("scene.shots_status", {"job_id": job_id})
        if status.get("done") or time.monotonic() > end:
            break
        time.sleep(0.25)
    if not status.get("done"):
        status["timed_out_waiting"] = True
    return status


def register_scene_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def capture_view(
        location: list[float],
        look_at: list[float] | None = None,
        rotation: list[float] | None = None,
        fov: float = 60.0,
        width: int = 1600,
        height: int = 900,
        ortho_width: float | None = None,
        marks: list[Any] | None = None,
        path: str = "",
        game_view: bool = True,
        warmup: int | None = None,
        settle_frames: int = 0,
        hide_actors: list[str] | None = None,
        respect_editor_hidden: bool = True,
        stream_textures: bool = True,
        stream_seconds: float | None = None,
    ) -> dict:
        """Renders a game view (no editor icons, splines or debug text) to an image and returns only
        after the file is on disk. Touches no actor.

        location / look_at: world cm [x, y, z]; or rotation [pitch, yaw, roll] instead of look_at.
        fov: horizontal degrees. ortho_width > 0: orthographic (top-down layout shots).
        path: .png/.jpg under <Project>/Saved (relative = under Saved; default Saved/MCPBridge/Captures/).
        marks: world points [[x,y,z]] or [{"name", "location"}] -> marks_px [{name, x, y, in_view,
        behind_camera}] pixel positions for labels.
        warmup: extra renders in the same call (default 4) so temporal AA / eye adaptation settle.
        settle_frames > 0: render over that many real editor frames instead (lets texture streaming and
        Lumen catch up) — runs as a one-shot scene.run_shots job and still returns the finished file.
        hide_actors: labels/paths left out of the image. Actors hidden with scene.temp_override and
        (respect_editor_hidden) actors hidden in the viewport with H are left out too.

        Returns {ok, path, file_size, width, height, location, rotation, fov, marks_px,
        hidden_actor_count}."""
        camera = _params(
            location=location,
            look_at=look_at,
            rotation=rotation,
            fov=fov,
            width=width,
            height=height,
            ortho_width=ortho_width,
            marks=marks,
            path=path,
            game_view=game_view,
            warmup=warmup,
            hide_actors=hide_actors,
            respect_editor_hidden=respect_editor_hidden,
            stream_textures=stream_textures,
            stream_seconds=stream_seconds,
        )
        if settle_frames and settle_frames > 0:
            job = conn.call("scene.run_shots", {"shots": [{"id": "capture", "camera": camera}], "settle_frames": settle_frames})
            status = _wait_for_job(conn, job["job_id"], 120.0)
            results = status.get("results") or []
            if not results:
                return {"ok": False, "error": "capture job did not finish", "status": status}
            shot = results[0]
            if not shot.get("ok"):
                return {"ok": False, "error": shot.get("error", "capture failed")}
            return shot["capture"]
        return conn.call("scene.capture_view", camera, timeout=120.0)

    @mcp.tool()
    def spawn_temp(
        asset: str = "",
        location: list[float] | None = None,
        rotation: list[float] | None = None,
        scale: list[float] | None = None,
        size_m: list[float] | None = None,
        material: str = "",
        color: list[float] | None = None,
        properties: dict[str, Any] | None = None,
        tag: str = "",
        label: str = "",
        snap_to_ground: bool = False,
        z_offset: float | None = None,
        items: list[dict[str, Any]] | None = None,
    ) -> dict:
        """Places actors that are never saved (RF_Transient, no __ExternalActors__ file) for photos and
        blockouts. Remove them with clear_temp.

        asset: static or skeletal mesh path, an actor Blueprint on the bridge's
        TempSpawnBlueprintAllowList, PointLight / SpotLight / RectLight / DirectionalLight /
        CameraActor, or Cube. size_m [x, y, z] with no asset (or Cube) = a blockout box of that size in
        metres, pivot at its centre; color [r, g, b] (0-1) tints it, material sets any material.
        properties: {"LightComponent.Intensity": "5000", ...} — ImportText values, numbers or bools.
        snap_to_ground: rest the bottom of the bounds on the ground below (z_offset adds height).
        items: [{...}, ...] places several in one call; top-level arguments are their defaults.
        All-or-nothing: one failed item removes the rest of the batch.

        Returns {actors: [{name, label, path, class, location, rotation, bounds {size_m, ...}, tags}],
        level_dirty (false = nothing was dirtied), dirtied_packages, map_dirty}."""
        return conn.call("scene.spawn_temp", _params(
            asset=asset,
            location=location,
            rotation=rotation,
            scale=scale,
            size_m=size_m,
            material=material,
            color=color,
            properties=properties,
            tag=tag,
            label=label,
            snap_to_ground=snap_to_ground or None,
            z_offset=z_offset,
            items=items,
        ))

    @mcp.tool()
    def clear_temp(tag: str = "") -> dict:
        """Removes temp actors by scanning the whole editor world for the bridge's MCPTemp tag (not just
        what this session remembers) — all of them, or only those carrying tag — and reverts
        temp_override changes the same way.

        Returns {removed_count, removed, reverted_overrides, level_dirty, dirtied_packages, map_dirty}."""
        return conn.call("scene.clear_temp", _params(tag=tag))

    @mcp.tool()
    def pose_character(
        meshes: list[str] | None = None,
        preset: str = "",
        preset_file: str = "",
        anim: str = "",
        time: float | None = None,
        fraction: float | None = None,
        yaw_offset: float | None = None,
        weapon: str = "",
        socket: str = "",
        weapon_location: list[float] | None = None,
        weapon_rotation: list[float] | None = None,
        location: list[float] | None = None,
        rotation: list[float] | None = None,
        tag: str = "",
        label: str = "",
        snap_to_ground: bool = False,
    ) -> dict:
        """Places a multi-part character as a temp actor and freezes it on one animation frame.

        meshes: skeletal meshes, the first is the leader and the rest (face, hair, gear) follow it
        with leader pose. preset: a named entry from the preset JSON (preset_file, or the bridge's
        PosePresetFile setting, default Saved/MCPBridge/pose_presets.json) holding the same fields —
        {"presets": {"Rita": {"meshes": [...], "weapon": "...", "socket": "...", "yaw_offset": -90}}};
        arguments given here override the preset.
        anim + time (seconds) or fraction (0-1 of the clip). yaw_offset: per-clip facing correction
        added to rotation's yaw. weapon: static or skeletal mesh attached to socket.
        Editor animation update is switched on and the pose is evaluated before this returns.

        Returns the actor (as spawn_temp) + {parts, anim, time, length, facing_yaw, yaw_offset,
        warnings, level_dirty, dirtied_packages}."""
        return conn.call("scene.pose_character", _params(
            meshes=meshes,
            preset=preset,
            preset_file=preset_file,
            anim=anim,
            time=time,
            fraction=fraction,
            yaw_offset=yaw_offset,
            weapon=weapon,
            socket=socket,
            weapon_location=weapon_location,
            weapon_rotation=weapon_rotation,
            location=location,
            rotation=rotation,
            tag=tag,
            label=label,
            snap_to_ground=snap_to_ground or None,
        ))

    @mcp.tool()
    def temp_override(
        actor: str,
        hide: bool | None = None,
        anim: str = "",
        time: float | None = None,
        fraction: float | None = None,
        component: str = "",
        tag: str = "",
        revert: bool = False,
    ) -> dict:
        """Borrows a REAL level actor for a shot: hide it (the editor's temporary hide, never the saved
        hidden flag — capture_view leaves it out) and/or freeze its skeletal mesh on an animation frame.

        actor: label, name or object path. anim + time / fraction (component picks the skeletal mesh
        component; default the first). revert=true puts this actor back now.
        Everything is reverted by clear_temp (all, or by tag) and automatically just before the actor's
        package is saved. Packages this call dirtied are set clean again (cleaned_dirty_flags).

        Returns {actor, hidden, anim_overridden, time, tag, cleaned_dirty_flags, level_dirty}."""
        return conn.call("scene.temp_override", _params(
            actor=actor,
            hide=hide,
            anim=anim,
            time=time,
            fraction=fraction,
            component=component,
            tag=tag,
            revert=revert or None,
        ))

    @mcp.tool()
    def get_ground_height(
        points: list[list[float]],
        ignore: list[str] | None = None,
        layer: int = 0,
        top_z: float | None = None,
        bottom_z: float | None = None,
        include_layers: bool = True,
    ) -> dict:
        """Read-only. Ground height under [x, y] points, tracing straight down on the Visibility channel.

        ignore: default ["volumes", "temp"] (trigger / blocking / boss-arena shapes and temp actors are
        skipped); passing a list replaces the default; "roofs" means take the lowest surface.
        layer: 0 = top surface, 1 = the next one down (floor under a roof), -1 = lowest.
        Each component counts once, so roof, floor and terrain are separate layers.

        Returns {points: [{x, y, hit, z, actor, layers: [{z, normal_z, actor, actor_path, component}]}]}."""
        return conn.call("scene.get_ground_height", _params(
            points=points,
            ignore=ignore,
            layer=layer,
            top_z=top_z,
            bottom_z=bottom_z,
            include_layers=include_layers,
        ))

    @mcp.tool()
    def run_shots(
        shots: list[dict[str, Any]],
        settle_frames: int = 4,
        timeout_s: float = 60.0,
        on_fail: str = "continue",
        wait: bool = True,
    ) -> dict:
        """Works through a shot list in the editor — per shot: place -> wait real frames -> capture ->
        clear — with one call instead of a round trip per step.

        shots: [{"id", "spawn": [spawn_temp args], "pose": [pose_character args], "override":
        [temp_override args], "camera": {capture_view args}, "path", "settle_frames", "keep"}].
        Each shot's temp actors / overrides get a per-shot tag and are cleared after its capture
        unless keep=true. settle_frames: frames rendered into the capture before the kept one.
        timeout_s: per shot. on_fail: "continue" (clean up, next shot) or "stop".
        wait=false returns {job_id} at once; read progress with shots_status.

        Returns {job_id, done, total, failed, elapsed_seconds, results: [{id, ok, path, capture, error,
        setup, seconds}]}."""
        job = conn.call("scene.run_shots", _params(
            shots=shots,
            settle_frames=settle_frames,
            timeout_s=timeout_s,
            on_fail=on_fail,
        ))
        if not wait:
            return job
        return _wait_for_job(conn, job["job_id"], timeout_s * max(1, len(shots)) + 30.0)

    @mcp.tool()
    def shots_status(job_id: int) -> dict:
        """Read-only. Progress of a run_shots job started with wait=false (kept 10 minutes after it ends).

        Returns {job_id, done, index, total, failed, elapsed_seconds, results}."""
        return conn.call("scene.shots_status", {"job_id": job_id})
