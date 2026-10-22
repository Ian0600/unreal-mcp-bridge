from __future__ import annotations

from typing import Any

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_sequencer_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def sequencer_list_bindings(sequence_path: str) -> dict:
        """Lists every binding in a Level Sequence with its kind and tracks, without opening Sequencer.

        Returns {"display_rate", "tick_resolution", "playback_start", "playback_end",
        "bindings": [{"name", "display_name", "guid", "kind" (spawnable/possessable), "bound_class",
        "custom_binding", "tracks": [{"class", "display_name", "num_sections"}]}], "tracks": [global]}.
        Frames are display frames (the numbers on the Sequencer timeline)."""
        if not sequence_path:
            raise ValueError("sequence_path is required")
        return conn.call("sequencer.list_bindings", {"sequence_path": sequence_path})

    @mcp.tool()
    def sequencer_list_sections(
        sequence_path: str,
        binding: str = "",
        track_class: str = "",
    ) -> dict:
        """Lists every section of every matching track — all of them, Additive layers included, not
        just the first.

        binding: binding name or GUID (empty = all bindings plus global tracks).
        track_class: exact class name/path, a superclass, or a substring — "ControlRig" picks the
        Control Rig tracks.

        Returns {"tracks": [{"binding", "binding_guid", "class", "display_name", "num_sections",
        "sections": [{"index", "class", "row_index", "overlap_priority", "blend_type", "active",
        "locked", "start_frame", "end_frame" (exclusive; null = open), "num_channels", "num_keys"}]}]}."""
        if not sequence_path:
            raise ValueError("sequence_path is required")
        params: dict = {"sequence_path": sequence_path}
        if binding:
            params["binding"] = binding
        if track_class:
            params["track_class"] = track_class
        return conn.call("sequencer.list_sections", params)

    @mcp.tool()
    def sequencer_add_spawnable(
        sequence_path: str,
        asset_path: str,
        name: str = "",
        animation_path: str = "",
        start_frame: float | None = None,
    ) -> dict:
        """Adds a spawnable that really spawns — a SkeletalMesh asset, actor Blueprint or actor class.

        Unlike Python's add_spawnable_from_instance(SkeletalMesh), which makes a binding that spawns
        nothing, a SkeletalMesh becomes a SkeletalMeshActor template. The level is not touched.
        name: binding name (default: the asset name). animation_path: optional AnimSequence put on a
        new skeletal animation track at start_frame (display frame; default = playback start).

        Returns {"ok", "guid", "name", "kind", "custom_binding", "bound_class",
        "animation_track_added"} (+ "warning" if the binding would not spawn an actor).
        Does not save — call asset_save on the sequence."""
        if not sequence_path:
            raise ValueError("sequence_path is required")
        if not asset_path:
            raise ValueError("asset_path is required")
        params: dict = {"sequence_path": sequence_path, "asset_path": asset_path}
        if name:
            params["name"] = name
        if animation_path:
            params["animation_path"] = animation_path
        if start_frame is not None:
            params["start_frame"] = start_frame
        return conn.call("sequencer.add_spawnable", params)

    @mcp.tool()
    def sequencer_bake_anim_sequence(
        sequence_path: str,
        binding: str,
        anim_path: str,
        skeleton_path: str = "",
        export_options: dict[str, Any] | None = None,
        create_link: bool = False,
    ) -> dict:
        """Bakes one binding's skeletal mesh to an AnimSequence in a single call — no need to open the
        sequence first, and no open/bake split.

        binding: binding name or GUID. anim_path: an existing AnimSequence, or a new asset path to
        create. skeleton_path: only needed when creating and the binding has no skeletal mesh template
        (a possessable). export_options: UAnimSeqExportOption properties, values as ImportText strings
        or bools/numbers, e.g. {"bRecordInWorldSpace": true, "bUseCustomTimeRange": true,
        "CustomStartFrame": "(Value=0)", "CustomEndFrame": "(Value=120)"}.
        create_link: link the AnimSequence to the sequence like the editor's "Create Linked Animation".

        Returns {"ok", "anim_path", "created", "binding", "skeleton", "play_length", "num_frames"}.
        Does not save — call asset_save on anim_path."""
        if not sequence_path:
            raise ValueError("sequence_path is required")
        if not binding:
            raise ValueError("binding is required")
        if not anim_path:
            raise ValueError("anim_path is required")
        params: dict = {"sequence_path": sequence_path, "binding": binding, "anim_path": anim_path}
        if skeleton_path:
            params["skeleton_path"] = skeleton_path
        if export_options:
            params["export_options"] = export_options
        if create_link:
            params["create_link"] = True
        return conn.call("sequencer.bake_anim_sequence", params)

    @mcp.tool()
    def sequencer_get_marked_frames(sequence_path: str) -> dict:
        """Reads the sequence's marked frames (replaces the deprecated get_marked_frames).

        Returns {"marks": [{"index", "frame" (display), "tick", "seconds", "label", "comment",
        "use_custom_color", "color"}], "locked", "display_rate"}."""
        if not sequence_path:
            raise ValueError("sequence_path is required")
        return conn.call("sequencer.get_marked_frames", {"sequence_path": sequence_path})

    @mcp.tool()
    def sequencer_set_marked_frames(
        sequence_path: str,
        marks: list[dict[str, Any]],
        replace: bool = False,
    ) -> dict:
        """Adds or updates marked frames. marks: [{"frame" (display frame), "label", "comment",
        "color" ([r,g,b] or [r,g,b,a])}] — only frame is required. A mark whose non-empty label already
        exists is moved/updated instead of duplicated. replace=true clears all marks first.

        Returns the full mark list like sequencer_get_marked_frames."""
        if not sequence_path:
            raise ValueError("sequence_path is required")
        if not marks:
            raise ValueError("marks is required")
        return conn.call("sequencer.set_marked_frames", {
            "sequence_path": sequence_path,
            "marks": marks,
            "replace": replace,
        })

    @mcp.tool()
    def sequencer_remove_marked_frames(
        sequence_path: str,
        labels: list[str] | None = None,
        indices: list[int] | None = None,
        all: bool = False,
    ) -> dict:
        """Removes marked frames by label, by index (from sequencer_get_marked_frames), or all.

        Returns {"removed"} plus the remaining mark list."""
        if not sequence_path:
            raise ValueError("sequence_path is required")
        if not all and not labels and not indices:
            raise ValueError("give labels, indices or all=true")
        params: dict = {"sequence_path": sequence_path}
        if all:
            params["all"] = True
        if labels:
            params["labels"] = labels
        if indices:
            params["indices"] = indices
        return conn.call("sequencer.remove_marked_frames", params)
