from __future__ import annotations

from typing import Any

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_object_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def object_list_properties(object_path: str, include_values: bool = True) -> dict:
        """Read-only. Every reflected property on an object (asset, subobject or level/PIE actor),
        with type, flags and current value.

        object_path: full path - /Game/Path/Asset.Asset, /Game/Path/Asset.Asset:SubObject or
        /Game/Map.Map:PersistentLevel.Actor_0.

        Returns {object_path, object_class, properties: [{name, type, class, value, flags}]}."""
        return conn.call("object.list_properties", {"object_path": object_path, "include_values": include_values})

    @mcp.tool()
    def object_get_property(object_path: str, property_name: str) -> dict:
        """Read-only. One property value by path, in the text form object_set_property takes back.

        property_name walks structs, objects, arrays and maps: "Count", "RelativeLocation.X",
        "Children[2].bIsEnabled", "PatternRules[Spore].Cooldown" (map key parsed as the key type -
        enum short or full name, FName, number)."""
        return conn.call("object.get_property", {"object_path": object_path, "property_name": property_name})

    @mcp.tool()
    def object_set_property(object_path: str, property_name: str, value: str) -> dict:
        """Sets one property by path (same path rules as object_get_property) the way the details
        panel would, and marks the package dirty. Nothing is saved - call asset_save.

        Returns {ok, object_path, old_value, new_value}."""
        return conn.call("object.set_property", {
            "object_path": object_path,
            "property_name": property_name,
            "value": value,
        })

    @mcp.tool()
    def object_set_properties(object_path: str, values: dict[str, Any], dry_run: bool = False) -> dict:
        """Sets several property paths on one object. Every value is parsed first, so one bad entry
        writes nothing. dry_run shows the diff without writing.

        values: {"PatternRules[Spore].Cooldown": 6, "SlamDamage": "2.0", ...} (text, numbers or bools).
        Nothing is saved - call asset_save.

        Returns {object_path, dry_run, changes: [{path, old_value, new_value, changed}]}."""
        return conn.call("object.set_properties", {"object_path": object_path, "values": values, "dry_run": dry_run})

    @mcp.tool()
    def object_diff_from_defaults(object_path: str, against: str = "class", edit_only: bool = True) -> dict:
        """Read-only. The properties whose value differs from the class defaults - what a data asset
        or Blueprint actually overrides.

        against: "class" (the object's own class defaults, so a Blueprint's defaults count as defaults)
        or "native" (the nearest C++ class's defaults - what the code would give).
        edit_only: only properties the details panel shows.

        Returns {object_path, against, against_class, differences: [{name, type, value, default}]}."""
        return conn.call("object.diff_from_defaults", {"object_path": object_path, "against": against, "edit_only": edit_only})
