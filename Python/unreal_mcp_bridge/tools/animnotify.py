from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_animnotify_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def animnotify_list(animation_path: str, notify_class: str = "", include_properties: bool = False) -> dict:
        """Lists the notify events on an animation asset (sequence or montage).

        notify_class optionally filters by class name or path and matches subclasses, e.g.
        "AnimNotifyState_MotionWarping" also returns project subclasses of it.

        "name" is the NotifyName stored when the notify was created - a cached copy that does not
        follow later edits (a sound notify can still say "PostSound (no event)" after its Wwise event
        was set). "label" is what the notify reports now (the Persona timeline text).
        include_properties adds "properties" (every editable property that differs from the notify
        class default: Wwise event, Niagara system, socket ...) - judge "is the sound set" from these.

        Returns {"notifies": [{"index", "class", "name", "label", "start_time", "duration", "track"}]}. The index is
        what statetree-style edits need — pass it to animnotify_set_window."""
        if not animation_path:
            raise ValueError("animation_path is required")
        params: dict = {"animation_path": animation_path}
        if notify_class:
            params["notify_class"] = notify_class
        if include_properties:
            params["include_properties"] = True
        return conn.call("animnotify.list", params)

    @mcp.tool()
    def animnotify_set_window(
        animation_path: str,
        index: int,
        start_time: float | None = None,
        duration: float | None = None,
    ) -> dict:
        """Retimes an existing notify event in place. index comes from animnotify_list. Pass start_time
        and/or duration (duration applies to notify states only).

        Edits the event in place rather than removing and re-adding it — a notify's Instanced sub-objects
        (a MotionWarping notify's RootMotionModifier, for instance) are rebuilt from class defaults on
        re-creation, which silently discards whatever the animator configured on them.

        Returns {"ok", "class", "start_time", "duration"} reflecting the stored values."""
        if not animation_path:
            raise ValueError("animation_path is required")
        if start_time is None and duration is None:
            raise ValueError("provide start_time and/or duration")
        params: dict = {"animation_path": animation_path, "index": index}
        if start_time is not None:
            params["start_time"] = start_time
        if duration is not None:
            params["duration"] = duration
        return conn.call("animnotify.set_window", params)

    @mcp.tool()
    def animnotify_add(
        animation_path: str,
        notify_class: str,
        start_time: float,
        duration: float | None = None,
        track: int = 0,
        properties: dict[str, str] | None = None,
    ) -> dict:
        """Places a new class-based notify or notify state. notify_class is a class path
        (/Script/IFSR.AnimNotifyState_X) or bare class name; duration is required for notify states
        and refused for plain notifies. properties: {"PropName": "ImportText value"} set on the new
        notify object (enums, structs, gameplay tags as the editor would write them).

        For a class-less named notify use animnotify_add_named instead.
        Returns {"ok", "index", "class", "start_time", "duration", "track"} — index after sorting."""
        if not animation_path:
            raise ValueError("animation_path is required")
        if not notify_class:
            raise ValueError("notify_class is required")
        params: dict = {
            "animation_path": animation_path,
            "notify_class": notify_class,
            "start_time": start_time,
            "track": track,
        }
        if duration is not None:
            params["duration"] = duration
        if properties:
            params["properties"] = properties
        return conn.call("animnotify.add", params)

    @mcp.tool()
    def animnotify_add_named(
        animation_path: str,
        notify_name: str,
        time: float | None = None,
        frame: float | None = None,
        track_name: str = "",
        track: int = 0,
    ) -> dict:
        """Places a class-less named notify (Add Notify → New Notify… in the editor), e.g.
        "SporeBurstFire". Python cannot: add_animation_notify_event_from_source adds nothing without a
        notify object.

        Give exactly one of time (seconds) or frame (at the animation's sampling rate). track_name picks
        a notify track by name and creates it when missing; otherwise track is an index. The name is
        also registered on the skeleton, so it appears in the picker and as an AnimBP
        "AnimNotify_<name>" event.

        Returns {"ok", "index", "notify_name", "start_time", "track", "track_name", "track_created",
        "skeleton_notify_added"}. Does not save — call asset_save on the animation (and the skeleton
        when skeleton_notify_added)."""
        if not animation_path:
            raise ValueError("animation_path is required")
        if not notify_name:
            raise ValueError("notify_name is required")
        if (time is None) == (frame is None):
            raise ValueError("give exactly one of time or frame")
        params: dict = {"animation_path": animation_path, "notify_name": notify_name}
        if time is not None:
            params["time"] = time
        else:
            params["frame"] = frame
        if track_name:
            params["track_name"] = track_name
        else:
            params["track"] = track
        return conn.call("animnotify.add_named", params)

    @mcp.tool()
    def animnotify_list_skeleton_names(skeleton_path: str = "", animation_path: str = "") -> dict:
        """Reads a skeleton's named-notify list (Skeleton.AnimationNotifies is protected in Python).
        Give skeleton_path (Skeleton or SkeletalMesh) or animation_path (uses that animation's skeleton).

        Returns {"skeleton", "names": [...]}."""
        params = _skeleton_target(skeleton_path, animation_path)
        return conn.call("animnotify.list_skeleton_names", params)

    @mcp.tool()
    def animnotify_add_skeleton_names(
        names: list[str],
        skeleton_path: str = "",
        animation_path: str = "",
    ) -> dict:
        """Registers notify names on a skeleton without placing any notify, so they are in the picker
        before anyone keys them. Already-registered names are reported, not duplicated.

        Returns {"skeleton", "added": [...], "existing": [...], "names": [full list]}.
        Does not save — call asset_save on the skeleton."""
        if not names:
            raise ValueError("names is required")
        params = _skeleton_target(skeleton_path, animation_path)
        params["names"] = names
        return conn.call("animnotify.add_skeleton_names", params)


def _skeleton_target(skeleton_path: str, animation_path: str) -> dict:
    if skeleton_path:
        return {"skeleton_path": skeleton_path}
    if animation_path:
        return {"animation_path": animation_path}
    raise ValueError("skeleton_path or animation_path is required")
