from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def _target(mesh_path: str, skeleton_path: str) -> dict:
    # The plugin takes either; a SkeletalMesh answers for its own sockets and its skeleton's.
    if mesh_path:
        return {"mesh_path": mesh_path}
    if skeleton_path:
        return {"skeleton_path": skeleton_path}
    raise ValueError("mesh_path or skeleton_path is required")


def register_skeletalmesh_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def skeletalmesh_list_sockets(mesh_path: str = "", skeleton_path: str = "") -> dict:
        """Lists attachment sockets. Give a SkeletalMesh (mesh_path) or a Skeleton (skeleton_path).

        Returns {"mesh_sockets": [...], "skeleton_sockets": [...]} — kept apart because a skeleton socket
        is shared by every mesh on that skeleton and a mesh socket of the same name shadows it."""
        return conn.call("skeletalmesh.list_sockets", _target(mesh_path, skeleton_path))

    @mcp.tool()
    def skeletalmesh_add_socket(
        socket_name: str,
        bone_name: str,
        mesh_path: str = "",
        skeleton_path: str = "",
        location: list[float] | None = None,
        rotation: list[float] | None = None,
        scale: list[float] | None = None,
        to_skeleton: bool = False,
        replace: bool = False,
        preview_mesh: str = "",
    ) -> dict:
        """Creates a socket (Python cannot: socket_name/bone_name are read-only there).

        location [x,y,z], rotation [pitch,yaw,roll], scale [x,y,z]. to_skeleton puts it on the mesh's
        Skeleton so every mesh on that skeleton gets it (a Skeleton target always does). replace
        overwrites a socket of the same name. preview_mesh: optional asset previewed on the socket.
        Does not save — call asset_save."""
        if not socket_name:
            raise ValueError("socket_name is required")
        if not bone_name:
            raise ValueError("bone_name is required")
        params = _target(mesh_path, skeleton_path)
        params.update({"socket_name": socket_name, "bone_name": bone_name})
        if location is not None:
            params["location"] = location
        if rotation is not None:
            params["rotation"] = rotation
        if scale is not None:
            params["scale"] = scale
        if to_skeleton:
            params["to_skeleton"] = True
        if replace:
            params["replace"] = True
        if preview_mesh:
            params["preview_mesh"] = preview_mesh
        return conn.call("skeletalmesh.add_socket", params)

    @mcp.tool()
    def skeletalmesh_update_socket(
        socket_name: str,
        mesh_path: str = "",
        skeleton_path: str = "",
        new_name: str = "",
        bone_name: str = "",
        location: list[float] | None = None,
        rotation: list[float] | None = None,
        scale: list[float] | None = None,
        preview_mesh: str | None = None,
    ) -> dict:
        """Changes only the fields given; new_name renames the socket (its preview asset follows).
        preview_mesh: asset path, or "" to clear; omit to leave it alone.

        Returns {"ok", "owner", "changed": [...], "socket"}. Does not save — call asset_save."""
        if not socket_name:
            raise ValueError("socket_name is required")
        params = _target(mesh_path, skeleton_path)
        params["socket_name"] = socket_name
        if new_name:
            params["new_name"] = new_name
        if bone_name:
            params["bone_name"] = bone_name
        if location is not None:
            params["location"] = location
        if rotation is not None:
            params["rotation"] = rotation
        if scale is not None:
            params["scale"] = scale
        if preview_mesh is not None:
            params["preview_mesh"] = preview_mesh
        return conn.call("skeletalmesh.update_socket", params)

    @mcp.tool()
    def skeletalmesh_remove_socket(
        socket_name: str,
        mesh_path: str = "",
        skeleton_path: str = "",
    ) -> dict:
        """Removes a socket from whichever list holds it (mesh first). Returns {"ok", "removed_from"}.
        Does not save — call asset_save."""
        if not socket_name:
            raise ValueError("socket_name is required")
        params = _target(mesh_path, skeleton_path)
        params["socket_name"] = socket_name
        return conn.call("skeletalmesh.remove_socket", params)
