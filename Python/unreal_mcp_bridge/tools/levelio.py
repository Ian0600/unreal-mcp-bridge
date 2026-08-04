from __future__ import annotations

from typing import Sequence

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_levelio_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def level_export_selected(
        path: str,
        selected_only: bool = True,
        include_collision: bool = False,
        ascii: bool = False,
    ) -> dict:
        """Export selected level actors (or the whole level) to one FBX file.

        Exports every mesh component of every selected actor into a single file — picking a
        Blueprint character pulls in all of its skeletal meshes at once. Use asset_export_fbx
        to get individual meshes out separately.
        """
        if not path:
            raise ValueError("path is required")
        return conn.call("level.export_selected", {
            "path": path,
            "selected_only": selected_only,
            "include_collision": include_collision,
            "ascii": ascii,
        })

    @mcp.tool()
    def asset_export_fbx(
        path: str,
        object_path: str | None = None,
        object_paths: Sequence[str] | None = None,
        ascii: bool = False,
        include_collision: bool = False,
        include_lods: bool = False,
        include_morph_targets: bool = True,
    ) -> dict:
        """Export individual StaticMesh/SkeletalMesh assets to FBX, one file each.

        Give object_path for a single asset, object_paths for several, or neither to use the
        current Content Browser selection. When path ends in .fbx it names the output file and
        only one asset may resolve; otherwise it is a directory receiving <AssetName>.fbx per
        asset.
        """
        if not path:
            raise ValueError("path is required")
        params: dict = {
            "path": path,
            "ascii": ascii,
            "include_collision": include_collision,
            "include_lods": include_lods,
            "include_morph_targets": include_morph_targets,
        }
        if object_path:
            params["object_path"] = object_path
        if object_paths:
            params["object_paths"] = list(object_paths)
        return conn.call("asset.export_fbx", params)

    @mcp.tool()
    def asset_import_fbx(
        path: str,
        destination_path: str,
        replace_existing: bool = True,
        import_as_skeletal: bool = False,
        skeleton_path: str | None = None,
        combine_meshes: bool = False,
        generate_lightmap_uvs: bool = True,
        auto_generate_collision: bool = False,
        save: bool = True,
        use_interchange: bool = False,
    ) -> dict:
        """Import an FBX from disk into the content browser.

        Set auto_generate_collision False when the FBX already carries UBX_/UCX_ collision from
        Maya, or Unreal adds its own on top. Materials and textures are never imported — an
        Unreal material and a Maya shader are different objects.
        """
        if not path:
            raise ValueError("path is required")
        if not destination_path:
            raise ValueError("destination_path is required")
        params: dict = {
            "path": path,
            "destination_path": destination_path,
            "replace_existing": replace_existing,
            "import_as_skeletal": import_as_skeletal,
            "combine_meshes": combine_meshes,
            "generate_lightmap_uvs": generate_lightmap_uvs,
            "auto_generate_collision": auto_generate_collision,
            "save": save,
            "use_interchange": use_interchange,
        }
        if skeleton_path:
            params["skeleton_path"] = skeleton_path
        return conn.call("asset.import_fbx", params)

    @mcp.tool()
    def level_spawn_asset(
        object_path: str,
        location: Sequence[float] | None = None,
        rotation: Sequence[float] | None = None,
        scale: Sequence[float] | None = None,
        label: str | None = None,
    ) -> dict:
        """Place an imported asset into the current editor level.

        rotation is [pitch, yaw, roll], following Unreal's FRotator ordering.
        """
        if not object_path:
            raise ValueError("object_path is required")
        params: dict = {"object_path": object_path}
        if location is not None:
            params["location"] = list(location)
        if rotation is not None:
            params["rotation"] = list(rotation)
        if scale is not None:
            params["scale"] = list(scale)
        if label:
            params["label"] = label
        return conn.call("level.spawn_asset", params)
