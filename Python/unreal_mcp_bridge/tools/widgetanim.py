from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_widgetanim_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    """UMG animation authoring: animations, widget bindings, tracks, sections, keyframes.

    A widget animation is a UMovieScene, so the pieces stack in a fixed order and each one fails
    with a clear message if you skip ahead:

        widget_anim_create -> widget_anim_bind_widget -> widget_anim_track_add -> widget_anim_key_add

    Two things are easy to get wrong from outside the editor. A widget must be exposed as a
    blueprint variable before it can be possessed by an animation. And a possessable GUID means
    nothing to UMG on its own — the parallel FWidgetAnimationBinding array is what maps it back to a
    widget name at runtime, so both have to be written together. These commands do that; hand-built
    movie scene edits from Python generally do not, and the animation then plays over nothing.

    Times cross the wire in seconds, matching what the animation panel shows; frame numbers come
    back alongside them for reference."""

    # ---------------------------------------------------------------- animation assets

    @mcp.tool()
    def widget_anim_list(widget_path: str) -> dict:
        """Lists the animations on a Widget Blueprint.

        Returns {"count", "animations": [{"name", "display_label", "start_time", "end_time",
        "display_rate", "binding_count", "track_count"}]}. Every other command here accepts either
        the name or the display label."""
        if not widget_path:
            raise ValueError("widget_path is required")
        return conn.call("widget.anim_list", {"widget_path": widget_path})

    @mcp.tool()
    def widget_anim_create(
        widget_path: str,
        animation_name: str,
        start_time: float = 0.0,
        end_time: float = 5.0,
        frame_rate: float = 60.0,
    ) -> dict:
        """Creates a new animation on a Widget Blueprint.

        Animations become blueprint variables, so a name already used by a variable would produce a
        duplicate member at the next compile rather than failing here — pick a name that is free in
        both namespaces.

        start_time/end_time are the playback range in seconds, frame_rate the display rate.
        Returns {"ok", "name"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not animation_name:
            raise ValueError("animation_name is required")
        return conn.call("widget.anim_create", {
            "widget_path": widget_path,
            "animation_name": animation_name,
            "start_time": start_time,
            "end_time": end_time,
            "frame_rate": frame_rate,
        })

    @mcp.tool()
    def widget_anim_remove(widget_path: str, animation_name: str) -> dict:
        """Deletes an animation and the blueprint variable it created. Returns {"ok"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not animation_name:
            raise ValueError("animation_name is required")
        return conn.call("widget.anim_remove", {
            "widget_path": widget_path,
            "animation_name": animation_name,
        })

    @mcp.tool()
    def widget_anim_rename(widget_path: str, animation_name: str, new_name: str) -> dict:
        """Renames an animation, its blueprint variable, and the graph nodes referencing it.

        Returns {"ok", "name"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not animation_name:
            raise ValueError("animation_name is required")
        if not new_name:
            raise ValueError("new_name is required")
        return conn.call("widget.anim_rename", {
            "widget_path": widget_path,
            "animation_name": animation_name,
            "new_name": new_name,
        })

    @mcp.tool()
    def widget_anim_set_range(
        widget_path: str,
        animation_name: str,
        start_time: float | None = None,
        end_time: float | None = None,
        frame_rate: float | None = None,
    ) -> dict:
        """Changes the playback range and/or display rate. All three are optional.

        Existing sections keep their own ranges, so shortening the animation can leave a section
        hanging past the end where it is authored but never evaluated — check widget_anim_track_list
        afterwards. Returns {"ok", "start_time", "end_time", "display_rate"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not animation_name:
            raise ValueError("animation_name is required")
        if start_time is None and end_time is None and frame_rate is None:
            raise ValueError("provide start_time, end_time and/or frame_rate")
        params: dict = {"widget_path": widget_path, "animation_name": animation_name}
        if start_time is not None:
            params["start_time"] = start_time
        if end_time is not None:
            params["end_time"] = end_time
        if frame_rate is not None:
            params["frame_rate"] = frame_rate
        return conn.call("widget.anim_set_range", params)

    # ---------------------------------------------------------------- widget bindings

    @mcp.tool()
    def widget_anim_binding_list(widget_path: str, animation_name: str) -> dict:
        """Lists the widgets an animation possesses.

        Returns {"count", "bindings": [{"widget_name", "slot_widget_name", "guid",
        "is_root_widget", "track_count"}]}. A non-empty slot_widget_name means the binding animates
        the widget's slot (its layout) rather than the widget itself."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not animation_name:
            raise ValueError("animation_name is required")
        return conn.call("widget.anim_binding_list", {
            "widget_path": widget_path,
            "animation_name": animation_name,
        })

    @mcp.tool()
    def widget_anim_bind_widget(
        widget_path: str,
        animation_name: str,
        widget_name: str,
        bind_slot: bool = False,
    ) -> dict:
        """Makes a widget animatable by this animation — the step before any track can be added.

        bind_slot=true possesses the widget's slot object instead of the widget, which is how layout
        is animated: a canvas slot's Position and Size live on the slot, not on the widget, so
        sliding a panel on-screen needs a slot binding while fading it needs a widget binding.

        Writes both halves of the binding (the movie scene possessable and the widget binding entry
        that names it), which is what lets the runtime resolve the GUID back to a widget.

        Returns {"ok", "guid", "already_bound"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not animation_name:
            raise ValueError("animation_name is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        return conn.call("widget.anim_bind_widget", {
            "widget_path": widget_path,
            "animation_name": animation_name,
            "widget_name": widget_name,
            "bind_slot": bind_slot,
        })

    @mcp.tool()
    def widget_anim_unbind_widget(widget_path: str, animation_name: str, widget_name: str) -> dict:
        """Removes a widget binding and every track hanging off it.

        Both halves go — dropping only the possessable would leave the animation holding a dangling
        GUID. Returns {"ok"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not animation_name:
            raise ValueError("animation_name is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        return conn.call("widget.anim_unbind_widget", {
            "widget_path": widget_path,
            "animation_name": animation_name,
            "widget_name": widget_name,
        })

    # ---------------------------------------------------------------- tracks and sections

    @mcp.tool()
    def widget_anim_track_list(
        widget_path: str,
        animation_name: str,
        widget_name: str = "",
    ) -> dict:
        """Lists the tracks and sections of an animation, grouped by bound widget.

        widget_name optionally filters to one widget.

        Returns {"count", "tracks": [{"widget_name", "guid", "track_class", "track_name",
        "property", "property_path", "sections": [{"index", "row_index", "start_time", "end_time",
        "channels": [names]}]}]}.

        The channel names here ("R", "G", "Translation.X", ...) are exactly what
        widget_anim_key_add's channel argument takes — read them from here rather than guessing at
        the struct layout."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not animation_name:
            raise ValueError("animation_name is required")
        params: dict = {"widget_path": widget_path, "animation_name": animation_name}
        if widget_name:
            params["widget_name"] = widget_name
        return conn.call("widget.anim_track_list", params)

    @mcp.tool()
    def widget_anim_track_add(
        widget_path: str,
        animation_name: str,
        widget_name: str,
        property: str,
        track_class: str = "",
        create_section: bool = True,
    ) -> dict:
        """Adds a property track for a bound widget, with an initial section spanning the playback
        range.

        property is resolved on whatever the binding possesses — the widget for a plain binding, the
        slot object for a slot binding — and the track's property path is written from that. Without
        that path the track exists and animates nothing, which is the usual outcome of building one
        by hand.

        track_class overrides the movie scene track type when the default for that property is not
        what you want. create_section=false leaves the track empty.

        Returns {"ok", "track_class", "already_existed", "property", "section_index", "channels"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not animation_name:
            raise ValueError("animation_name is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        if not property:
            raise ValueError("property is required")
        params: dict = {
            "widget_path": widget_path,
            "animation_name": animation_name,
            "widget_name": widget_name,
            "property": property,
            "create_section": create_section,
        }
        if track_class:
            params["track_class"] = track_class
        return conn.call("widget.anim_track_add", params)

    @mcp.tool()
    def widget_anim_track_remove(
        widget_path: str,
        animation_name: str,
        widget_name: str,
        property: str,
    ) -> dict:
        """Deletes a property track and its sections. Returns {"ok", "removed"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not animation_name:
            raise ValueError("animation_name is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        if not property:
            raise ValueError("property is required")
        return conn.call("widget.anim_track_remove", {
            "widget_path": widget_path,
            "animation_name": animation_name,
            "widget_name": widget_name,
            "property": property,
        })

    @mcp.tool()
    def widget_anim_section_add(
        widget_path: str,
        animation_name: str,
        widget_name: str,
        property: str,
        start_time: float | None = None,
        end_time: float | None = None,
        row_index: int = 0,
    ) -> dict:
        """Adds another section to an existing track — a second burst of animation on the same
        property, separated in time from the first.

        row_index stacks sections that overlap in time; sections on one row must not overlap.

        Returns {"ok", "section_index", "channels"}. Pass that section_index to
        widget_anim_key_add."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not animation_name:
            raise ValueError("animation_name is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        if not property:
            raise ValueError("property is required")
        params: dict = {
            "widget_path": widget_path,
            "animation_name": animation_name,
            "widget_name": widget_name,
            "property": property,
            "row_index": row_index,
        }
        if start_time is not None:
            params["start_time"] = start_time
        if end_time is not None:
            params["end_time"] = end_time
        return conn.call("widget.anim_section_add", params)

    # ---------------------------------------------------------------- keyframes

    @mcp.tool()
    def widget_anim_key_add(
        widget_path: str,
        animation_name: str,
        widget_name: str,
        property: str,
        time: float,
        value: float | bool,
        channel: str = "",
        channel_index: int = 0,
        section_index: int = 0,
        interpolation: str = "cubic",
    ) -> dict:
        """Writes a keyframe onto one channel of one section.

        channel is the name from widget_anim_track_list ("R", "Translation.X", ...); channel_index
        is the fallback when no name is given. value is a number, or a bool for a boolean channel.
        interpolation is "cubic", "linear" or "constant" and applies to this key.

        A key outside the section's range is authored but never evaluated, so the section is grown
        to contain it. The animation's own playback range is not — widget_anim_set_range does that.

        Returns {"ok", "frame", "key_count"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not animation_name:
            raise ValueError("animation_name is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        if not property:
            raise ValueError("property is required")
        params: dict = {
            "widget_path": widget_path,
            "animation_name": animation_name,
            "widget_name": widget_name,
            "property": property,
            "time": time,
            "value": value,
            "channel_index": channel_index,
            "section_index": section_index,
            "interpolation": interpolation,
        }
        if channel:
            params["channel"] = channel
        return conn.call("widget.anim_key_add", params)

    @mcp.tool()
    def widget_anim_key_list(
        widget_path: str,
        animation_name: str,
        widget_name: str,
        property: str,
        channel: str = "",
        channel_index: int = 0,
        section_index: int = 0,
    ) -> dict:
        """Lists the keys on one channel. Returns {"count", "keys": [{"frame", "time"}]}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not animation_name:
            raise ValueError("animation_name is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        if not property:
            raise ValueError("property is required")
        params: dict = {
            "widget_path": widget_path,
            "animation_name": animation_name,
            "widget_name": widget_name,
            "property": property,
            "channel_index": channel_index,
            "section_index": section_index,
        }
        if channel:
            params["channel"] = channel
        return conn.call("widget.anim_key_list", params)
