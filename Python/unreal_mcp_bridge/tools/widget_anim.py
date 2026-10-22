from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection

# Widget animations are movie scene sequences, so the authoring order is fixed:
#   anim_create -> anim_bind_widget -> anim_track_add -> anim_key_add
# Each step depends on the previous one, and skipping a step produces an animation that
# exists but animates nothing. The docstrings below spell the chain out rather than
# referencing a shared constant: a docstring has to be a plain literal or Python discards it,
# taking the tool description with it.


def register_widget_anim_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    def _bp(widget_path: str) -> dict:
        if not widget_path:
            raise ValueError("widget_path is required")
        return {"widget_path": widget_path}

    def _anim(widget_path: str, animation_name: str) -> dict:
        if not animation_name:
            raise ValueError("animation_name is required")
        params = _bp(widget_path)
        params["animation_name"] = animation_name
        return params

    def _track(widget_path: str, animation_name: str, widget_name: str, property: str) -> dict:
        if not widget_name:
            raise ValueError("widget_name is required")
        if not property:
            raise ValueError("property is required")
        params = _anim(widget_path, animation_name)
        params["widget_name"] = widget_name
        params["property"] = property
        return params

    # =====================================================================================
    # ANIMATION ASSETS
    # =====================================================================================

    @mcp.tool()
    def widget_anim_list(widget_path: str) -> dict:
        """Lists a Widget Blueprint's animations with their time range, display rate and how
        many widgets each one animates. The names returned here are what every other
        widget_anim_* tool takes as animation_name."""
        return conn.call("widget.anim_list", _bp(widget_path))

    @mcp.tool()
    def widget_anim_create(
        widget_path: str,
        animation_name: str,
        start_time: float = 0.0,
        end_time: float = 5.0,
        frame_rate: float = 60.0,
    ) -> dict:
        """Creates an animation on a Widget Blueprint. Times are in seconds. The animation
        becomes a blueprint variable, so the name must not collide with a widget or another
        animation. This is step 1 of anim_create -> anim_bind_widget -> anim_track_add ->
        anim_key_add — an animation with no bound widgets and no tracks plays but changes
        nothing."""
        params = _anim(widget_path, animation_name)
        params["start_time"] = start_time
        params["end_time"] = end_time
        params["frame_rate"] = frame_rate
        return conn.call("widget.anim_create", params)

    @mcp.tool()
    def widget_anim_remove(widget_path: str, animation_name: str) -> dict:
        """Deletes an animation and its blueprint variable. Graph nodes that played it are
        left orphaned and will show up as errors on the next compile."""
        return conn.call("widget.anim_remove", _anim(widget_path, animation_name))

    @mcp.tool()
    def widget_anim_rename(widget_path: str, animation_name: str, new_name: str) -> dict:
        """Renames an animation and retargets the graph nodes that reference it. Fails if the
        name is taken by a widget or another animation."""
        if not new_name:
            raise ValueError("new_name is required")
        return conn.call(
            "widget.anim_rename", {**_anim(widget_path, animation_name), "new_name": new_name}
        )

    @mcp.tool()
    def widget_anim_set_range(
        widget_path: str,
        animation_name: str,
        start_time: float | None = None,
        end_time: float | None = None,
        frame_rate: float | None = None,
    ) -> dict:
        """Adjusts an animation's playback range (seconds) and display rate (fps). Omitted
        arguments are left alone. Shrinking the range does not delete keys outside it — they
        stay in the sections but are no longer played."""
        params = _anim(widget_path, animation_name)
        if start_time is not None:
            params["start_time"] = start_time
        if end_time is not None:
            params["end_time"] = end_time
        if frame_rate is not None:
            params["frame_rate"] = frame_rate
        return conn.call("widget.anim_set_range", params)

    # =====================================================================================
    # WIDGET BINDINGS
    # =====================================================================================

    @mcp.tool()
    def widget_anim_binding_list(widget_path: str, animation_name: str) -> dict:
        """Lists the widgets an animation can animate, with the GUID each is bound by and how
        many tracks it has. "slot_widget_name" non-empty means the binding targets the
        widget's layout slot rather than the widget itself."""
        return conn.call("widget.anim_binding_list", _anim(widget_path, animation_name))

    @mcp.tool()
    def widget_anim_bind_widget(
        widget_path: str, animation_name: str, widget_name: str, bind_slot: bool = False
    ) -> dict:
        """Makes a widget animatable by this animation — step 2 of anim_create ->
        anim_bind_widget -> anim_track_add -> anim_key_add.
        bind_slot false binds the widget itself, which is what you want for colour, opacity,
        visibility and render transform. bind_slot true binds the widget's slot in its parent
        panel instead, which is how position and size are animated (a CanvasPanelSlot's
        LayoutData, a box slot's Padding). A widget can carry both bindings. Returns the
        binding GUID; already-bound widgets return "already_bound": true."""
        if not widget_name:
            raise ValueError("widget_name is required")
        params = _anim(widget_path, animation_name)
        params["widget_name"] = widget_name
        params["bind_slot"] = bind_slot
        return conn.call("widget.anim_bind_widget", params)

    @mcp.tool()
    def widget_anim_unbind_widget(widget_path: str, animation_name: str, widget_name: str) -> dict:
        """Removes a widget from an animation, deleting every track and key that targeted
        it."""
        if not widget_name:
            raise ValueError("widget_name is required")
        return conn.call(
            "widget.anim_unbind_widget",
            {**_anim(widget_path, animation_name), "widget_name": widget_name},
        )

    # =====================================================================================
    # TRACKS AND SECTIONS
    # =====================================================================================

    @mcp.tool()
    def widget_anim_track_list(widget_path: str, animation_name: str, widget_name: str = "") -> dict:
        """Lists an animation's tracks with their property, sections and keyframe channels.
        The channel names reported here ("R", "G", "Translation.X", ...) are what
        widget_anim_key_add addresses. widget_name filters to one bound widget."""
        params = _anim(widget_path, animation_name)
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
        """Adds a property track for a bound widget — step 3 of anim_create ->
        anim_bind_widget -> anim_track_add -> anim_key_add. The
        widget must already be bound (widget_anim_bind_widget). property is a property on the
        bound object: on the widget itself for a normal binding ("RenderOpacity",
        "ColorAndOpacity", "RenderTransform", "Visibility"), or on its slot for a slot
        binding ("LayoutData", "Padding", "Size").

        The track type is inferred from the property's type and covers everything UMG
        animates — pass track_class only for a custom track. create_section true (the
        default) adds a section spanning the playback range and returns its channels, which
        is what you need before keying."""
        params = _track(widget_path, animation_name, widget_name, property)
        if track_class:
            params["track_class"] = track_class
        params["create_section"] = create_section
        return conn.call("widget.anim_track_add", params)

    @mcp.tool()
    def widget_anim_track_remove(
        widget_path: str, animation_name: str, widget_name: str, property: str
    ) -> dict:
        """Deletes a property track and all of its sections and keys."""
        return conn.call(
            "widget.anim_track_remove", _track(widget_path, animation_name, widget_name, property)
        )

    @mcp.tool()
    def widget_anim_section_add(
        widget_path: str,
        animation_name: str,
        widget_name: str,
        property: str,
        start_time: float,
        end_time: float,
        row_index: int = 0,
    ) -> dict:
        """Adds another section to an existing track, for animations that need separate
        stretches of the same property (a blink, a pause, a second pass). Times are in
        seconds. row_index stacks overlapping sections on separate rows. Most tracks only
        need the one section widget_anim_track_add already created."""
        params = _track(widget_path, animation_name, widget_name, property)
        params["start_time"] = start_time
        params["end_time"] = end_time
        params["row_index"] = row_index
        return conn.call("widget.anim_section_add", params)

    @mcp.tool()
    def widget_anim_key_add(
        widget_path: str,
        animation_name: str,
        widget_name: str,
        property: str,
        time: float,
        value: float,
        channel: str = "",
        channel_index: int = 0,
        section_index: int = 0,
        interpolation: str = "cubic",
    ) -> dict:
        """Writes a keyframe — step 4 of anim_create -> anim_bind_widget -> anim_track_add ->
        anim_key_add. time is in seconds, value is the channel's numeric value (pass 1/0 for
        a bool channel, the enum's integer for Visibility).

        A track has one channel per animatable number: RenderOpacity has a single channel, a
        colour has four ("Color.R".."Color.A"), a 2D transform has seven ("Translation.X",
        "Translation.Y", "Angle", "Scale.X", "Scale.Y", "Shear.X", "Shear.Y"). Name the one
        you want with channel, using the "name" widget_anim_track_list reports; the leaf on
        its own works when unambiguous ("R" resolves, "X" on a transform does not). Leave it
        empty and use channel_index for single-channel tracks.

        interpolation is "cubic" (default, smooth), "linear", or "constant" (step). Discrete
        channels — bool, int, enum — ignore it. A key placed outside the section's range
        extends the section to include it."""
        params = _track(widget_path, animation_name, widget_name, property)
        params["time"] = time
        params["value"] = value
        if channel:
            params["channel"] = channel
        params["channel_index"] = channel_index
        params["section_index"] = section_index
        params["interpolation"] = interpolation
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
        """Lists the keyframe times on one channel, in both frames and seconds. Use it to
        verify what widget_anim_key_add wrote."""
        params = _track(widget_path, animation_name, widget_name, property)
        if channel:
            params["channel"] = channel
        params["channel_index"] = channel_index
        params["section_index"] = section_index
        return conn.call("widget.anim_key_list", params)
