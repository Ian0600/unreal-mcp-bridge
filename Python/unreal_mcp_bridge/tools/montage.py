from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_montage_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def montage_get_info(montage_path: str) -> dict:
        """Dumps an AnimMontage's structure: slot tracks, their segments, and the section table.

        Read this before every other montage_* call — segment and section indices come from here, and
        section start times must line up with segment boundaries for a staged montage to play cleanly.

        Returns {"path", "skeleton", "play_length", "blend_in", "blend_out",
                 "slots": [{"slot", "length", "segments": [{"index", "animation", "start_pos",
                            "anim_start", "anim_end", "play_rate", "loop_count", "length"}]}],
                 "sections": [{"index", "name", "start_time", "end_time", "next"}]}."""
        if not montage_path:
            raise ValueError("montage_path is required")
        return conn.call("montage.get_info", {"montage_path": montage_path})

    @mcp.tool()
    def montage_add_segment(
        montage_path: str,
        animation_path: str,
        slot: str = "",
        start_pos: float | None = None,
        play_rate: float = 1.0,
        loop_count: int = 1,
    ) -> dict:
        """Appends an anim sequence to a montage slot track, building up a multi-stage montage.

        editor_python_exec cannot do this correctly. `slot_anim_tracks` is writable from Python, but a
        montage's length is derived from its segments and `sequence_length` is Read-Only there — so a
        Python-built multi-segment montage keeps the length of its first segment and everything after
        that is cut off mid-playback. This command recalculates the length and re-links the sections
        and notifies that hang off the moved segments.

        slot defaults to "DefaultSlot". A slot that does not exist yet is created.
        start_pos defaults to the end of that slot's existing content, i.e. straight append — which is
        what you want when laying stages down in order.

        Returns {"ok", "slot", "index", "start_pos", "length", "play_length"}. The index is the
        position AFTER segments are re-sorted by time, so pass that one to montage_remove_segment."""
        if not montage_path:
            raise ValueError("montage_path is required")
        if not animation_path:
            raise ValueError("animation_path is required")
        params: dict = {
            "montage_path": montage_path,
            "animation_path": animation_path,
            "play_rate": play_rate,
            "loop_count": loop_count,
        }
        if slot:
            params["slot"] = slot
        if start_pos is not None:
            params["start_pos"] = start_pos
        return conn.call("montage.add_segment", params)

    @mcp.tool()
    def montage_remove_segment(montage_path: str, index: int, slot: str = "") -> dict:
        """Removes a segment from a montage slot track. index comes from montage_get_info.

        slot defaults to "DefaultSlot". Sections keep their times, so a removal that shortens the
        montage can leave a section stranded past the end — check montage_get_info afterwards.

        Returns {"ok", "slot", "play_length"}."""
        if not montage_path:
            raise ValueError("montage_path is required")
        params: dict = {"montage_path": montage_path, "index": index}
        if slot:
            params["slot"] = slot
        return conn.call("montage.remove_segment", params)

    @mcp.tool()
    def montage_add_section(
        montage_path: str,
        name: str,
        start_time: float = 0.0,
        next: str = "",
    ) -> dict:
        """Adds a named section to a montage. Sections are how one montage holds several stages of an
        animation (grab start / loop / escape) that gameplay jumps between at runtime.

        Python cannot reach these at all: UAnimMontage::CompositeSections has no Blueprint accessor, so
        get_editor_property and set_editor_property both refuse it.

        next names the section that plays when this one ends. Pass this section's OWN name to make it
        loop in place; leave it empty and the montage blends out at the end of the section. The target
        must already exist — add sections in playback order, or set the links afterwards with
        montage_set_section.

        Adding a section auto-links the section BEFORE it, if that one's next was empty. So where two
        stages must NOT run into each other (an escape and a death ending, say), break the link
        afterwards with montage_set_section(clear_next=True). Links that were already set are left
        alone, so a loop you declared stays a loop.

        start_time need not be in order — sections are re-sorted by time. The auto-link is not:
        it binds by array position at insert time, i.e. BEFORE that sort, so inserting out of order
        produces links that skip around once the table settles. Add stages in playback order, or set
        every next explicitly afterwards.

        Returns {"ok", "index", "name", "start_time", "end_time", "next"}."""
        if not montage_path:
            raise ValueError("montage_path is required")
        if not name:
            raise ValueError("name is required")
        params: dict = {"montage_path": montage_path, "name": name, "start_time": start_time}
        if next:
            params["next"] = next
        return conn.call("montage.add_section", params)

    @mcp.tool()
    def montage_set_section(
        montage_path: str,
        section: str = "",
        index: int | None = None,
        name: str | None = None,
        next: str | None = None,
        clear_next: bool = False,
    ) -> dict:
        """Renames a section and/or repoints its `next` link. Identify the section by `section` (name)
        or `index` (from montage_get_info).

        name renames it, fixing up any other section whose `next` pointed at the old name — those links
        are stored as name strings, so a bare rename silently breaks them and the montage just stops
        there. Use this to rename the "Default" section a freshly created montage comes with.

        next repoints the link: name the section that should follow, or this section's own name to loop
        in place.

        clear_next=True unlinks it instead, so the montage blends out at the end of this section. Use
        this rather than next="" — an empty string is easy to drop on the way through a tool call, and
        dropping it silently means "leave the link alone", which is the opposite of what you asked for.
        clear_next and next are mutually exclusive.

        A section's start time cannot be changed here. Retime by removing and re-adding it; insertion
        order does not matter, since sections are re-sorted by time.

        Returns {"ok", "index", "name", "start_time", "end_time", "next"}."""
        if not montage_path:
            raise ValueError("montage_path is required")
        if not section and index is None:
            raise ValueError("provide section (name) or index")
        if clear_next and next:
            raise ValueError("clear_next and next are mutually exclusive")
        if name is None and next is None and not clear_next:
            raise ValueError("provide name, next, or clear_next")
        params: dict = {"montage_path": montage_path}
        if section:
            params["section"] = section
        if index is not None:
            params["index"] = index
        if name is not None:
            params["name"] = name
        if clear_next:
            params["next"] = ""
        elif next is not None:
            params["next"] = next
        return conn.call("montage.set_section", params)

    @mcp.tool()
    def montage_remove_section(montage_path: str, section: str = "", index: int | None = None) -> dict:
        """Removes a section, identified by `section` (name) or `index` (from montage_get_info).

        Any other section whose `next` pointed at it has that link cleared, rather than left dangling at
        a name that no longer resolves — a dangling link stops playback with nothing in the asset to
        show why.

        A montage must keep at least one section, so removing the last one fails; rename it instead.

        Returns {"ok", "removed", "sections": [...]}."""
        if not montage_path:
            raise ValueError("montage_path is required")
        if not section and index is None:
            raise ValueError("provide section (name) or index")
        params: dict = {"montage_path": montage_path}
        if section:
            params["section"] = section
        if index is not None:
            params["index"] = index
        return conn.call("montage.remove_section", params)
