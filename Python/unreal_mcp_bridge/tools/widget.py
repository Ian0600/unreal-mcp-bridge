from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection

# Widget and slot classes are addressed by path rather than by a per-type tool: UMG ships 46
# widget classes and 18 slot classes, and any project adds more as UserWidget blueprints.
# Native classes look like "/Script/UMG.Button"; widget blueprints like
# "/Game/UI/WBP_Card.WBP_Card" (the "_C" class form is accepted too).


def register_widget_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    def _bp(widget_path: str) -> dict:
        if not widget_path:
            raise ValueError("widget_path is required")
        return {"widget_path": widget_path}

    def _bp_widget(widget_path: str, widget_name: str) -> dict:
        if not widget_name:
            raise ValueError("widget_name is required")
        params = _bp(widget_path)
        params["widget_name"] = widget_name
        return params

    # =====================================================================================
    # BLUEPRINT LEVEL
    # =====================================================================================

    @mcp.tool()
    def widget_create(
        package_path: str,
        asset_name: str,
        parent_class: str = "",
        root_widget_class: str = "",
    ) -> dict:
        """Creates a new Widget Blueprint asset. package_path is the content folder
        ("/Game/UI"), asset_name the asset ("WBP_MainMenu"). parent_class defaults to
        UserWidget and must derive from it — pass a project base class like
        "/Game/UI/WBP_ActivatableBase.WBP_ActivatableBase" to inherit from a blueprint.
        root_widget_class optionally seeds the tree with a root panel (e.g.
        "/Script/UMG.CanvasPanel"); leave it empty and add the root later with widget_add.
        Returns {"object_path", "parent_class", "root_widget"}."""
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
        """Summarises a Widget Blueprint: parent class, root widget, widget/animation/binding
        counts, palette category, "inherited_named_slots" (slots from the parent class this
        blueprint can fill) and "exposed_named_slots" (slots this blueprint offers to
        blueprints that consume it, one per NamedSlot widget in its tree). Start here before
        other widget_* tools."""
        return conn.call("widget.get_info", _bp(widget_path))

    @mcp.tool()
    def widget_list_blueprint_properties(widget_path: str, target: str = "blueprint") -> dict:
        """Lists editable properties on the asset itself (target "blueprint" — thumbnail
        settings, tick frequency, bCanCallInitializedWithoutPlayerContext) or on the generated
        class default object (target "cdo" — everything a UserWidget subclass declares).
        Reported value strings are the same form widget_set_property accepts back."""
        return conn.call("widget.list_blueprint_properties", {**_bp(widget_path), "target": target})

    @mcp.tool()
    def widget_set_property(widget_path: str, property: str, value: str, target: str = "blueprint") -> dict:
        """Writes a property on the Widget Blueprint asset (target "blueprint") or its class
        default object (target "cdo"). Values use Unreal's property text form: structs as
        "(X=10,Y=20)", enums as their entry name, bools as "true"/"false". The CDO target
        requires the blueprint to have been compiled at least once."""
        if not property:
            raise ValueError("property is required")
        return conn.call(
            "widget.set_property",
            {**_bp(widget_path), "property": property, "value": value, "target": target},
        )

    @mcp.tool()
    def widget_set_desired_focus(widget_path: str, widget_name: str = "") -> dict:
        """Sets which widget receives focus when this user widget is activated — the Desired
        Focus field in the designer. Pass an empty widget_name to clear it."""
        params = _bp(widget_path)
        if widget_name:
            params["widget_name"] = widget_name
        return conn.call("widget.set_desired_focus", params)

    @mcp.tool()
    def widget_compile(widget_path: str) -> dict:
        """Compiles the Widget Blueprint. Widget trees, animations and bindings are all
        compile-time constructs, so call this after a batch of edits before testing in PIE.
        Returns {"compiled", "has_warnings", "status"}."""
        return conn.call("widget.compile", _bp(widget_path))

    @mcp.tool()
    def widget_validate(widget_path: str) -> dict:
        """Checks the widget tree for circular references (a widget blueprint containing
        itself) and for widget names that collide with widgets inherited from the parent
        class. Both fail the compile, so this is the cheap pre-check. Returns
        {"valid", "issues"}."""
        return conn.call("widget.validate", _bp(widget_path))

    @mcp.tool()
    def widget_palette_list(include_blueprints: bool = True, name_filter: str = "") -> dict:
        """Lists widget classes that can be placed in a tree, with palette category and
        whether each is a panel (can hold children), a user widget, or a blueprint class.
        Use name_filter to narrow a long list. These class paths are what widget_add,
        widget_replace and widget_wrap take."""
        params: dict = {"include_blueprints": include_blueprints}
        if name_filter:
            params["name_filter"] = name_filter
        return conn.call("widget.palette_list", params)

    # =====================================================================================
    # WIDGET TREE
    # =====================================================================================

    @mcp.tool()
    def widget_tree_list(widget_path: str, start_widget: str = "", max_depth: int = -1) -> dict:
        """Walks the widget hierarchy depth-first, returning each widget with its depth,
        class, parent, slot type, variable flag and child count. Named slot content is
        included and tagged with "named_slot". start_widget limits the walk to one subtree;
        max_depth -1 is unlimited, 0 is the start widget alone. This is the map the other
        tree tools navigate by name."""
        params = _bp(widget_path)
        if start_widget:
            params["start_widget"] = start_widget
        params["max_depth"] = max_depth
        return conn.call("widget.tree_list", params)

    @mcp.tool()
    def widget_get_widget(widget_path: str, widget_name: str) -> dict:
        """Details one widget: class, parent, slot class, whether it is the tree root or a
        blueprint variable, its children in order (for panels), and its named slots and their
        content (for named slot hosts)."""
        return conn.call("widget.get_widget", _bp_widget(widget_path, widget_name))

    @mcp.tool()
    def widget_add(
        widget_path: str,
        widget_class: str,
        parent_widget: str = "",
        child_index: int = -1,
        widget_name: str = "",
    ) -> dict:
        """Creates a widget and inserts it into the tree. widget_class is a class path —
        "/Script/UMG.Button" for native classes, "/Game/UI/WBP_Card.WBP_Card" for widget
        blueprints. Leave parent_widget empty to set the tree root when the tree is still
        empty; otherwise it names the panel to add into. child_index -1 appends.
        widget_name optionally renames the widget after insertion — it is ignored (rather
        than failing) if the name is already taken. Panels reject children they cannot hold,
        so check "is_panel" and "can_have_multiple_children" from widget_get_widget first."""
        if not widget_class:
            raise ValueError("widget_class is required")
        params = _bp(widget_path)
        params["widget_class"] = widget_class
        if parent_widget:
            params["parent_widget"] = parent_widget
        params["child_index"] = child_index
        if widget_name:
            params["widget_name"] = widget_name
        return conn.call("widget.add", params)

    @mcp.tool()
    def widget_remove(widget_path: str, widget_name: str) -> dict:
        """Deletes a widget and its whole subtree, cleaning up graph nodes that referenced it
        and clearing it from Desired Focus. To keep a widget but hide it, set its Visibility
        with widget_set_widget_property instead."""
        return conn.call("widget.remove", _bp_widget(widget_path, widget_name))

    @mcp.tool()
    def widget_rename(widget_path: str, widget_name: str, new_name: str) -> dict:
        """Renames a widget and retargets the blueprint variable and every graph node that
        referenced it. Fails cleanly if the name is taken by another widget, an animation, or
        a member of the parent class."""
        if not new_name:
            raise ValueError("new_name is required")
        return conn.call("widget.rename", {**_bp_widget(widget_path, widget_name), "new_name": new_name})

    @mcp.tool()
    def widget_move(widget_path: str, widget_name: str, new_parent: str, child_index: int = -1) -> dict:
        """Reparents a widget, or reorders it within its current parent by passing the same
        parent with a different child_index (-1 appends). new_parent must be a panel widget.
        Slot properties are preserved where the new slot type accepts them."""
        if not new_parent:
            raise ValueError("new_parent is required")
        params = _bp_widget(widget_path, widget_name)
        params["new_parent"] = new_parent
        params["child_index"] = child_index
        return conn.call("widget.move", params)

    @mcp.tool()
    def widget_replace(widget_path: str, widget_name: str, widget_class: str) -> dict:
        """Swaps a widget for a new instance of a different class, keeping the name and
        retargeting graph references to members that exist on the new class (references to
        members that don't are left orphaned so they're visible in the graph). Replacing a
        panel that has children requires a panel class that can hold all of them. Aborts
        without side effects if validation fails."""
        if not widget_class:
            raise ValueError("widget_class is required")
        return conn.call("widget.replace", {**_bp_widget(widget_path, widget_name), "widget_class": widget_class})

    @mcp.tool()
    def widget_replace_with_child(widget_path: str, widget_name: str) -> dict:
        """Removes a single-child panel and promotes its child into its place — the designer's
        "Replace With Child". The named widget must be a panel with exactly one child."""
        return conn.call("widget.replace_with_child", _bp_widget(widget_path, widget_name))

    @mcp.tool()
    def widget_replace_with_named_slot(widget_path: str, widget_name: str, slot_name: str) -> dict:
        """Removes a named slot host widget and promotes the content of one of its slots into
        its place. widget_name must be a nested user widget or an ExpandableArea — see
        widget_named_slot_list for what counts as a host."""
        if not slot_name:
            raise ValueError("slot_name is required")
        return conn.call(
            "widget.replace_with_named_slot",
            {**_bp_widget(widget_path, widget_name), "slot_name": slot_name},
        )

    @mcp.tool()
    def widget_wrap(widget_path: str, widget_names: list[str], widget_class: str) -> dict:
        """Wraps widgets in a newly created panel of widget_class, which takes their place in
        the tree — the designer's "Wrap With". widget_class must be a panel widget. Returns
        the created wrapper widget(s)."""
        if not widget_names:
            raise ValueError("widget_names is required and must be non-empty")
        if not widget_class:
            raise ValueError("widget_class is required")
        return conn.call(
            "widget.wrap",
            {**_bp(widget_path), "widget_names": widget_names, "widget_class": widget_class},
        )

    @mcp.tool()
    def widget_set_is_variable(widget_path: str, widget_name: str, is_variable: bool) -> dict:
        """Exposes (or hides) a widget as a blueprint variable — the "Is Variable" checkbox.
        A widget must be a variable before the graph can reference it, which includes binding
        its events with widget_bind_event."""
        return conn.call(
            "widget.set_is_variable",
            {**_bp_widget(widget_path, widget_name), "is_variable": is_variable},
        )

    @mcp.tool()
    def widget_set_root(widget_path: str, widget_name: str = "") -> dict:
        """Makes an existing parentless widget the tree root, or clears the root when
        widget_name is empty. A widget that still has a parent must be moved out first.
        Normally you don't need this — widget_add sets the root when the tree is empty."""
        params = _bp(widget_path)
        if widget_name:
            params["widget_name"] = widget_name
        return conn.call("widget.set_root", params)

    @mcp.tool()
    def widget_duplicate(widget_path: str, widget_name: str, parent_widget: str = "") -> dict:
        """Copies a widget and its subtree back into the same blueprint with fresh unique
        names, preserving slot layout. parent_widget defaults to the source widget's own
        parent; it is required when duplicating the root."""
        params = _bp_widget(widget_path, widget_name)
        if parent_widget:
            params["parent_widget"] = parent_widget
        return conn.call("widget.duplicate", params)

    # =====================================================================================
    # WIDGET AND SLOT PROPERTIES
    # =====================================================================================

    @mcp.tool()
    def widget_list_properties(widget_path: str, widget_name: str, editable_only: bool = True) -> dict:
        """Lists a widget's properties with type, current value, category and read-only flag.
        The value strings round-trip: pass one straight back to widget_set_widget_property.
        editable_only false also reports properties hidden from the details panel."""
        return conn.call(
            "widget.list_properties",
            {**_bp_widget(widget_path, widget_name), "editable_only": editable_only},
        )

    @mcp.tool()
    def widget_set_widget_property(widget_path: str, widget_name: str, property: str, value: str) -> dict:
        """Writes a property on a widget — appearance, behaviour, anything in its details
        panel. Values use Unreal's property text form: "Visible" / "Collapsed" for
        Visibility, "(R=1,G=0,B=0,A=1)" for colours, "(Left=4,Top=4,Right=4,Bottom=4)" for
        margins, "true"/"false" for bools. Call widget_list_properties to see the exact name
        and current value form. Layout inside the parent panel lives on the slot, not the
        widget — use widget_set_slot_property for that."""
        if not property:
            raise ValueError("property is required")
        return conn.call(
            "widget.set_widget_property",
            {**_bp_widget(widget_path, widget_name), "property": property, "value": value},
        )

    @mcp.tool()
    def widget_list_slot_properties(widget_path: str, widget_name: str, editable_only: bool = True) -> dict:
        """Lists the layout properties of a widget's slot in its parent panel. The available
        properties depend on the parent: CanvasPanelSlot has anchors and offsets,
        HorizontalBoxSlot has size and padding, GridSlot has row and column. The root widget
        and widgets in named slots have no slot and return an error."""
        return conn.call(
            "widget.list_slot_properties",
            {**_bp_widget(widget_path, widget_name), "editable_only": editable_only},
        )

    @mcp.tool()
    def widget_set_slot_property(widget_path: str, widget_name: str, property: str, value: str) -> dict:
        """Writes a layout property on a widget's slot — position, size, padding, alignment.
        Examples: on a CanvasPanelSlot, "LayoutData" takes
        "(Offsets=(Left=0,Top=0,Right=200,Bottom=50),Anchors=(Minimum=(X=0,Y=0),Maximum=(X=0,Y=0)),Alignment=(X=0,Y=0))";
        on a HorizontalBoxSlot, "Padding" takes "(Left=8,Top=0,Right=8,Bottom=0)". Call
        widget_list_slot_properties for the exact names of the parent's slot type."""
        if not property:
            raise ValueError("property is required")
        return conn.call(
            "widget.set_slot_property",
            {**_bp_widget(widget_path, widget_name), "property": property, "value": value},
        )

    # =====================================================================================
    # NAMED SLOTS
    # =====================================================================================

    @mcp.tool()
    def widget_named_slot_list(widget_path: str, host_widget: str = "") -> dict:
        """Lists named slots and their current content.

        Only three things host named slots: a nested user widget placed in this tree (it
        offers the slots its own blueprint exposes), an ExpandableArea, and the blueprint
        itself — omit host_widget for that, which addresses the slots inherited from the
        parent class. A NamedSlot widget is NOT a host: it declares a slot for blueprints
        that consume this one, and its own content is an ordinary child (widget_add /
        widget_move). See "exposed_named_slots" in widget_get_info for what this blueprint
        offers to its consumers."""
        params = _bp(widget_path)
        if host_widget:
            params["host_widget"] = host_widget
        return conn.call("widget.named_slot_list", params)

    @mcp.tool()
    def widget_named_slot_set_content(
        widget_path: str, slot_name: str, content_widget: str, host_widget: str = ""
    ) -> dict:
        """Places an existing widget into a named slot. The widget is detached from its
        current panel parent first — a widget can only live in one place. host_widget names
        the slot host; omit it to fill a slot inherited from the parent class."""
        if not slot_name:
            raise ValueError("slot_name is required")
        if not content_widget:
            raise ValueError("content_widget is required")
        params = _bp(widget_path)
        params["slot_name"] = slot_name
        params["content_widget"] = content_widget
        if host_widget:
            params["host_widget"] = host_widget
        return conn.call("widget.named_slot_set_content", params)

    @mcp.tool()
    def widget_named_slot_clear(
        widget_path: str, slot_name: str, host_widget: str = "", delete_content: bool = False
    ) -> dict:
        """Empties a named slot. By default the content widget stays in the tree unparented —
        pass delete_content true to delete it outright. Returns the name of what was in the
        slot as "previous_content"."""
        if not slot_name:
            raise ValueError("slot_name is required")
        params = _bp(widget_path)
        params["slot_name"] = slot_name
        params["delete_content"] = delete_content
        if host_widget:
            params["host_widget"] = host_widget
        return conn.call("widget.named_slot_clear", params)

    # =====================================================================================
    # PROPERTY BINDINGS AND EVENTS
    # =====================================================================================

    @mcp.tool()
    def widget_binding_list(widget_path: str) -> dict:
        """Lists the blueprint's property bindings — widget properties driven each frame by a
        function or variable. "target_exists" false means the bound function or variable is
        gone and the binding will warn at compile time."""
        return conn.call("widget.binding_list", _bp(widget_path))

    @mcp.tool()
    def widget_binding_add(
        widget_path: str, widget_name: str, property: str, source: str, kind: str = "function"
    ) -> dict:
        """Binds a widget property to a blueprint member so it re-evaluates each frame — the
        chain icon next to a property in the details panel. kind "function" binds to a
        blueprint function whose return type matches the property (source is the function
        name); kind "property" binds directly to a member variable. Requires the blueprint to
        have been compiled at least once so the member exists. Binding an already-bound
        property replaces the existing binding. Property bindings cost per-frame evaluation —
        prefer setting values from an event where you can."""
        if not property:
            raise ValueError("property is required")
        if not source:
            raise ValueError("source is required")
        params = _bp_widget(widget_path, widget_name)
        params["property"] = property
        params["source"] = source
        params["kind"] = kind
        return conn.call("widget.binding_add", params)

    @mcp.tool()
    def widget_binding_remove(widget_path: str, widget_name: str, property: str) -> dict:
        """Removes a property binding. Returns {"removed"} — the count of bindings dropped,
        0 if there was none."""
        if not property:
            raise ValueError("property is required")
        return conn.call(
            "widget.binding_remove",
            {**_bp_widget(widget_path, widget_name), "property": property},
        )

    @mcp.tool()
    def widget_list_events(widget_path: str, widget_name: str) -> dict:
        """Lists the events a widget exposes to the graph — OnClicked on a Button,
        OnTextChanged on an EditableText, and so on. These names are what widget_bind_event
        takes."""
        return conn.call("widget.list_events", _bp_widget(widget_path, widget_name))

    @mcp.tool()
    def widget_bind_event(widget_path: str, widget_name: str, event_name: str) -> dict:
        """Creates the event graph node for one of a widget's events, the same as clicking
        the event button in the details panel. The widget must be exposed as a variable first
        (widget_set_is_variable). Call widget_list_events for valid event names. Binding an
        event that already has a node is a no-op."""
        if not event_name:
            raise ValueError("event_name is required")
        return conn.call(
            "widget.bind_event",
            {**_bp_widget(widget_path, widget_name), "event_name": event_name},
        )

    # =====================================================================================
    # UI COMPONENTS
    # =====================================================================================

    @mcp.tool()
    def widget_component_add(widget_path: str, widget_name: str, component_class: str) -> dict:
        """Attaches a UIComponent to a widget — reusable behaviour modules (UE 5.6+) that
        extend a widget without subclassing it. component_class is a UUIComponent subclass
        path."""
        if not component_class:
            raise ValueError("component_class is required")
        return conn.call(
            "widget.component_add",
            {**_bp_widget(widget_path, widget_name), "component_class": component_class},
        )

    @mcp.tool()
    def widget_component_remove(widget_path: str, widget_name: str, component_class: str) -> dict:
        """Detaches a UIComponent from a widget."""
        if not component_class:
            raise ValueError("component_class is required")
        return conn.call(
            "widget.component_remove",
            {**_bp_widget(widget_path, widget_name), "component_class": component_class},
        )

    @mcp.tool()
    def widget_component_move(
        widget_path: str,
        widget_name: str,
        component_class: str,
        relative_to_class: str,
        move_after: bool = True,
    ) -> dict:
        """Reorders a UIComponent relative to another on the same widget. Order matters when
        components affect the same widget state."""
        if not component_class:
            raise ValueError("component_class is required")
        if not relative_to_class:
            raise ValueError("relative_to_class is required")
        params = _bp_widget(widget_path, widget_name)
        params["component_class"] = component_class
        params["relative_to_class"] = relative_to_class
        params["move_after"] = move_after
        return conn.call("widget.component_move", params)

    # =====================================================================================
    # CLIPBOARD TEXT
    # =====================================================================================

    @mcp.tool()
    def widget_export_text(widget_path: str, widget_names: list[str]) -> dict:
        """Serialises widgets (and their subtrees) to the editor's clipboard text format.
        Pair with widget_import_text to copy widgets between blueprints, or keep the text as
        a snapshot before a risky edit."""
        if not widget_names:
            raise ValueError("widget_names is required and must be non-empty")
        return conn.call("widget.export_text", {**_bp(widget_path), "widget_names": widget_names})

    @mcp.tool()
    def widget_import_text(widget_path: str, text: str, parent_widget: str = "") -> dict:
        """Recreates widgets from clipboard text produced by widget_export_text, assigning
        fresh names on collision. parent_widget names the panel to attach the imported roots
        to; omit it to leave them unparented in the tree (useful before deciding placement)."""
        if not text:
            raise ValueError("text is required")
        params = _bp(widget_path)
        params["text"] = text
        if parent_widget:
            params["parent_widget"] = parent_widget
        return conn.call("widget.import_text", params)
