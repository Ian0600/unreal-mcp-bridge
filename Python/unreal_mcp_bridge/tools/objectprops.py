from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_object_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def object_list_properties(object_path: str, include_values: bool = True) -> dict:
        """Lists every reflected property on any Unreal object, with its current value.

        object_path is a full path — no short names:
          /Game/Path/Asset.Asset                  the asset
          /Game/Path/Asset.Asset:SubObject        an instanced subobject
          /Game/Map.Map:PersistentLevel.Actor_0   a live actor (PIE worlds included)

        Reaches properties the editor's Python API cannot: private and protected members,
        and classes declared UCLASS(MinimalAPI) that have no Python binding at all. Each
        entry carries flags (Edit, BlueprintVisible, Transient, Instanced, ...) so you can
        see whether the details panel would have shown it.

        Returns {"object_path", "object_class", "properties": [{name, type, class, flags,
        value}]}."""
        if not object_path:
            raise ValueError("object_path is required")
        return conn.call("object.list_properties", {
            "object_path": object_path,
            "include_values": include_values,
        })

    @mcp.tool()
    def object_get_property(object_path: str, property_name: str) -> dict:
        """Reads one property off any Unreal object as Unreal text format.

        property_name walks nested values: "BoomOffset", "RelativeLocation.X",
        "Children[2]", "Children[2].bIsEnabled".

        Read before you write — the returned string is exactly the format
        object_set_property expects. Returns {"object_path", "property_name", "type",
        "value"}."""
        for name, val in (("object_path", object_path), ("property_name", property_name)):
            if not val:
                raise ValueError(f"{name} is required")
        return conn.call("object.get_property", {
            "object_path": object_path,
            "property_name": property_name,
        })

    @mcp.tool()
    def object_set_property(object_path: str, property_name: str, value: str) -> dict:
        """Writes one property on any Unreal object from Unreal text format.

        Structs are "(X=1.0,Y=2.0)", arrays are "(Elem1,Elem2)", object references are
        their full path. property_name supports the same nested paths as
        object_get_property. The reliable way to build a value is to read the property
        first and edit the string it gives back.

        Nothing downstream is rebuilt: a camera rig still needs camerarig_build, a
        Blueprint still needs blueprint_compile, and nothing reaches disk until
        asset_save. Returns {"ok", "object_path", "old_value", "new_value"}."""
        for name, val in (("object_path", object_path), ("property_name", property_name)):
            if not val:
                raise ValueError(f"{name} is required")
        if value is None:
            raise ValueError("value is required")
        return conn.call("object.set_property", {
            "object_path": object_path,
            "property_name": property_name,
            "value": value,
        })
