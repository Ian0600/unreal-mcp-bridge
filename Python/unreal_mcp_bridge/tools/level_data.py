from __future__ import annotations

from typing import Any

from fastmcp import FastMCP

from ..connection import UnrealConnection


def _params(**kwargs: Any) -> dict:
    """Drops unset arguments (None / empty string) so the plugin's own defaults apply."""
    return {k: v for k, v in kwargs.items() if v is not None and v != ""}


def register_level_data_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def level_query_actors(
        class_name: str = "",
        exact_class: bool = False,
        folder: str = "",
        label: str = "",
        tag: str = "",
        sphere: dict[str, Any] | None = None,
        box: dict[str, Any] | None = None,
        include_temp: bool = False,
        properties: list[str] | None = None,
        include_bounds: bool = False,
        max: int = 500,
    ) -> dict:
        """Read-only. Finds level actors by any mix of filters (AND).

        class_name: class or parent class name (MonsterSpawnPoint, BP_Foo / BP_Foo_C) or path.
        folder: outliner folder prefix. label: substring, or a wildcard with * / ?.
        sphere: {"center": [x,y,z], "radius": cm} (results sorted by distance). box: {"min", "max"}.
        properties: property paths to read back, e.g. ["Count", "ScatterRadius", "PatrolRoute.Duration"].
        Temp actors (spawn_temp) are skipped unless include_temp.

        Returns {actors: [{object_path, name, label, class, folder, location, rotation,
        external_package, file, distance?, bounds?, properties?}], total, truncated}."""
        return conn.call("level.query_actors", _params(
            **{"class": class_name},
            exact_class=exact_class or None,
            folder=folder,
            label=label,
            tag=tag,
            sphere=sphere,
            box=box,
            include_temp=include_temp or None,
            properties=properties,
            include_bounds=include_bounds or None,
            max=max,
        ))

    @mcp.tool()
    def actor_spawn(
        class_name: str = "",
        asset: str = "",
        location: list[float] | None = None,
        rotation: list[float] | None = None,
        scale: list[float] | None = None,
        label: str = "",
        folder: str = "",
        properties: dict[str, Any] | None = None,
        snap_to_ground: bool = False,
        z_offset: float | None = None,
        items: list[dict[str, Any]] | None = None,
        dry_run: bool = False,
    ) -> dict:
        """Places SAVED level actors (the level and, in a World Partition map, a new __ExternalActors__ file
        per actor become dirty; nothing is saved). One undo step for the whole call.

        class_name: native class (MonsterSpawnPoint, Container, ...) or a Blueprint path; or asset: a
        mesh etc. spawned the way a Content Browser drag would. properties: {path: ImportText / number /
        bool} set after spawning (e.g. {"Count": 4, "InitialAlertState": "Dormant"}).
        items: [{...}] places several; top-level arguments are their defaults.
        dry_run: validate class and every property against the class defaults, spawn nothing.

        Returns {actors: [{object_path, label, class, folder, location, external_package, file, bounds}],
        level_dirty, dirtied_packages, map_dirty} or {dry_run, plan}. Then level_dirty_files for the
        Perforce add/checkout list."""
        return conn.call("level.actor_spawn", _params(
            **{"class": class_name},
            asset=asset,
            location=location,
            rotation=rotation,
            scale=scale,
            label=label,
            folder=folder,
            properties=properties,
            snap_to_ground=snap_to_ground or None,
            z_offset=z_offset,
            items=items,
            dry_run=dry_run or None,
        ))

    @mcp.tool()
    def actors_set_properties(
        changes: list[dict[str, Any]] | None = None,
        actors: list[str] | None = None,
        properties: dict[str, Any] | None = None,
        dry_run: bool = False,
    ) -> dict:
        """Writes many properties on many level actors as ONE undo step. Run with dry_run=true first and
        show the diff for approval.

        changes: [{"actor": label/path, "properties": {path: value}}], or actors: [...] + properties:
        {...} for the same writes on each. Values are ImportText strings (the form level_query_actors /
        object_get_property return), numbers or bools. Every write is parsed before any is made.

        Returns {dry_run, changed_count, diff: [{actor, actor_path, property, old, new, changed}]}
        (+ level_dirty / dirtied_packages when applied)."""
        return conn.call("level.actors_set_properties", _params(
            changes=changes,
            actors=actors,
            properties=properties,
            dry_run=dry_run,
        ))

    @mcp.tool()
    def actor_add_instanced(
        actor: str,
        property: str,
        class_name: str,
        properties: dict[str, Any] | None = None,
        index: int | None = None,
        dry_run: bool = False,
    ) -> dict:
        """Adds an instanced subobject to an actor's array property — e.g. a quest-give action to
        AIFSRTriggerVolume.TriggerActions — and sets its properties. One undo step; not saved.

        class_name: concrete subclass of the array's element class (native name or Blueprint path).
        index: insert position (default end). dry_run: validate class and properties only.

        Returns {actor, property, index, object_path, class, count, properties, level_dirty, ...}."""
        return conn.call("level.actor_add_instanced", _params(
            actor=actor,
            property=property,
            **{"class": class_name},
            properties=properties,
            index=index,
            dry_run=dry_run or None,
        ))

    @mcp.tool()
    def level_dirty_files(include_assets: bool = False) -> dict:
        """Read-only. Packages that saving the level would write: the map, and in a World Partition map one
        __ExternalActors__ / __ExternalObjects__ .uasset per changed or new actor/folder.
        include_assets also lists other dirty assets (data tables, ...).

        Returns {files: [{package, file, kind, exists_on_disk, read_only, p4 (add / edit / delete),
        actor, actor_class}], counts}. p4 is a hint from the file state; the checkout and add go through
        the Perforce tooling, not this bridge."""
        return conn.call("level.dirty_files", _params(include_assets=include_assets or None))

    @mcp.tool()
    def spline_set_points(
        actor: str,
        points: list[list[float]],
        component: str = "",
        space: str = "world",
        closed_loop: bool | None = None,
        point_type: str = "",
        dry_run: bool = False,
    ) -> dict:
        """Rewrites a spline component's points (patrol routes: AMonsterSpawnPoint.PatrolRoute). One undo
        step; marks the spline as edited so a construction script does not regenerate it.

        points: [[x,y,z], ...] (>= 2) in space "world" or "local". point_type: curve / linear /
        constant / curve_clamped. component: spline component name (default the first).

        Returns {actor, component, old_points, new_points (world), closed_loop, level_dirty, ...}."""
        return conn.call("level.spline_set_points", _params(
            actor=actor,
            points=points,
            component=component,
            space=space,
            closed_loop=closed_loop,
            point_type=point_type,
            dry_run=dry_run or None,
        ))

    @mcp.tool()
    def volume_set_extent(
        actor: str,
        extent: list[float] | None = None,
        size_m: list[float] | None = None,
        component: str = "",
        dry_run: bool = False,
    ) -> dict:
        """Resizes a trigger / blocking volume. extent: [x,y,z] half size in cm, or size_m: full size in
        metres. Box component -> box extent; sphere -> radius = x; capsule -> radius = x, half height = z;
        brush volume -> actor scale so its bounds match. One undo step; not saved.

        Returns {actor, method, component, old_extent, new_extent, level_dirty, ...}."""
        return conn.call("level.volume_set_extent", _params(
            actor=actor,
            extent=extent,
            size_m=size_m,
            component=component,
            dry_run=dry_run or None,
        ))

    @mcp.tool()
    def nav_project_point(
        points: list[list[float]],
        extent: list[float] | None = None,
        world: str = "",
    ) -> dict:
        """Read-only. Projects points onto the navmesh — are spawn / patrol points on it?

        extent: query box half size (default [50, 50, 250]). world: "editor" (default) or "pie".

        Returns {points: [{input, ok, location, distance}], nav_data}."""
        return conn.call("nav.project_point", _params(points=points, extent=extent, world=world))

    @mcp.tool()
    def nav_test_path(
        start: list[float] | None = None,
        end: list[float] | None = None,
        pairs: list[dict[str, Any]] | None = None,
        extent: list[float] | None = None,
        world: str = "",
    ) -> dict:
        """Read-only. Does a navmesh path connect start to end (e.g. can an ambush group reach the player)?
        Give start + end, or pairs: [{"start", "end"}]. Both ends are projected onto the navmesh first.

        Returns {paths: [{start, end, projected, ok, partial, length, points}], nav_data}."""
        return conn.call("nav.test_path", _params(start=start, end=end, pairs=pairs, extent=extent, world=world))
