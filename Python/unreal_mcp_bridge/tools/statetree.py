from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_statetree_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def statetree_list_states(tree_path: str) -> dict:
        """Dumps a StateTree's state hierarchy — read this before editing. Sibling order decides
        which state wins (the first one whose conditions pass), so where to insert cannot be
        chosen without seeing the existing order, and the asset's EditorData is protected, so
        editor Python cannot read it either.

        Each entry is {id, name, tasks[], enter_conditions[], transitions[], children[]}. Task
        and condition entries are full struct paths, the same strings statetree_add_task /
        statetree_add_condition take. A transition is {id, trigger (raw enum bits), target,
        condition_count}. Returns {"states": [...]} with one entry per root subtree."""
        if not tree_path:
            raise ValueError("tree_path is required")
        return conn.call("statetree.list_states", {"tree_path": tree_path})

    @mcp.tool()
    def statetree_move_state(
        tree_path: str,
        state_id: str,
        index: int = -1,
        parent_state_id: str | None = None,
    ) -> dict:
        """Reorders a state among its siblings, optionally reparenting it. statetree_add_state
        always appends, and an unconditional sibling ahead of the new state means it is never
        selected — so a freshly added state usually has to be moved to be worth anything.

        index is 0-based; -1 or past the end means last. parent_state_id has three cases:
        omitted keeps the current parent (pure reorder), "" moves the state to the root subtree
        list, and a GUID reparents under that state. Moving a state under itself or its own
        descendant is refused. Returns {"moved","parent","index"}."""
        if not tree_path:
            raise ValueError("tree_path is required")
        if not state_id:
            raise ValueError("state_id is required")
        params: dict = {"tree_path": tree_path, "state_id": state_id, "index": index}
        if parent_state_id is not None:
            params["parent_state_id"] = parent_state_id
        return conn.call("statetree.move_state", params)

    @mcp.tool()
    def statetree_add_state(
        tree_path: str,
        state_name: str,
        parent_state_id: str = "",
    ) -> dict:
        """Adds a state to a StateTree asset. If parent_state_id is empty the state is
        added under the tree's Root subtree (created if missing); otherwise it becomes a
        child of the given state. Returns {"state_id": <guid>} to pass to
        statetree_add_task / statetree_add_transition."""
        if not tree_path:
            raise ValueError("tree_path is required")
        if not state_name:
            raise ValueError("state_name is required")
        params: dict = {"tree_path": tree_path, "state_name": state_name}
        if parent_state_id:
            params["parent_state_id"] = parent_state_id
        return conn.call("statetree.add_state", params)

    @mcp.tool()
    def statetree_add_evaluator(tree_path: str, struct_path: str) -> dict:
        """Adds a tree-level Evaluator node of the given struct type (e.g.
        "/Script/IFSR.STEval_SenseTarget"). Evaluators run every tick and expose outputs
        (target/distance/flags) that transitions and tasks can bind to. Returns
        {"node_id": <guid>}."""
        if not tree_path:
            raise ValueError("tree_path is required")
        if not struct_path:
            raise ValueError("struct_path is required")
        return conn.call("statetree.add_evaluator", {
            "tree_path": tree_path,
            "struct_path": struct_path,
        })

    @mcp.tool()
    def statetree_add_task(tree_path: str, state_id: str, struct_path: str) -> dict:
        """Adds a Task node of the given struct type to a state. The task runs while the
        state is active (e.g. a MoveTo or PlayAnimation task). Returns {"node_id": <guid>}
        to bind task inputs from evaluator outputs via statetree_add_binding."""
        if not tree_path:
            raise ValueError("tree_path is required")
        if not state_id:
            raise ValueError("state_id is required")
        if not struct_path:
            raise ValueError("struct_path is required")
        return conn.call("statetree.add_task", {
            "tree_path": tree_path,
            "state_id": state_id,
            "struct_path": struct_path,
        })

    @mcp.tool()
    def statetree_add_transition(
        tree_path: str,
        from_state_id: str,
        to_state_id: str,
        trigger: str = "OnTick",
    ) -> dict:
        """Adds a GotoState transition from one state to another. trigger is one of
        "OnTick" (default, re-evaluated every tick), "OnStateCompleted",
        "OnStateSucceeded", or "OnEvent". The transition is unconditional; add a bound
        condition in the editor (or a future statetree_add_condition) to gate it.
        Returns {"transition_id": <guid>}."""
        if not tree_path:
            raise ValueError("tree_path is required")
        if not from_state_id:
            raise ValueError("from_state_id is required")
        if not to_state_id:
            raise ValueError("to_state_id is required")
        return conn.call("statetree.add_transition", {
            "tree_path": tree_path,
            "from_state_id": from_state_id,
            "to_state_id": to_state_id,
            "trigger": trigger,
        })

    @mcp.tool()
    def statetree_add_binding(
        tree_path: str,
        source_node_id: str,
        source_path: str,
        target_node_id: str,
        target_path: str,
    ) -> dict:
        """Binds an output property of a source node (evaluator/task, by node_id) to an
        input property of a target node. Paths are property names relative to the node's
        instance data, e.g. source_path="TargetActor", target_path="Target". Returns
        {"ok": true}."""
        for name, value in (
            ("tree_path", tree_path),
            ("source_node_id", source_node_id),
            ("source_path", source_path),
            ("target_node_id", target_node_id),
            ("target_path", target_path),
        ):
            if not value:
                raise ValueError(f"{name} is required")
        return conn.call("statetree.add_binding", {
            "tree_path": tree_path,
            "source_node_id": source_node_id,
            "source_path": source_path,
            "target_node_id": target_node_id,
            "target_path": target_path,
        })

    @mcp.tool()
    def statetree_remove_state(tree_path: str, state_id: str) -> dict:
        """Removes a state (and everything under it) from a StateTree by its GUID. A
        root-level state is removed from the tree's SubTrees; otherwise it is removed from
        its parent's children. Use to delete duplicate/stray states left by earlier edits.
        Returns {"removed": true}."""
        if not tree_path:
            raise ValueError("tree_path is required")
        if not state_id:
            raise ValueError("state_id is required")
        return conn.call("statetree.remove_state", {
            "tree_path": tree_path,
            "state_id": state_id,
        })

    @mcp.tool()
    def statetree_add_condition(
        tree_path: str,
        transition_id: str = "",
        state_id: str = "",
        struct_path: str = "",
    ) -> dict:
        """Adds a condition node, gating either a transition (pass transition_id) or a state's
        selection (pass state_id — an enter condition, evaluated when the tree considers the state
        rather than every tick). Pass exactly one of the two.

        struct_path defaults to "/Script/StateTreeModule.StateTreeCompareBoolCondition" (compares a
        bound bool input `bLeft` against a constant `bRight`). Other useful ones:
        "/Script/StateTreeModule.StateTreeRandomCondition" (parameter "Threshold", 0..1 — passes with
        that probability, for chance-based state selection).

        Typical wiring: statetree_add_binding to bind an evaluator bool output (e.g. "bHasTarget") to
        the condition's "bLeft", then statetree_set_node_property to set the constant "bRight".
        Returns {"node_id": <guid>} for the new condition node."""
        if not tree_path:
            raise ValueError("tree_path is required")
        if bool(transition_id) == bool(state_id):
            raise ValueError("provide exactly one of transition_id or state_id")
        params: dict = {"tree_path": tree_path}
        if transition_id:
            params["transition_id"] = transition_id
        if state_id:
            params["state_id"] = state_id
        if struct_path:
            params["struct_path"] = struct_path
        return conn.call("statetree.add_condition", params)

    @mcp.tool()
    def statetree_set_node_property(
        tree_path: str,
        node_id: str,
        property: str,
        value: str,
    ) -> dict:
        """Sets a property on a node's instance data by reflection. Works on any
        evaluator/task/condition node found by node_id. value is Unreal import text, e.g.
        "true" for a bool, or "(TagName=\\"Ability.Monster.Melee\\")" for a GameplayTag.
        Use to set a task's AbilityTag, a CompareBool condition's "bRight" constant, etc.
        Returns {"ok": true}."""
        for name, val in (
            ("tree_path", tree_path),
            ("node_id", node_id),
            ("property", property),
        ):
            if not val:
                raise ValueError(f"{name} is required")
        return conn.call("statetree.set_node_property", {
            "tree_path": tree_path,
            "node_id": node_id,
            "property": property,
            "value": value,
        })

    @mcp.tool()
    def statetree_compile(tree_path: str) -> dict:
        """Compiles the StateTree from its editor data (states/tasks/transitions/bindings)
        into runtime data and updates the compiled hash. Call after finishing edits.
        Returns {"compiled": true|false}."""
        if not tree_path:
            raise ValueError("tree_path is required")
        return conn.call("statetree.compile", {"tree_path": tree_path})
