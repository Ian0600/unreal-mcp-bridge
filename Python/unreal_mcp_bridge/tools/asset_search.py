from __future__ import annotations

from typing import Any

from fastmcp import FastMCP

from ..connection import UnrealConnection


def _params(**kwargs: Any) -> dict:
    """Drops unset arguments (None / empty string) so the plugin's own defaults apply."""
    return {k: v for k, v in kwargs.items() if v is not None and v != ""}


def register_asset_search_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def asset_find(
        query: str = "",
        asset_class: str = "",
        path: str = "/Game",
        max: int = 100,
        include_size: bool = False,
    ) -> dict:
        """Read-only. Finds assets whose name contains every word of query (case-insensitive).

        asset_class: StaticMesh / SkeletalMesh / AnimSequence / AnimMontage / Blueprint / any class
        name or /Script path (subclasses included). path: content folder to search (recursive).
        include_size: add disk_size (bytes). query or asset_class is required.

        Returns {assets: [{name, class, object_path, package_path, skeleton?, disk_size?}], total,
        truncated}."""
        return conn.call("asset.find", _params(
            query=query,
            **{"class": asset_class},
            path=path,
            max=max,
            include_size=include_size or None,
        ))

    @mcp.tool()
    def asset_bounds(assets: list[str]) -> dict:
        """Read-only. Size of static / skeletal meshes (skeletal = reference pose), loading them if needed.

        Returns {bounds: [{asset, class, center, extent, size_m, min, max, sphere_radius,
        pivot_to_bottom} | {asset, error}]}. pivot_to_bottom: how far the bottom sits below the
        pivot — add it to a ground height to rest the mesh on the ground."""
        return conn.call("asset.bounds", {"assets": assets})

    @mcp.tool()
    def anims_for_skeleton(
        skeleton: str = "",
        mesh: str = "",
        classes: list[str] | None = None,
        query: str = "",
        path: str = "/Game",
        max: int = 500,
    ) -> dict:
        """Read-only. Animation assets a skeleton can play, compatible skeletons included (asset registry,
        nothing is loaded except the skeleton).

        skeleton: Skeleton path, or mesh: SkeletalMesh path (its skeleton is used).
        classes: default ["AnimSequence"]; add AnimMontage, BlendSpace, ... query: name words.

        Returns {skeleton, anims: [{name, class, object_path, skeleton, frames, frame_rate, length}],
        total, truncated}."""
        return conn.call("asset.anims_for_skeleton", _params(
            skeleton=skeleton,
            mesh=mesh,
            classes=classes,
            query=query,
            path=path,
            max=max,
        ))
