from __future__ import annotations

from typing import Any

from fastmcp import FastMCP

from ..connection import UnrealConnection


def _params(**kwargs: Any) -> dict:
    """Drops unset arguments (None / empty string) so the plugin's own defaults apply."""
    return {k: v for k, v in kwargs.items() if v is not None and v != ""}


def register_datatable_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def datatable_get_rows(
        table: str,
        rows: list[str] | None = None,
        columns: list[str] | None = None,
        max: int = 500,
    ) -> dict:
        """Read-only. Rows of a data table, values as the table's own export text.

        table: e.g. "/Game/04_Data/DT/DT_Quest" (the object path suffix is optional).
        rows: row names (default all). columns: column names (default all).

        Returns {table, row_struct, source_file, row_count, columns: [{name, type}], rows: [{name,
        values: {column: text}}], missing_rows, unknown_columns, truncated}.
        There is no row write on purpose: tables come from Excel -> DataGen -> JSON."""
        return conn.call("datatable.get_rows", _params(table=table, rows=rows, columns=columns, max=max))

    @mcp.tool()
    def datatable_reimport(table: str, json_path: str = "", dry_run: bool = True) -> dict:
        """Re-reads a data table from its JSON source (Content/.TableSource/*.json, from the table's import
        data) or json_path, and reports the row diff.

        dry_run (default true) only reports; dry_run=false applies it — the asset becomes dirty but is
        not saved (asset_save after approval; the .uasset is in Perforce). A JSON that yields no rows is
        refused, so a broken file cannot empty the table. No editor dialogs (the scripting route's
        "missing entry" popup would block the bridge).

        Returns {table, source_file, dry_run, applied, added, removed, changed: [{row, columns: [{column,
        old, new}]}], unchanged_count, problems}."""
        return conn.call("datatable.reimport", _params(table=table, json_path=json_path, dry_run=dry_run))
