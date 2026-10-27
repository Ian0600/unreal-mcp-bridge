from __future__ import annotations

from typing import Any

from fastmcp import FastMCP

from ..connection import UnrealConnection


def _params(**kwargs: Any) -> dict:
    """Drops unset arguments (None / empty string) so the plugin's own defaults apply."""
    return {k: v for k, v in kwargs.items() if v is not None and v != ""}


def register_sequence_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def sequence_create(name: str = "MCPSequence", fps: float = 30.0, duration: float = 5.0) -> dict:
        """Creates a TRANSIENT Level Sequence for a shot: never saved, never in Content (so no file
        outside Perforce), gone when the editor closes. Works with every sequencer_* / sequence_* tool.

        fps: display rate. duration: playback length in seconds (keys past the end extend it).

        Returns {sequence_path, transient, display_rate, playback_start, playback_end} (frames)."""
        return conn.call("sequence.create", {"name": name, "fps": fps, "duration": duration})

    @mcp.tool()
    def sequence_add_track(
        sequence_path: str,
        kind: str,
        binding: str = "",
        actor: str = "",
        spawn: str = "",
        name: str = "",
        anim: str = "",
        row: int | None = None,
        start_frame: float | None = None,
        start_time: float | None = None,
        location: list[float] | None = None,
        rotation: list[float] | None = None,
        scale: list[float] | None = None,
        camera: str = "",
        camera_actor: str = "",
        camera_spawn: str = "",
        camera_name: str = "",
    ) -> dict:
        """Adds a track to a sequence. kind: "transform", "animation" or "camera_cut".

        What it binds (transform / animation): binding = an existing binding name or GUID; or actor = a
        level actor label, bound as a possessable (it keeps its current transform until keyed); or
        spawn = an asset / actor class spawned by the sequence (skeletal mesh, Blueprint,
        /Script/CinematicCamera.CineCameraActor). name names a new binding.
        camera_cut uses camera / camera_actor / camera_spawn / camera_name the same way.
        transform: location / rotation [pitch, yaw, roll] / scale put a first key at the start.
        animation: anim (AnimSequence path), row. Start: start_frame (timeline frame) or start_time (s).

        Returns {sequence_path, kind, binding, binding_guid, binding_how, track, section_start,
        playback_end, warnings}."""
        return conn.call("sequence.add_track", _params(
            sequence_path=sequence_path,
            kind=kind,
            binding=binding,
            actor=actor,
            spawn=spawn,
            name=name,
            anim=anim,
            row=row,
            start_frame=start_frame,
            start_time=start_time,
            location=location,
            rotation=rotation,
            scale=scale,
            camera=camera,
            camera_actor=camera_actor,
            camera_spawn=camera_spawn,
            camera_name=camera_name,
        ))

    @mcp.tool()
    def sequence_set_keys(
        sequence_path: str,
        binding: str,
        keys: list[dict[str, Any]],
        interpolation: str = "cubic",
    ) -> dict:
        """Writes transform keys on a binding (creates its transform track when missing).

        keys: [{"frame" or "time", "location"?: [x,y,z], "rotation"?: [pitch,yaw,roll], "scale"?: [x,y,z]}].
        interpolation: cubic / linear / constant. Keys past the end extend the playback range.

        Returns {binding, keys_added, key_count, extended, playback_end, ...}."""
        return conn.call("sequence.set_keys", {
            "sequence_path": sequence_path,
            "binding": binding,
            "keys": keys,
            "interpolation": interpolation,
        })

    @mcp.tool()
    def sequence_capture_frame(
        sequence_path: str,
        frame: float | None = None,
        time: float | None = None,
        camera: str = "",
        location: list[float] | None = None,
        look_at: list[float] | None = None,
        rotation: list[float] | None = None,
        fov: float | None = None,
        width: int = 1600,
        height: int = 900,
        path: str = "",
        marks: list[Any] | None = None,
        warmup: int | None = None,
        game_view: bool = True,
        hide_actors: list[str] | None = None,
    ) -> dict:
        """Evaluates one frame of the sequence in the editor and saves an image of it from the camera —
        a camera binding, else an explicit location + look_at /
        rotation + fov, else the active camera cut at that frame. Returns once the file is on disk. Every level actor the sequence moved is put
        back and spawned actors are removed afterwards; level_dirty false confirms nothing was dirtied.

        frame (timeline frame) or time (seconds); default the playback start.
        path / width / height / marks / warmup / game_view / hide_actors: as capture_view.

        Returns capture_view's result + {frame, camera: {binding, location, rotation, fov}, level_dirty,
        dirtied_packages, map_dirty}."""
        return conn.call("sequence.capture_frame", _params(
            sequence_path=sequence_path,
            frame=frame,
            time=time,
            camera=camera,
            location=location,
            look_at=look_at,
            rotation=rotation,
            fov=fov,
            width=width,
            height=height,
            path=path,
            marks=marks,
            warmup=warmup,
            game_view=game_view,
            hide_actors=hide_actors,
        ), timeout=120.0)

    @mcp.tool()
    def sequence_discard(sequence_path: str) -> dict:
        """Drops a transient sequence made by sequence_create. Saved sequence assets are refused.

        Returns {discarded}."""
        return conn.call("sequence.discard", {"sequence_path": sequence_path})
