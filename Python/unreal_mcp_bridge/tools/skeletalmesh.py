from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_skeletalmesh_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def skeletalmesh_list_sockets(mesh_path: str) -> dict:
        """Lists every attachment socket a skeletal mesh answers with, its own and its skeleton's.

        The two lists come back separately because they do not behave the same way: a skeleton
        socket is shared by every mesh built on that skeleton, a mesh socket is not. At runtime the
        mesh is asked first, so a mesh socket silently shadows a skeleton socket of the same name.

        Returns {"mesh_sockets": [...], "skeleton_sockets": [...]}, each entry
        {"name", "bone", "location" [x,y,z], "rotation" [pitch,yaw,roll], "scale" [x,y,z]}."""
        if not mesh_path:
            raise ValueError("mesh_path is required")
        return conn.call("skeletalmesh.list_sockets", {"mesh_path": mesh_path})

    @mcp.tool()
    def skeletalmesh_add_socket(
        mesh_path: str,
        socket_name: str,
        bone_name: str,
        location: list[float] | None = None,
        rotation: list[float] | None = None,
        scale: list[float] | None = None,
        to_skeleton: bool = False,
        replace: bool = False,
        preview_mesh: str | None = None,
    ) -> dict:
        """Creates (or overwrites) an attachment socket on a skeletal mesh or its skeleton.

        Re-exporting a character mesh wipes its sockets, so this is the command that puts a rig back
        together after an art drop rather than a one-off convenience.

        rotation is [pitch, yaw, roll] — the order the details panel shows, not FRotator's
        constructor order. location and scale are [x, y, z]; omitting one leaves the default
        (zero / one), so a socket that only needs a bone can pass just bone_name.

        to_skeleton puts the socket on the USkeleton instead of this mesh, which is what a character
        whose body mesh gets swapped needs — every mesh on that skeleton then sees it. Default is the
        mesh, the narrower choice.

        The bone is checked for existence. Attaching to a bone that is not there is not a runtime
        error: the attached component just sits at the mesh origin and nothing says why.

        replace=true overwrites an existing socket in place instead of failing, keeping anything
        already attached to it pointing at the same object.

        preview_mesh sets the editor-only preview asset shown at the socket (a weapon on the hand,
        say). It lives on the skeleton's PreviewAttachedAssetContainer, not on the socket, and is
        unreachable from Python. Pass "" to clear it; omit it to leave it alone.

        Returns {"ok", "replaced", "owner", "socket": {...}}."""
        if not mesh_path:
            raise ValueError("mesh_path is required")
        if not socket_name:
            raise ValueError("socket_name is required")
        if not bone_name:
            raise ValueError("bone_name is required")
        params: dict = {
            "mesh_path": mesh_path,
            "socket_name": socket_name,
            "bone_name": bone_name,
            "to_skeleton": to_skeleton,
            "replace": replace,
        }
        if location is not None:
            params["location"] = location
        if rotation is not None:
            params["rotation"] = rotation
        if scale is not None:
            params["scale"] = scale
        if preview_mesh is not None:
            params["preview_mesh"] = preview_mesh
        return conn.call("skeletalmesh.add_socket", params)

    @mcp.tool()
    def skeletalmesh_remove_socket(mesh_path: str, socket_name: str) -> dict:
        """Removes a socket, from whichever of the two lists holds it — mesh first, then skeleton.

        Returns {"ok", "removed_from": "mesh" | "skeleton"}. Removing a skeleton socket affects every
        mesh on that skeleton, so check removed_from if you meant to touch only this one."""
        if not mesh_path:
            raise ValueError("mesh_path is required")
        if not socket_name:
            raise ValueError("socket_name is required")
        return conn.call("skeletalmesh.remove_socket", {
            "mesh_path": mesh_path,
            "socket_name": socket_name,
        })

    @mcp.tool()
    def skeletalmesh_dump_skeleton(asset_path: str) -> dict:
        """스켈레톤 구조 덤프: 슬롯그룹/가상본/소켓/커브 이름/호환 스켈레톤/프리뷰 메시.

        Forwarded to BlueprintInspector (BlueprintInspectorLibrary::DumpSkeletonStructure); requires that plugin to be loaded."""
        return conn.call("skeletalmesh.dump_skeleton", {"asset_path": asset_path})
