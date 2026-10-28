from __future__ import annotations

import time

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_asset_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def asset_list(package_path: str, recursive: bool = False) -> dict:
        if not package_path:
            raise ValueError("package_path is required")
        return conn.call("asset.list", {
            "package_path": package_path,
            "recursive": recursive,
        })

    @mcp.tool()
    def asset_get_metadata(object_path: str) -> dict:
        if not object_path:
            raise ValueError("object_path is required")
        return conn.call("asset.get_metadata", {"object_path": object_path})

    @mcp.tool()
    def asset_save(
        object_path: str = "",
        paths: list[str] | None = None,
        deferred: bool = True,
        timeout: float = 60.0,
    ) -> dict:
        """Saves one or more asset packages and reports each file's state.

        deferred (default True): the save runs on the next editor tick, outside this request, and this
        tool waits for it (up to timeout seconds) - the safe way after editing or compiling the asset.
        Read-only files are not attempted: their p4 field says "edit" (check out first); "add" = new file.

        Returns {saved (all ok), results: [{path, saved, read_only, p4, file, error}]}."""
        all_paths = ([object_path] if object_path else []) + list(paths or [])
        if not all_paths:
            raise ValueError("object_path or paths is required")
        queued = conn.call("asset.save", {"paths": all_paths, "deferred": deferred})
        if not deferred:
            return queued
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            result = conn.call("asset.save_result", {"job": queued["job"]})
            if result.get("done"):
                return result
            time.sleep(0.2)
        raise TimeoutError(f"save job {queued['job']} did not finish in {timeout}s (asset.save_result to check later)")

    @mcp.tool()
    def asset_fixup_redirectors(
        package_path: str = "",
        object_paths: list[str] | None = None,
        dry_run: bool = False,
    ) -> dict:
        """Fix Up Redirectors without any dialog (the engine's FixupReferencers opens modals, which
        stall the bridge). package_path is a folder (recursive); object_paths lists redirectors or any
        assets (non-redirectors are skipped). Writable referencers are re-saved; read-only (not checked
        out) ones and maps are left alone and reported. Nothing is checked out or deleted.

        Returns {"redirectors", "fixed", "referencers_saved", "remaining": [{"redirector",
        "destination", "referencers": [{"package", "reason"}]}], "delete_with_p4": [files]} —
        `p4 delete` the files in delete_with_p4 yourself."""
        if not package_path and not object_paths:
            raise ValueError("package_path or object_paths is required")
        params: dict = {"dry_run": dry_run}
        if package_path:
            params["package_path"] = package_path
        if object_paths:
            params["object_paths"] = object_paths
        return conn.call("asset.fixup_redirectors", params)
