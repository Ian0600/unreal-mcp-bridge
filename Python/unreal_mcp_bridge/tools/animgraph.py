from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_animgraph_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def animgraph_add_state_machine(
        blueprint_path: str,
        graph_name: str = "AnimGraph",
        pos_x: int = 0,
        pos_y: int = 0,
    ) -> dict:
        """Adds a State Machine node to an AnimBlueprint's AnimGraph. Auto-creates the
        machine's inner graph + Entry node. Returns {"node_name": ...} to pass as
        state_machine_node to animgraph_add_state / animgraph_add_transition."""
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        return conn.call("animgraph.add_state_machine", {
            "blueprint_path": blueprint_path,
            "graph_name": graph_name,
            "pos_x": pos_x,
            "pos_y": pos_y,
        })

    @mcp.tool()
    def animgraph_add_state(
        blueprint_path: str,
        state_machine_node: str,
        state_name: str,
        graph_name: str = "AnimGraph",
        pos_x: int = 0,
        pos_y: int = 0,
    ) -> dict:
        """Adds a state to a state machine's inner graph. Auto-creates the state's bound
        graph (with its Output Animation Pose sink). The first state added is auto-wired
        to the machine's Entry node."""
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        if not state_machine_node:
            raise ValueError("state_machine_node is required")
        if not state_name:
            raise ValueError("state_name is required")
        return conn.call("animgraph.add_state", {
            "blueprint_path": blueprint_path,
            "state_machine_node": state_machine_node,
            "state_name": state_name,
            "graph_name": graph_name,
            "pos_x": pos_x,
            "pos_y": pos_y,
        })

    @mcp.tool()
    def animgraph_add_blend_space_player(
        blueprint_path: str,
        state_machine_node: str,
        state_name: str,
        blend_space_path: str,
        graph_name: str = "AnimGraph",
        pos_x: int = 0,
        pos_y: int = 0,
    ) -> dict:
        """Adds a BlendSpacePlayer node inside state_name's bound graph, sets its
        BlendSpace asset, and wires it to the state's pose sink. The blend space's axis
        input pins (typically X/Y) are left unwired — connect them with a follow-up
        blueprint graph edit if the axes need to be driven by variables."""
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        if not state_machine_node:
            raise ValueError("state_machine_node is required")
        if not state_name:
            raise ValueError("state_name is required")
        if not blend_space_path:
            raise ValueError("blend_space_path is required")
        return conn.call("animgraph.add_blend_space_player", {
            "blueprint_path": blueprint_path,
            "state_machine_node": state_machine_node,
            "state_name": state_name,
            "blend_space_path": blend_space_path,
            "graph_name": graph_name,
            "pos_x": pos_x,
            "pos_y": pos_y,
        })

    @mcp.tool()
    def animgraph_add_transition(
        blueprint_path: str,
        state_machine_node: str,
        from_state: str,
        to_state: str,
        graph_name: str = "AnimGraph",
        condition_property: str = "",
        negate_condition: bool = False,
    ) -> dict:
        """Adds a transition between two states. If condition_property is set (e.g.
        "bIsInAir", a bool property on the AnimBlueprint's parent AnimInstance class),
        the transition rule is wired to `Get <condition_property>` -> Result, optionally
        negated via negate_condition."""
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        if not state_machine_node:
            raise ValueError("state_machine_node is required")
        if not from_state:
            raise ValueError("from_state is required")
        if not to_state:
            raise ValueError("to_state is required")
        params: dict = {
            "blueprint_path": blueprint_path,
            "state_machine_node": state_machine_node,
            "from_state": from_state,
            "to_state": to_state,
            "graph_name": graph_name,
            "negate_condition": negate_condition,
        }
        if condition_property:
            params["condition_property"] = condition_property
        return conn.call("animgraph.add_transition", params)

    @mcp.tool()
    def animgraph_add_variable_get(
        blueprint_path: str,
        graph_name: str,
        property_name: str,
        pos_x: int = 0,
        pos_y: int = 0,
    ) -> dict:
        """Adds a `Get <property_name>` node to a graph. graph_name resolves nested anim
        graphs too, so pass a state's name to add the getter inside that state's bound
        graph (e.g. to drive a BlendSpacePlayer's Speed axis from GroundSpeed). The new
        node's output pin is named after the property. Returns {"node_name": ...}."""
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        if not graph_name:
            raise ValueError("graph_name is required")
        if not property_name:
            raise ValueError("property_name is required")
        return conn.call("animgraph.add_variable_get", {
            "blueprint_path": blueprint_path,
            "graph_name": graph_name,
            "property_name": property_name,
            "pos_x": pos_x,
            "pos_y": pos_y,
        })

    @mcp.tool()
    def animgraph_connect_pins(
        blueprint_path: str,
        graph_name: str,
        src_node: str,
        src_pin: str,
        dst_node: str,
        dst_pin: str,
    ) -> dict:
        """Connects src_node's output pin to dst_node's input pin within one graph. Use to
        wire a variable getter into a node's input, e.g. src_pin="GroundSpeed" (a
        Get GroundSpeed node) -> dst_pin="X" (a BlendSpacePlayer's axis pin). Returns
        {"connected": true}."""
        for name, value in (
            ("blueprint_path", blueprint_path), ("graph_name", graph_name),
            ("src_node", src_node), ("src_pin", src_pin),
            ("dst_node", dst_node), ("dst_pin", dst_pin),
        ):
            if not value:
                raise ValueError(f"{name} is required")
        return conn.call("animgraph.connect_pins", {
            "blueprint_path": blueprint_path,
            "graph_name": graph_name,
            "src_node": src_node,
            "src_pin": src_pin,
            "dst_node": dst_node,
            "dst_pin": dst_pin,
        })

    @mcp.tool()
    def animgraph_disconnect_pins(
        blueprint_path: str,
        graph_name: str,
        node_a: str,
        pin_a: str,
        node_b: str,
        pin_b: str,
    ) -> dict:
        """Removes a specific link between two pins in a graph. Returns
        {"disconnected": true}."""
        for name, value in (
            ("blueprint_path", blueprint_path), ("graph_name", graph_name),
            ("node_a", node_a), ("pin_a", pin_a),
            ("node_b", node_b), ("pin_b", pin_b),
        ):
            if not value:
                raise ValueError(f"{name} is required")
        return conn.call("animgraph.disconnect_pins", {
            "blueprint_path": blueprint_path,
            "graph_name": graph_name,
            "node_a": node_a,
            "pin_a": pin_a,
            "node_b": node_b,
            "pin_b": pin_b,
        })

    @mcp.tool()
    def animgraph_add_sequence_player(
        blueprint_path: str,
        state_machine_node: str,
        state_name: str,
        sequence_path: str,
        graph_name: str = "AnimGraph",
        loop: bool = True,
        pos_x: int = 0,
        pos_y: int = 0,
    ) -> dict:
        """Adds a SequencePlayer node inside state_name's bound graph, sets its AnimSequence
        + loop flag, and wires it to the state's pose sink. Use for one-shot transition
        clips (Start/Stop) in a dedicated state. Returns {"node_name": ...}."""
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        if not state_machine_node:
            raise ValueError("state_machine_node is required")
        if not state_name:
            raise ValueError("state_name is required")
        if not sequence_path:
            raise ValueError("sequence_path is required")
        return conn.call("animgraph.add_sequence_player", {
            "blueprint_path": blueprint_path,
            "state_machine_node": state_machine_node,
            "state_name": state_name,
            "sequence_path": sequence_path,
            "graph_name": graph_name,
            "loop": loop,
            "pos_x": pos_x,
            "pos_y": pos_y,
        })

    @mcp.tool()
    def animgraph_add_slot(
        blueprint_path: str,
        slot_name: str,
        graph_name: str = "AnimGraph",
        pos_x: int = 0,
        pos_y: int = 0,
    ) -> dict:
        """Adds a Slot node (montage layering point) to the top-level AnimGraph. slot_name
        must match a slot defined on the Skeleton (e.g. "UpperBody"). Pins: "Source" (in) /
        "Pose" (out) — wire both with animgraph_connect_pins so the slot sits in series
        between the pose source and the output. Returns {"node_name": ...}."""
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        if not slot_name:
            raise ValueError("slot_name is required")
        return conn.call("animgraph.add_slot", {
            "blueprint_path": blueprint_path,
            "slot_name": slot_name,
            "graph_name": graph_name,
            "pos_x": pos_x,
            "pos_y": pos_y,
        })

    @mcp.tool()
    def animgraph_remove_node(
        blueprint_path: str,
        graph_name: str,
        node_name: str,
    ) -> dict:
        """Removes a node from the named graph (state bound graphs resolve by state name).
        Returns {"removed": true}."""
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        if not graph_name:
            raise ValueError("graph_name is required")
        if not node_name:
            raise ValueError("node_name is required")
        return conn.call("animgraph.remove_node", {
            "blueprint_path": blueprint_path,
            "graph_name": graph_name,
            "node_name": node_name,
        })

    @mcp.tool()
    def animgraph_add_nested_state_machine(asset_path: str, parent_sm_node_name: str, parent_state_name: str, nested_machine_name: str, pos_x: int, pos_y: int) -> dict:
        """특정 State 내부 그래프에 네스티드 StateMachine 삽입.
        부모 State의 BoundGraph 안에 UAnimGraphNode_StateMachine을 생성하고
        출력 포즈를 StateResult.Result 핀에 자동 연결.
        이미 Result에 연결된 노드가 있으면 연결을 끊고 교체.
        
        parent_sm_node_name: 부모 SM 노드 이름 (AddStateMachine 반환값)
        parent_state_name: 내부 그래프를 사용할 State 이름 (AddStateToStateMachine 반환값)
        nested_machine_name: 네스티드 SM 그래프 표시 이름
        Returns: 생성된 네스티드 SM 노드 이름, 실패 시 빈 문자열

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::AddNestedStateMachine; no other plugin needed."""
        return conn.call("animgraph.add_nested_state_machine", {"asset_path": asset_path, "parent_sm_node_name": parent_sm_node_name, "parent_state_name": parent_state_name, "nested_machine_name": nested_machine_name, "pos_x": pos_x, "pos_y": pos_y})

    @mcp.tool()
    def animgraph_set_all_transitions_automatic(asset_path: str, sm_node_name: str) -> dict:
        """SM 내 모든 Transition의 bAutomaticRuleBasedOnSequencePlayerInState 활성화.
        노드 연결 없이 "will never be taken" 경고 제거용.
        Returns: 처리된 Transition 수

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::SetAllTransitionsAutomatic; no other plugin needed."""
        return conn.call("animgraph.set_all_transitions_automatic", {"asset_path": asset_path, "sm_node_name": sm_node_name})

    @mcp.tool()
    def animgraph_fix_blend_list_by_enum_nodes(asset_path: str, enum_path: str) -> dict:
        """AnimBP의 모든 BlendListByEnum 노드에서 지정 Enum의 구버전 항목을 제거하고 재구성.
        Enum 경로 변경 후 VisibleEnumEntries 정리에 사용.
        enum_path: UEnum 오브젝트 경로 (예: "/Game/Enums/E_MyEnum.E_MyEnum")
        Returns: 처리된 노드 수, 음수이면 에러 (-1: BP 로드 실패, -2: Enum 로드 실패)

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::FixBlendListByEnumNodes; no other plugin needed."""
        return conn.call("animgraph.fix_blend_list_by_enum_nodes", {"asset_path": asset_path, "enum_path": enum_path})

    @mcp.tool()
    def animgraph_rebuild_blend_list_enum_entries(asset_path: str, enum_path: str, entry_names: list[str]) -> dict:
        """AnimBP의 모든 BlendListByEnum 노드의 VisibleEnumEntries를 EntryNames로 완전 재설정.
        Returns: 처리된 노드 수

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::RebuildBlendListEnumEntries; no other plugin needed."""
        return conn.call("animgraph.rebuild_blend_list_enum_entries", {"asset_path": asset_path, "enum_path": enum_path, "entry_names": entry_names})

    @mcp.tool()
    def animgraph_set_blend_list_enum_entries_by_index(asset_path: str, graph_index: int, node_index: int, entry_names: list[str]) -> dict:
        """특정 그래프/노드 인덱스의 BlendListByEnum 노드 VisibleEnumEntries 설정 및 재구성.
        RemovedPins도 클리어.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::SetBlendListEnumEntriesByIndex; no other plugin needed."""
        return conn.call("animgraph.set_blend_list_enum_entries_by_index", {"asset_path": asset_path, "graph_index": graph_index, "node_index": node_index, "entry_names": entry_names})

    @mcp.tool()
    def animgraph_dump_blend_list_enum_info(asset_path: str) -> dict:
        """AnimBP의 모든 BlendListByEnum 노드 정보 덤프.
        출력 형식: "g[idx]=GraphName n[idx] enum=EnumName entries=[A,B,...] pin_count=N"

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::DumpBlendListEnumInfo; no other plugin needed."""
        return conn.call("animgraph.dump_blend_list_enum_info", {"asset_path": asset_path})

    @mcp.tool()
    def animgraph_list_node_properties(asset_path: str, graph_index: int, node_index: int) -> dict:
        """노드의 UPROPERTY 목록을 "경로=타입=값" 줄로 반환. FAnimNode(Node) struct 는 1단계 재귀 ("Node.Sequence" 등).

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::ListAnimGraphNodeProperties; no other plugin needed."""
        return conn.call("animgraph.list_node_properties", {"asset_path": asset_path, "graph_index": graph_index, "node_index": node_index})

    @mcp.tool()
    def animgraph_get_node_property(asset_path: str, graph_index: int, node_index: int, property_path: str) -> dict:
        """PropertyPath("Node.Sequence", "Node.LayerSetup" 등) 값을 ExportText 문자열로 반환.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::GetAnimGraphNodePropertyByIndex; no other plugin needed."""
        return conn.call("animgraph.get_node_property", {"asset_path": asset_path, "graph_index": graph_index, "node_index": node_index, "property_path": property_path})

    @mcp.tool()
    def animgraph_set_node_property(asset_path: str, graph_index: int, node_index: int, property_path: str, value_text: str) -> dict:
        """PropertyPath 에 ImportText 로 값 기록 — 기존 노드의 시퀀스 교체("Node.Sequence"),
        LayeredBlendPerBone 필터("Node.LayerSetup"), 싱크그룹("Node.GroupName"), 슬롯("Node.SlotName") 등 범용.
        핀 구성이 변하는 프로퍼티는 변경 후 ReconstructNodeByGraphIndex 호출 권장.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::SetAnimGraphNodePropertyByIndex; no other plugin needed."""
        return conn.call("animgraph.set_node_property", {"asset_path": asset_path, "graph_index": graph_index, "node_index": node_index, "property_path": property_path, "value_text": value_text})

    @mcp.tool()
    def animgraph_dump_node_details(asset_path: str) -> dict:
        """AnimBP 내 모든 AnimGraph 노드(SequencePlayer/BlendSpacePlayer/Slot/StateMachine 등)의
        internal property(참조 에셋, slot, loop, play_rate 등)를 구조화 dump.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::DumpAnimGraphNodeDetails; no other plugin needed."""
        return conn.call("animgraph.dump_node_details", {"asset_path": asset_path})

    @mcp.tool()
    def animgraph_dump_state_machine(asset_path: str, sm_node_name: str) -> dict:
        """특정 StateMachine 노드의 상태/전이/진입 구조 dump.
        sm_node_name: AddStateMachine 반환값 또는 GetAllNodesInBlueprint 의 NodeName
                           (빈 문자열이면 첫 StateMachine 사용)

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::DumpStateMachineStructure; no other plugin needed."""
        return conn.call("animgraph.dump_state_machine", {"asset_path": asset_path, "sm_node_name": sm_node_name})

    @mcp.tool()
    def animgraph_dump_all_state_machines(asset_path: str) -> dict:
        """AnimBP 의 모든 StateMachine 구조를 한 번에 dump.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::DumpAllStateMachineStructures; no other plugin needed."""
        return conn.call("animgraph.dump_all_state_machines", {"asset_path": asset_path})

    @mcp.tool()
    def animgraph_get_settings(asset_path: str) -> dict:
        """GetAnimBlueprintSettings

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::GetAnimBlueprintSettings; no other plugin needed."""
        return conn.call("animgraph.get_settings", {"asset_path": asset_path})

    @mcp.tool()
    def animgraph_dump_linked_layer_usage(asset_path: str) -> dict:
        """AnimBP 의 LinkedAnimGraph / LinkedAnimLayer 노드 덤프 (인터페이스/대상 클래스/레이어 바인딩).

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::DumpLinkedAnimLayerUsage; no other plugin needed."""
        return conn.call("animgraph.dump_linked_layer_usage", {"asset_path": asset_path})
