from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_blendspace_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def blendspace_dump_structure(asset_path: str) -> dict:
        """블렌드스페이스 구조 덤프: 축/샘플(좌표·RateScale·유효성)/보간·루프 설정. 1D/2D/AimOffset 공용.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::DumpBlendSpaceStructure; no other plugin needed."""
        return conn.call("blendspace.dump_structure", {"asset_path": asset_path})

    @mcp.tool()
    def blendspace_add_sample(asset_path: str, sequence_path: str, x: float, y: float) -> dict:
        """샘플 추가. 1D 는 Y 무시. Returns: 샘플 인덱스 (-1 실패)

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::AddBlendSpaceSample; no other plugin needed."""
        return conn.call("blendspace.add_sample", {"asset_path": asset_path, "sequence_path": sequence_path, "x": x, "y": y})

    @mcp.tool()
    def blendspace_move_sample(asset_path: str, sample_index: int, x: float, y: float) -> dict:
        """MoveBlendSpaceSample

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::MoveBlendSpaceSample; no other plugin needed."""
        return conn.call("blendspace.move_sample", {"asset_path": asset_path, "sample_index": sample_index, "x": x, "y": y})

    @mcp.tool()
    def blendspace_replace_sample_animation(asset_path: str, sample_index: int, sequence_path: str) -> dict:
        """ReplaceBlendSpaceSampleAnimation

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::ReplaceBlendSpaceSampleAnimation; no other plugin needed."""
        return conn.call("blendspace.replace_sample_animation", {"asset_path": asset_path, "sample_index": sample_index, "sequence_path": sequence_path})

    @mcp.tool()
    def blendspace_remove_sample(asset_path: str, sample_index: int) -> dict:
        """샘플 삭제. 이후 SampleIndex 재배열 — 재덤프 필요.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::RemoveBlendSpaceSample; no other plugin needed."""
        return conn.call("blendspace.remove_sample", {"asset_path": asset_path, "sample_index": sample_index})

    @mcp.tool()
    def blendspace_set_sample_rate_scale(asset_path: str, sample_index: int, rate_scale: float) -> dict:
        """SetBlendSpaceSampleRateScale

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::SetBlendSpaceSampleRateScale; no other plugin needed."""
        return conn.call("blendspace.set_sample_rate_scale", {"asset_path": asset_path, "sample_index": sample_index, "rate_scale": rate_scale})

    @mcp.tool()
    def blendspace_set_axis(asset_path: str, axis_index: int, axis_name: str, min: float, max: float, grid_num: int, snap_to_grid: bool, wrap_input: bool) -> dict:
        """축 파라미터 설정. AxisIndex: 0=가로, 1=세로(2D 전용). 설정 후 Resample.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::SetBlendSpaceAxis; no other plugin needed."""
        return conn.call("blendspace.set_axis", {"asset_path": asset_path, "axis_index": axis_index, "axis_name": axis_name, "min": min, "max": max, "grid_num": grid_num, "snap_to_grid": snap_to_grid, "wrap_input": wrap_input})
