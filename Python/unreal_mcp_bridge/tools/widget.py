from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_widget_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    """Widget Blueprint authoring: the asset, its widget tree, bindings, and UI components.

    Everything here goes through the editor's own UMG utilities (FWidgetBlueprintEditorUtils and
    friends), which is the difference that matters. UWidgetTree is reachable from Python, but the
    Python route skips the name-collision handling, the reference retargeting, and the
    blueprint-structure notifications the editor does around every edit — so a tree built that way
    looks right when read back and then compiles into something else.

    The animation half of a widget blueprint lives in widgetanim.py."""

    # ---------------------------------------------------------------- blueprint level

    @mcp.tool()
    def widget_create(
        package_path: str,
        asset_name: str,
        parent_class: str = "",
        root_widget_class: str = "",
    ) -> dict:
        """Creates a new Widget Blueprint asset.

        package_path is the content folder ("/Game/UI"), asset_name the asset name. parent_class
        defaults to UUserWidget and takes either a native class path ("/Script/UMG.UserWidget") or
        another Widget Blueprint ("/Game/UI/WBP_Base.WBP_Base"). root_widget_class optionally seeds
        the tree with a root panel, saving a widget_add call.

        Returns {"ok", "object_path", "parent_class", "root_widget"}."""
        if not package_path:
            raise ValueError("package_path is required")
        if not asset_name:
            raise ValueError("asset_name is required")
        params: dict = {"package_path": package_path, "asset_name": asset_name}
        if parent_class:
            params["parent_class"] = parent_class
        if root_widget_class:
            params["root_widget_class"] = root_widget_class
        return conn.call("widget.create", params)

    @mcp.tool()
    def widget_get_info(widget_path: str) -> dict:
        """Asset-level summary of a Widget Blueprint — read this first.

        Returns {"widget_blueprint", "parent_class", "palette_category", "widget_count",
        "animation_count", "binding_count", "property_bindings_allowed",
        "can_call_initialized_without_player_context", "inherited_named_slots",
        "exposed_named_slots", "root_widget", "root_widget_class"}.

        exposed_named_slots is read from the widget tree rather than the generated class, so it is
        right before the next compile — the generated class only catches up afterwards."""
        if not widget_path:
            raise ValueError("widget_path is required")
        return conn.call("widget.get_info", {"widget_path": widget_path})

    @mcp.tool()
    def widget_set_property(
        widget_path: str,
        property: str,
        value: str,
        target: str = "blueprint",
    ) -> dict:
        """Reflection write on the Widget Blueprint asset itself or on its CDO.

        target="blueprint" writes asset-level settings (PaletteCategory, for instance);
        target="cdo" writes the class defaults every instance starts from.

        value is the same text form the editor's property system uses, so structs go in as
        "(X=10,Y=20)" and enums as their entry name — exactly what widget_list_blueprint_properties
        reports back. Returns {"ok"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not property:
            raise ValueError("property is required")
        return conn.call("widget.set_property", {
            "widget_path": widget_path,
            "property": property,
            "value": value,
            "target": target,
        })

    @mcp.tool()
    def widget_list_blueprint_properties(widget_path: str, target: str = "blueprint") -> dict:
        """Lists the editable properties on the asset (target="blueprint") or its CDO (target="cdo").

        Returns {"target_class", "properties": [{"name", "type", "value"}]}. The reported value is
        accepted back verbatim by widget_set_property."""
        if not widget_path:
            raise ValueError("widget_path is required")
        return conn.call("widget.list_blueprint_properties", {
            "widget_path": widget_path,
            "target": target,
        })

    @mcp.tool()
    def widget_set_desired_focus(widget_path: str, widget_name: str = "") -> dict:
        """Sets which widget takes focus when this user widget is activated — the gamepad entry point
        for a menu. Pass an empty widget_name to clear it. Returns {"ok"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        return conn.call("widget.set_desired_focus", {
            "widget_path": widget_path,
            "widget_name": widget_name,
        })

    @mcp.tool()
    def widget_compile(widget_path: str) -> dict:
        """Compiles the Widget Blueprint and reports the result.

        Returns {"ok", "compiled", "has_warnings", "status"}. Tree edits are not visible to the
        generated class (or to anything reading it, like the exposed named slots of a consumer)
        until this runs, so compile before asset_save."""
        if not widget_path:
            raise ValueError("widget_path is required")
        return conn.call("widget.compile", {"widget_path": widget_path})

    @mcp.tool()
    def widget_validate(widget_path: str) -> dict:
        """Runs UMG's own asset validation: circular references, name conflicts with an inherited
        widget tree, and slate widget leaks.

        Returns {"valid", "issues": [...]}. Worth calling after a scripted restructure — a circular
        reference is the kind of thing that only surfaces as an editor hang later."""
        if not widget_path:
            raise ValueError("widget_path is required")
        return conn.call("widget.validate", {"widget_path": widget_path})

    @mcp.tool()
    def widget_palette_list(include_blueprints: bool = True, name_filter: str = "") -> dict:
        """Lists the widget classes that can be placed in a tree — the editor's palette.

        name_filter is a substring match. include_blueprints=false narrows it to native UMG classes.

        Returns {"count", "classes": [{"name", "class", "category", "is_panel", "is_user_widget",
        "is_blueprint"}]}. is_panel tells you whether a class can take children at all, and
        can_have_multiple_children on widget_get_widget tells you how many."""
        params: dict = {"include_blueprints": include_blueprints}
        if name_filter:
            params["name_filter"] = name_filter
        return conn.call("widget.palette_list", params)

    # ---------------------------------------------------------------- widget tree

    @mcp.tool()
    def widget_tree_list(widget_path: str, start_widget: str = "", max_depth: int = -1) -> dict:
        """Walks the widget hierarchy.

        start_widget limits the walk to one subtree; max_depth=-1 is unlimited.

        Returns {"count", "tree_depth", "widgets": [{"name", "class", "depth", "named_slot",
        "child_count"}]} in tree order. named_slot names the slot a widget occupies when it was
        placed through one, which is how a named-slot child is told apart from a panel child."""
        if not widget_path:
            raise ValueError("widget_path is required")
        params: dict = {"widget_path": widget_path, "max_depth": max_depth}
        if start_widget:
            params["start_widget"] = start_widget
        return conn.call("widget.tree_list", params)

    @mcp.tool()
    def widget_get_widget(widget_path: str, widget_name: str) -> dict:
        """One widget's identity, parent, slot and children.

        Returns {"name", "class", "is_root", "index", "children", "slot_class_for_children",
        "can_have_multiple_children", "content", "named_slots"}.

        slot_class_for_children is what decides which slot properties widget_set_slot_property will
        accept on a child — a CanvasPanelSlot takes anchors, a VerticalBoxSlot does not."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        return conn.call("widget.get_widget", {
            "widget_path": widget_path,
            "widget_name": widget_name,
        })

    @mcp.tool()
    def widget_add(
        widget_path: str,
        widget_class: str,
        parent_widget: str = "",
        child_index: int = -1,
        widget_name: str = "",
    ) -> dict:
        """Instantiates a widget class into the tree.

        widget_class takes a native class path ("/Script/UMG.Button") or a Widget Blueprint path
        ("/Game/UI/WBP_Card.WBP_Card"); the "_C" suffix form is accepted too.

        parent_widget may be omitted only when the tree is empty, in which case the new widget
        becomes the root. child_index=-1 appends. widget_name renames it after insertion.

        Goes through the same FWidgetTemplateClass path the palette uses, which resolves name
        collisions against inherited widget trees and calls CreatedFromPalette — several widgets
        depend on that for their initial designer state, so a widget constructed any other way can
        come out subtly different.

        Returns {"ok", "name", "class", "is_root"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_class:
            raise ValueError("widget_class is required")
        params: dict = {
            "widget_path": widget_path,
            "widget_class": widget_class,
            "child_index": child_index,
        }
        if parent_widget:
            params["parent_widget"] = parent_widget
        if widget_name:
            params["widget_name"] = widget_name
        return conn.call("widget.add", params)

    @mcp.tool()
    def widget_remove(widget_path: str, widget_name: str) -> dict:
        """Deletes a widget and everything under it. Returns {"ok"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        return conn.call("widget.remove", {
            "widget_path": widget_path,
            "widget_name": widget_name,
        })

    @mcp.tool()
    def widget_rename(widget_path: str, widget_name: str, new_name: str) -> dict:
        """Renames a widget and retargets the graph nodes that referenced it as a variable.

        That retargeting is the whole point — renaming through Python leaves every Get node in the
        event graph pointing at a name that no longer exists, and the blueprint only complains at
        the next compile. Returns {"ok", "name"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        if not new_name:
            raise ValueError("new_name is required")
        return conn.call("widget.rename", {
            "widget_path": widget_path,
            "widget_name": widget_name,
            "new_name": new_name,
        })

    @mcp.tool()
    def widget_move(
        widget_path: str,
        widget_name: str,
        new_parent: str,
        child_index: int = -1,
    ) -> dict:
        """Reparents or reorders a widget. child_index=-1 appends; passing the current parent with a
        different index is how a widget is reordered inside its panel.

        The slot is rebuilt for the new parent's slot class, so layout set on the old slot does not
        survive the move — read widget_list_slot_properties afterwards. Returns {"ok"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        if not new_parent:
            raise ValueError("new_parent is required")
        return conn.call("widget.move", {
            "widget_path": widget_path,
            "widget_name": widget_name,
            "new_parent": new_parent,
            "child_index": child_index,
        })

    @mcp.tool()
    def widget_replace(widget_path: str, widget_name: str, widget_class: str) -> dict:
        """Swaps a widget for a different class in place, keeping children and keeping references
        wherever the new class has a matching member. A TextBlock becoming a RichTextBlock keeps its
        graph wiring; a Button becoming a Border does not keep OnClicked. Returns {"ok"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        if not widget_class:
            raise ValueError("widget_class is required")
        return conn.call("widget.replace", {
            "widget_path": widget_path,
            "widget_name": widget_name,
            "widget_class": widget_class,
        })

    @mcp.tool()
    def widget_replace_with_child(widget_path: str, widget_name: str) -> dict:
        """Collapses a single-child panel into its child — the inverse of widget_wrap. Returns {"ok"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        return conn.call("widget.replace_with_child", {
            "widget_path": widget_path,
            "widget_name": widget_name,
        })

    @mcp.tool()
    def widget_replace_with_named_slot(widget_path: str, widget_name: str, slot_name: str) -> dict:
        """Collapses a named-slot host into the content of one of its slots. Returns {"ok"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        if not slot_name:
            raise ValueError("slot_name is required")
        return conn.call("widget.replace_with_named_slot", {
            "widget_path": widget_path,
            "widget_name": widget_name,
            "slot_name": slot_name,
        })

    @mcp.tool()
    def widget_wrap(widget_path: str, widget_names: list[str], widget_class: str) -> dict:
        """Puts widgets inside a newly created panel of widget_class, in place.

        Wrapping several siblings at once gives them one shared wrapper; widgets from different
        parents each get their own. Returns {"ok", "wrappers": [names]}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_names:
            raise ValueError("widget_names (non-empty) is required")
        if not widget_class:
            raise ValueError("widget_class is required")
        return conn.call("widget.wrap", {
            "widget_path": widget_path,
            "widget_names": widget_names,
            "widget_class": widget_class,
        })

    @mcp.tool()
    def widget_set_is_variable(widget_path: str, widget_name: str, is_variable: bool) -> dict:
        """Exposes (or hides) a widget as a blueprint variable.

        A widget that is not a variable cannot be referenced from the event graph, cannot be bound
        with widget_bind_event, and is not animatable — so this is usually the first call after
        widget_add for anything the game touches. Returns {"ok", "is_variable"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        return conn.call("widget.set_is_variable", {
            "widget_path": widget_path,
            "widget_name": widget_name,
            "is_variable": is_variable,
        })

    @mcp.tool()
    def widget_set_root(widget_path: str, widget_name: str = "") -> dict:
        """Makes an existing widget the tree root. An empty widget_name clears the root, leaving the
        tree empty. Returns {"ok"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        return conn.call("widget.set_root", {
            "widget_path": widget_path,
            "widget_name": widget_name,
        })

    @mcp.tool()
    def widget_duplicate(widget_path: str, widget_name: str, parent_widget: str = "") -> dict:
        """Copies a widget subtree back into the same blueprint, under parent_widget (default: the
        source widget's own parent).

        Runs through the clipboard text format, the same round trip the editor's own duplicate uses,
        so slot data survives and the copies get fresh unique names rather than collisions.

        Returns {"ok", "widgets": [names]} — the new subtree roots."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        params: dict = {"widget_path": widget_path, "widget_name": widget_name}
        if parent_widget:
            params["parent_widget"] = parent_widget
        return conn.call("widget.duplicate", params)

    # ---------------------------------------------------------------- properties

    @mcp.tool()
    def widget_list_properties(
        widget_path: str,
        widget_name: str,
        editable_only: bool = True,
    ) -> dict:
        """Lists the properties on one widget.

        Returns {"widget", "class", "properties": [{"name", "type", "value"}]}. Values come back in
        the text form widget_set_widget_property accepts, so read-then-write round-trips."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        return conn.call("widget.list_properties", {
            "widget_path": widget_path,
            "widget_name": widget_name,
            "editable_only": editable_only,
        })

    @mcp.tool()
    def widget_set_widget_property(
        widget_path: str,
        widget_name: str,
        property: str,
        value: str,
    ) -> dict:
        """Reflection write on a widget. value is the editor's text form — "(R=1,G=0,B=0,A=1)" for a
        color, an enumerator name for an enum, a text literal for FText. Returns {"ok"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        if not property:
            raise ValueError("property is required")
        return conn.call("widget.set_widget_property", {
            "widget_path": widget_path,
            "widget_name": widget_name,
            "property": property,
            "value": value,
        })

    @mcp.tool()
    def widget_list_slot_properties(
        widget_path: str,
        widget_name: str,
        editable_only: bool = True,
    ) -> dict:
        """Lists the layout properties of a widget's slot in its parent panel — anchors and offsets on
        a canvas, size rule and padding in a box.

        These live on the slot object, not the widget, which is why they do not appear in
        widget_list_properties. Returns {"widget", "slot_class", "properties": [...]}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        return conn.call("widget.list_slot_properties", {
            "widget_path": widget_path,
            "widget_name": widget_name,
            "editable_only": editable_only,
        })

    @mcp.tool()
    def widget_set_slot_property(
        widget_path: str,
        widget_name: str,
        property: str,
        value: str,
    ) -> dict:
        """Reflection write on a widget's slot — the layout half of positioning a widget.

        The owning panel is re-synchronised afterwards; a slot write that skips that reaches the
        stored data but not the live Slate layout, so the designer keeps drawing the old position.
        Returns {"ok"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        if not property:
            raise ValueError("property is required")
        return conn.call("widget.set_slot_property", {
            "widget_path": widget_path,
            "widget_name": widget_name,
            "property": property,
            "value": value,
        })

    # ---------------------------------------------------------------- named slots

    @mcp.tool()
    def widget_named_slot_list(widget_path: str, host_widget: str = "") -> dict:
        """Lists named slots and their content — on host_widget, or on the widget tree itself when
        host_widget is omitted (which is how a blueprint fills the slots its parent class declares).

        Only three things host named slots: a nested UUserWidget, an ExpandableArea, and the widget
        tree. A NamedSlot widget is not one of them — it *declares* a slot for consumers of this
        blueprint, and locally its content is an ordinary child, so point tree commands at it rather
        than these.

        Returns {"host", "slots": [{"name", "content", "content_class"}]}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        params: dict = {"widget_path": widget_path}
        if host_widget:
            params["host_widget"] = host_widget
        return conn.call("widget.named_slot_list", params)

    @mcp.tool()
    def widget_named_slot_set_content(
        widget_path: str,
        slot_name: str,
        content_widget: str,
        host_widget: str = "",
    ) -> dict:
        """Places an existing widget into a named slot.

        The widget is detached from a panel parent first — it can only live in one place, and a tree
        that reaches the same widget by two routes is the kind of corruption that survives a save.
        Returns {"ok"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not slot_name:
            raise ValueError("slot_name is required")
        if not content_widget:
            raise ValueError("content_widget is required")
        params: dict = {
            "widget_path": widget_path,
            "slot_name": slot_name,
            "content_widget": content_widget,
        }
        if host_widget:
            params["host_widget"] = host_widget
        return conn.call("widget.named_slot_set_content", params)

    @mcp.tool()
    def widget_named_slot_clear(
        widget_path: str,
        slot_name: str,
        host_widget: str = "",
        delete_content: bool = False,
    ) -> dict:
        """Empties a named slot.

        delete_content=false (the default) leaves the widget in the tree but unparented — fine when
        you are about to place it somewhere else, an orphan otherwise. Pass true to delete it.

        Returns {"ok", "previous_content"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not slot_name:
            raise ValueError("slot_name is required")
        params: dict = {
            "widget_path": widget_path,
            "slot_name": slot_name,
            "delete_content": delete_content,
        }
        if host_widget:
            params["host_widget"] = host_widget
        return conn.call("widget.named_slot_clear", params)

    # ---------------------------------------------------------------- bindings and events

    @mcp.tool()
    def widget_binding_list(widget_path: str) -> dict:
        """Lists the property bindings declared on the blueprint — the per-frame "get this value from
        a function" arrows shown next to a property in the Details panel.

        Returns {"count", "bindings": [{"widget_name", "property", "kind", "function_name",
        "source_property", "target_exists"}]}. target_exists=false means the binding survived the
        deletion of what it pointed at and evaluates to nothing at runtime."""
        if not widget_path:
            raise ValueError("widget_path is required")
        return conn.call("widget.binding_list", {"widget_path": widget_path})

    @mcp.tool()
    def widget_binding_add(
        widget_path: str,
        widget_name: str,
        property: str,
        source: str,
        kind: str = "function",
    ) -> dict:
        """Binds a widget property to a blueprint function or variable on the user widget.

        kind="function" points at a function name, kind="property" at a variable name. An existing
        binding on the same widget+property is overwritten.

        The source is resolved against the skeleton class, which exists as soon as a member is
        declared — so a function added earlier in the same session binds without a full compile
        first. Returns {"ok"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        if not property:
            raise ValueError("property is required")
        if not source:
            raise ValueError("source is required")
        return conn.call("widget.binding_add", {
            "widget_path": widget_path,
            "widget_name": widget_name,
            "property": property,
            "kind": kind,
            "source": source,
        })

    @mcp.tool()
    def widget_binding_remove(widget_path: str, widget_name: str, property: str) -> dict:
        """Drops a property binding. Returns {"ok", "removed"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        if not property:
            raise ValueError("property is required")
        return conn.call("widget.binding_remove", {
            "widget_path": widget_path,
            "widget_name": widget_name,
            "property": property,
        })

    @mcp.tool()
    def widget_list_events(widget_path: str, widget_name: str) -> dict:
        """Lists the multicast delegates on a widget that an event graph node can bind to — OnClicked,
        OnHovered, and so on.

        Returns {"widget", "class", "events": [{"name", "category", "signature"}]}. Feed a name to
        widget_bind_event."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        return conn.call("widget.list_events", {
            "widget_path": widget_path,
            "widget_name": widget_name,
        })

    @mcp.tool()
    def widget_bind_event(widget_path: str, widget_name: str, event_name: str) -> dict:
        """Creates the event graph node for a widget's delegate — what the green buttons in the
        Details panel do.

        The binding is made against the widget's blueprint variable, so the widget has to be exposed
        as one first (widget_set_is_variable). Returns {"ok"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        if not event_name:
            raise ValueError("event_name is required")
        return conn.call("widget.bind_event", {
            "widget_path": widget_path,
            "widget_name": widget_name,
            "event_name": event_name,
        })

    # ---------------------------------------------------------------- UI components

    @mcp.tool()
    def widget_component_add(widget_path: str, widget_name: str, component_class: str) -> dict:
        """Attaches a UIComponent (UE 5.5+ widget extension) to a widget. Returns {"ok", "widget"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        if not component_class:
            raise ValueError("component_class is required")
        return conn.call("widget.component_add", {
            "widget_path": widget_path,
            "widget_name": widget_name,
            "component_class": component_class,
        })

    @mcp.tool()
    def widget_component_remove(widget_path: str, widget_name: str, component_class: str) -> dict:
        """Detaches a UIComponent from a widget. Returns {"ok"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        if not component_class:
            raise ValueError("component_class is required")
        return conn.call("widget.component_remove", {
            "widget_path": widget_path,
            "widget_name": widget_name,
            "component_class": component_class,
        })

    @mcp.tool()
    def widget_component_move(
        widget_path: str,
        widget_name: str,
        component_class: str,
        relative_to_class: str,
        move_after: bool = True,
    ) -> dict:
        """Reorders a UIComponent relative to another on the same widget. Order decides evaluation
        order, so it matters wherever two components touch the same property. Returns {"ok"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_name:
            raise ValueError("widget_name is required")
        if not component_class:
            raise ValueError("component_class is required")
        if not relative_to_class:
            raise ValueError("relative_to_class is required")
        return conn.call("widget.component_move", {
            "widget_path": widget_path,
            "widget_name": widget_name,
            "component_class": component_class,
            "relative_to_class": relative_to_class,
            "move_after": move_after,
        })

    # ---------------------------------------------------------------- clipboard text

    @mcp.tool()
    def widget_export_text(widget_path: str, widget_names: list[str]) -> dict:
        """Serialises widgets to the editor's clipboard text format.

        The same text the designer's copy produces, so it can be pasted into UMG by hand, diffed, or
        fed back through widget_import_text — including into a different blueprint, which is how a
        laid-out subtree moves between assets. Returns {"ok", "text"}."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not widget_names:
            raise ValueError("widget_names (non-empty) is required")
        return conn.call("widget.export_text", {
            "widget_path": widget_path,
            "widget_names": widget_names,
        })

    @mcp.tool()
    def widget_import_text(widget_path: str, text: str, parent_widget: str = "") -> dict:
        """Recreates widgets from clipboard text, parenting the imported roots under parent_widget
        (omit to leave them unparented in the tree).

        Returns {"ok", "imported_count", "roots": [names]} — names are freshly uniqued, so they will
        not match the ones in the text if those were taken."""
        if not widget_path:
            raise ValueError("widget_path is required")
        if not text:
            raise ValueError("text is required")
        params: dict = {"widget_path": widget_path, "text": text}
        if parent_widget:
            params["parent_widget"] = parent_widget
        return conn.call("widget.import_text", params)

    @mcp.tool()
    def widget_mvvm_set_binding_mode(widget_path: str, binding_id: str, mode: str) -> dict:
        """UMG Viewmodel (MVVM) authoring that the official MVVM toolset lacks: binding mode / execution mode / enabled flags,
        editing binding paths and conversion-function arguments, viewmodel removal / creation type / options / rename,
        viewmodel Blueprint property removal and defaults, view events and conditions, and a full JSON dump.
        Works through UMVVMEditorSubsystem like the MVVM editor panels do.
        
        Ids (interoperable with the official MVVM toolset):
         - BindingId = FMVVMBlueprintViewBinding::BindingId, written "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" (same as
           ListWidgetViewBindings / CreateViewBinding). Any FGuid text form is accepted.
         - ViewModelName = the viewmodel property name on the widget (FMVVMBlueprintViewModelContext::ViewModelName). The viewmodel
           id (GUID) or the viewmodel class name/path (what the official toolset uses) is also accepted when unambiguous.
         - Event / condition id = the object name of the UMVVMBlueprintViewEvent / UMVVMBlueprintViewCondition (see DumpWidgetMVVM).
        Path syntax (same contexts as the official CreateViewBinding, written as one string): "<Context>.<Field>[.<Field>...]"
         where Context is a viewmodel name (or class name), a widget name from the widget tree, or "Self" (the widget blueprint).
         Fields are properties or functions, e.g. "VM_Player.Health", "Txt_Score.Text", "Self.SetScoreText", "VM_Player.Stats.Level".
        Literal values use the UE text-import format of the pin type, e.g. "true", "3", "Hello", "(R=1,G=0,B=0,A=1)".
        Edits are transacted and mark the Blueprint modified like the editor; they do not compile unless stated
        (use CompileWidgetMVVM). Nothing is saved; save with the official asset tools.
        /
        UCLASS(BlueprintType)
        class BLUEPRINTINSPECTOR_API UBPIMVVMToolset : public UToolsetDefinition
        {
        	GENERATED_BODY()
        
        public:
        	virtual FString GetToolsetVersion() const override { return TEXT("1.0"); }
        
        	// ---- bindings ----
        
        	/**
        Set the binding mode (direction) of a view binding (UMVVMEditorSubsystem::SetBindingTypeForBinding, like the binding row combo).
        widget_path: Widget Blueprint asset path, e.g. "/Game/UI/WBP_HUD".
        binding_id: Binding id (from ListWidgetViewBindings, CreateViewBinding or DumpWidgetMVVM).
        mode: OneTimeToDestination, OneWayToDestination, TwoWay or OneWayToSource ("ToWidget"/"ToViewModel" aliases accepted).
        Returns: "OK" or "ERROR|...". Not compiled; the compiler reports unsupported directions (e.g. TwoWay to a read-only field).

        Built-in port of BlueprintInspector's BPIMVVMToolset::SetBindingMode; no other plugin needed."""
        return conn.call("widget.mvvm_set_binding_mode", {"widget_path": widget_path, "binding_id": binding_id, "mode": mode})

    @mcp.tool()
    def widget_mvvm_set_binding_execution_mode(widget_path: str, binding_id: str, execution_mode: str) -> dict:
        """Override or reset the execution mode of a view binding.
        execution_mode: Default (remove the override), Immediate, Delayed, Tick or DelayedWhenSharedElseImmediate (alias "Auto").
         Modes not in the project's MVVM "Allowed Execution Mode" setting are rejected.
        Returns: "OK" or "ERROR|...".

        Built-in port of BlueprintInspector's BPIMVVMToolset::SetBindingExecutionMode; no other plugin needed."""
        return conn.call("widget.mvvm_set_binding_execution_mode", {"widget_path": widget_path, "binding_id": binding_id, "execution_mode": execution_mode})

    @mcp.tool()
    def widget_mvvm_set_binding_enabled(widget_path: str, binding_id: str, enabled: bool, compile: bool) -> dict:
        """Set the Enabled and Compile flags of a view binding.
        enabled: Binding enabled by default (instances may still enable it at runtime).
        compile: False keeps the binding in the editor but excludes it from compilation.
        Returns: "OK" or "ERROR|...".

        Built-in port of BlueprintInspector's BPIMVVMToolset::SetBindingEnabled; no other plugin needed."""
        return conn.call("widget.mvvm_set_binding_enabled", {"widget_path": widget_path, "binding_id": binding_id, "enabled": enabled, "compile": compile})

    @mcp.tool()
    def widget_mvvm_update_binding_paths(widget_path: str, binding_id: str, source_path: str, destination_path: str) -> dict:
        """Change the source and/or destination field of an existing binding (keeps its id, mode and flags).
        Refuses to replace a side that is driven by a conversion function (edit it with SetConversionFunctionArgument instead).
        source_path: New source path, e.g. "VM_Player.Health". Empty = unchanged.
        destination_path: New destination path, e.g. "Bar_Health.Percent". Empty = unchanged.
        Returns: JSON {"id","source","destination"} (resolved path text) or "ERROR|...".

        Built-in port of BlueprintInspector's BPIMVVMToolset::UpdateBindingPaths; no other plugin needed."""
        return conn.call("widget.mvvm_update_binding_paths", {"widget_path": widget_path, "binding_id": binding_id, "source_path": source_path, "destination_path": destination_path})

    @mcp.tool()
    def widget_mvvm_set_conversion_function_argument(widget_path: str, binding_id: str, argument_name: str, value_or_path: str, source_to_destination: bool) -> dict:
        """Set an input of a binding's conversion function to a literal value or bind it to a field path.
        argument_name: Conversion function parameter name, e.g. "bUseGrouping" (split pins: "Parent.Child"). See DumpWidgetMVVM.
        value_or_path: "path:<Context>.<Field>" binds the argument to a field ("path:" alone unbinds it);
         anything else is a literal in pin default-value format (a "value:" prefix is stripped), e.g. "false", "2", "Hello".
        source_to_destination: True for the source->destination conversion, false for destination->source.
        Returns: JSON {"argument","value"|"path"} or "ERROR|...".

        Built-in port of BlueprintInspector's BPIMVVMToolset::SetConversionFunctionArgument; no other plugin needed."""
        return conn.call("widget.mvvm_set_conversion_function_argument", {"widget_path": widget_path, "binding_id": binding_id, "argument_name": argument_name, "value_or_path": value_or_path, "source_to_destination": source_to_destination})

    @mcp.tool()
    def widget_mvvm_remove_view_model(widget_path: str, view_model_name: str, remove_referencing_bindings: bool) -> dict:
        """Remove a viewmodel from a widget and report the bindings, events and conditions that referenced it.
        view_model_name: Viewmodel name (or id / class name).
        remove_referencing_bindings: Also remove those bindings, events and conditions (otherwise they are kept and will fail to compile).
        Returns: JSON {"removed":name,"referencingBindings":[{"id","text"}],"referencingEvents":[id],"referencingConditions":[id],
         "referencesRemoved":bool,"usedInGraphs":bool} or "ERROR|...".

        Built-in port of BlueprintInspector's BPIMVVMToolset::RemoveViewModelFromWidget; no other plugin needed."""
        return conn.call("widget.mvvm_remove_view_model", {"widget_path": widget_path, "view_model_name": view_model_name, "remove_referencing_bindings": remove_referencing_bindings})

    @mcp.tool()
    def widget_mvvm_set_view_model_creation_type(widget_path: str, view_model_name: str, creation_type: str, argument: str) -> dict:
        """Change how a widget gets its viewmodel instance. Applies the same side effects as the details panel
        (Manual => setter + optional; Context => optional).
        creation_type: Manual, CreateInstance, GlobalViewModelCollection, PropertyPath, Resolver or Context
         (must be allowed for the viewmodel class and by the project settings).
        argument: GlobalViewModelCollection: global viewmodel identifier (required). PropertyPath: the property/function path
         evaluated on the widget, e.g. "GetOwningPlayer.MyViewModel" (required). Resolver: resolver class path
         ("/Script/Module.MyResolver" or a Blueprint path); empty = project default resolver. Ignored for other types.
        Returns: JSON with the updated viewmodel (same shape as DumpWidgetMVVM "viewModels" entries) or "ERROR|...".

        Built-in port of BlueprintInspector's BPIMVVMToolset::SetViewModelCreationType; no other plugin needed."""
        return conn.call("widget.mvvm_set_view_model_creation_type", {"widget_path": widget_path, "view_model_name": view_model_name, "creation_type": creation_type, "argument": argument})

    @mcp.tool()
    def widget_mvvm_set_view_model_options(widget_path: str, view_model_name: str, expose_instance_in_editor: bool, create_setter_function: bool, optional: bool) -> dict:
        """Set the advanced options of a widget viewmodel.
        expose_instance_in_editor: Expose the instance on every widget instance for editing (only meaningful for CreateInstance).
        create_setter_function: Generate a public setter (always on for Manual).
        optional: Do not warn when the instance is missing (always on for Manual and Context).
        Returns: JSON with the updated viewmodel plus "warnings":[...] or "ERROR|...".

        Built-in port of BlueprintInspector's BPIMVVMToolset::SetViewModelOptions; no other plugin needed."""
        return conn.call("widget.mvvm_set_view_model_options", {"widget_path": widget_path, "view_model_name": view_model_name, "expose_instance_in_editor": expose_instance_in_editor, "create_setter_function": create_setter_function, "optional": optional})

    @mcp.tool()
    def widget_mvvm_rename_view_model(widget_path: str, view_model_name: str, new_name: str) -> dict:
        """Rename a widget viewmodel (renames the generated property and updates graph references, like the viewmodel panel).
        Unloaded Blueprints that reference this widget are loaded first (as the editor does when you answer Yes).
        Returns: JSON {"oldName","newName","loadedReferencers":[paths]} or "ERROR|...".

        Built-in port of BlueprintInspector's BPIMVVMToolset::RenameViewModel; no other plugin needed."""
        return conn.call("widget.mvvm_rename_view_model", {"widget_path": widget_path, "view_model_name": view_model_name, "new_name": new_name})

    @mcp.tool()
    def widget_mvvm_remove_view_model_property(view_model_path: str, property_name: str) -> dict:
        """Remove a member variable (e.g. a FieldNotify property) from a viewmodel Blueprint (a UMVVMViewModelBase subclass).
        Graph nodes using it are removed with it; widget bindings in other assets that used it fail on their next compile.
        view_model_path: Viewmodel Blueprint asset path, e.g. "/Game/UI/VM_Player".
        Returns: JSON {"removed","fieldNotify":bool,"wasUsedInGraphs":bool} or "ERROR|...".

        Built-in port of BlueprintInspector's BPIMVVMToolset::RemoveViewModelProperty; no other plugin needed."""
        return conn.call("widget.mvvm_remove_view_model_property", {"view_model_path": view_model_path, "property_name": property_name})

    @mcp.tool()
    def widget_mvvm_set_view_model_property_default(view_model_path: str, property_name: str, value_text: str) -> dict:
        """Set the default value (class default object) of a viewmodel Blueprint variable.
        If the variable is not on the generated class yet (added but never compiled), the Blueprint is compiled first (not saved).
        value_text: Value in UE text-import format, e.g. "100", "true", "Hello", "(X=1,Y=2,Z=3)".
        Returns: JSON {"property","value","compiled":bool} (value read back) or "ERROR|...".

        Built-in port of BlueprintInspector's BPIMVVMToolset::SetViewModelPropertyDefault; no other plugin needed."""
        return conn.call("widget.mvvm_set_view_model_property_default", {"view_model_path": view_model_path, "property_name": property_name, "value_text": value_text})

    @mcp.tool()
    def widget_mvvm_add_view_event(widget_path: str, event_path: str, destination_function_path: str) -> dict:
        """Add a view event: when a multicast delegate fires, call a function (requires the MVVM "Allow Binding Event" project setting).
        event_path: Path to a multicast delegate property, e.g. "Btn_Buy.OnClicked" or "Self.OnVisibilityChanged".
        destination_function_path: Function to call, e.g. "VM_Shop.Buy" or "Self.HandleClicked". Set its parameters with SetViewEventArgument.
        Returns: JSON {"id","eventPath","destination","arguments":[...]} or "ERROR|...".

        Built-in port of BlueprintInspector's BPIMVVMToolset::AddViewEvent; no other plugin needed."""
        return conn.call("widget.mvvm_add_view_event", {"widget_path": widget_path, "event_path": event_path, "destination_function_path": destination_function_path})

    @mcp.tool()
    def widget_mvvm_set_view_event_argument(widget_path: str, event_id: str, argument_name: str, value_or_path: str) -> dict:
        """Set a parameter of a view event's destination function to a literal or to a field path (same ValueOrPath rules as SetConversionFunctionArgument).
        event_id: Event id from AddViewEvent / DumpWidgetMVVM.
        Returns: JSON {"argument","value"|"path"} or "ERROR|...".

        Built-in port of BlueprintInspector's BPIMVVMToolset::SetViewEventArgument; no other plugin needed."""
        return conn.call("widget.mvvm_set_view_event_argument", {"widget_path": widget_path, "event_id": event_id, "argument_name": argument_name, "value_or_path": value_or_path})

    @mcp.tool()
    def widget_mvvm_remove_view_event(widget_path: str, event_id: str) -> dict:
        """Remove a view event.
        Returns: "OK" or "ERROR|...".

        Built-in port of BlueprintInspector's BPIMVVMToolset::RemoveViewEvent; no other plugin needed."""
        return conn.call("widget.mvvm_remove_view_event", {"widget_path": widget_path, "event_id": event_id})

    @mcp.tool()
    def widget_mvvm_add_view_condition(widget_path: str, condition_path: str, operation: str, value: float, max_value: float, destination_function_path: str) -> dict:
        """Add a view condition: when a numeric field satisfies a comparison, call a function
        (requires the MVVM "Allow Condition Binding" project setting).
        condition_path: Source field, e.g. "VM_Player.Health".
        operation: Equal, NotEqual, MoreThan, MoreThanOrEqual, LessThan, LessThanOrEqual, BetweenInclusive or BetweenExclusive.
        value: Comparison value (lower bound for Between*).
        max_value: Upper bound for Between* (ignored otherwise).
        destination_function_path: Function to call, e.g. "Self.PlayLowHealthAnim".
        Returns: JSON {"id","conditionPath","operation","value","maxValue","destination"} or "ERROR|...".

        Built-in port of BlueprintInspector's BPIMVVMToolset::AddViewCondition; no other plugin needed."""
        return conn.call("widget.mvvm_add_view_condition", {"widget_path": widget_path, "condition_path": condition_path, "operation": operation, "value": value, "max_value": max_value, "destination_function_path": destination_function_path})

    @mcp.tool()
    def widget_mvvm_remove_view_condition(widget_path: str, condition_id: str) -> dict:
        """Remove a view condition.
        Returns: "OK" or "ERROR|...".

        Built-in port of BlueprintInspector's BPIMVVMToolset::RemoveViewCondition; no other plugin needed."""
        return conn.call("widget.mvvm_remove_view_condition", {"widget_path": widget_path, "condition_id": condition_id})

    @mcp.tool()
    def widget_mvvm_dump(widget_path: str) -> dict:
        """Dump the widget's MVVM view with ids usable by these tools and by the official MVVM toolset.
        Returns: JSON {"widget","status","hasView","settings":{...},
         "viewModels":[{"name","id","class","creationType","globalViewModelIdentifier","propertyPath","resolver","instanced",
           "exposeInstanceInEditor","createSetterFunction","createGetterFunction","optional","canRename","canEdit","canRemove","allowedCreationTypes":[...]}],
         "bindings":[{"id","text","source":path,"destination":path,"mode","executionMode","enabled","compile",
           "conversion":{"sourceToDestination":conv?,"destinationToSource":conv?},"messages":[{"severity","text"}]}],
         "events":[{"id","eventPath":path,"destination":path,"enabled","compile","arguments":[pin],"messages":[...]}],
         "conditions":[{"id","conditionPath":path,"operation","value","maxValue","destination":path,"enabled","compile","arguments":[pin],"messages":[...]}]}
         where path = {"valid","spec","text","source","widget"?,"viewModelId"?,"viewModelName"?,"fields":[...]} ("spec" is re-usable as a path argument),
         conv = {"function","type","valid","wrapperGraph","arguments":[pin]} and pin = {"name","id","value","path"?,"split","orphaned","linked"?}.
         Messages come from the last compile in this editor session (see CompileWidgetMVVM). Or "ERROR|...".

        Built-in port of BlueprintInspector's BPIMVVMToolset::DumpWidgetMVVM; no other plugin needed."""
        return conn.call("widget.mvvm_dump", {"widget_path": widget_path})

    @mcp.tool()
    def widget_mvvm_compile(widget_path: str) -> dict:
        """Compile the Widget Blueprint (never saves) and return compiler and per-binding MVVM messages.
        Returns: JSON {"status","errors","warnings","messages":[{"severity","text"}],"bindingMessages":[{"id","kind","severity","text"}]} or "ERROR|...".

        Built-in port of BlueprintInspector's BPIMVVMToolset::CompileWidgetMVVM; no other plugin needed."""
        return conn.call("widget.mvvm_compile", {"widget_path": widget_path})
