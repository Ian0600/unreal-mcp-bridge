from __future__ import annotations

from typing import Sequence

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
    def asset_save(object_path: str) -> dict:
        if not object_path:
            raise ValueError("object_path is required")
        return conn.call("asset.save", {"object_path": object_path})

    @mcp.tool()
    def asset_get_selection() -> dict:
        """Assets currently selected in the Content Browser.

        Call this first when the user says "the selected mesh" — a Content Browser
        selection is a different thing from a level selection and never appears in
        level_get_selection. If this returns count 0, check level_get_selection before
        telling the user nothing is selected.
        """
        return conn.call("asset.get_selection", {})

    @mcp.tool()
    def asset_set_selection(object_paths: Sequence[str], focus: bool = True) -> dict:
        """Select assets in the Content Browser by object path.

        Paths that do not resolve come back in "not_found" instead of failing the call,
        so check that field rather than assuming every path was selected.

        Args:
            object_paths: Full object paths, e.g. "/Game/Props/SM_Prop.SM_Prop".
            focus: Bring the Content Browser to the front.
        """
        paths = [p for p in (object_paths or []) if p]
        if not paths:
            raise ValueError("object_paths is required and must be non-empty")
        return conn.call("asset.set_selection", {"object_paths": paths, "focus": focus})

    @mcp.tool()
    def asset_fixup_redirectors(content_paths: list[str]) -> dict:
        """Fix up redirectors under the given content paths (re-save referencers, delete redirectors).
        Referencing packages are saved; check them out of source control first if required.
        content_paths: Folders like "/Game/Characters" (recursive).
        Returns: JSON: {"found":int,"fixed":int,"remaining":[paths]}

        Built-in port of BlueprintInspector's BPIEditorToolset::FixupRedirectors; no other plugin needed."""
        return conn.call("asset.fixup_redirectors", {"content_paths": content_paths})
