from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_blueprint_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def blueprint_create(
        package_path: str,
        asset_name: str,
        parent_class_path: str,
    ) -> dict:
        if not package_path:
            raise ValueError("package_path is required")
        if not asset_name:
            raise ValueError("asset_name is required")
        if not parent_class_path:
            raise ValueError("parent_class_path is required")
        return conn.call("blueprint.create", {
            "package_path": package_path,
            "asset_name": asset_name,
            "parent_class_path": parent_class_path,
        })

    @mcp.tool()
    def blueprint_compile(blueprint_path: str) -> dict:
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        return conn.call("blueprint.compile", {"blueprint_path": blueprint_path})

    @mcp.tool()
    def blueprint_list_variables(blueprint_path: str) -> dict:
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        return conn.call("blueprint.list_variables", {"blueprint_path": blueprint_path})

    @mcp.tool()
    def blueprint_get_graph_nodes(blueprint_path: str, graph_name: str = "") -> dict:
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        params: dict = {"blueprint_path": blueprint_path}
        if graph_name:
            params["graph_name"] = graph_name
        return conn.call("blueprint.get_graph_nodes", params)

    @mcp.tool()
    def blueprint_list_functions(blueprint_path: str, graph_name: str = "") -> dict:
        """Lightweight inventory of a Blueprint's graphs: name, graph_type
        (ubergraph/function/macro), node_count, and for function graphs the
        resolved input/output signature. Use this before blueprint_get_graph_nodes
        to see what's there without pulling every node."""
        if not blueprint_path:
            raise ValueError("blueprint_path is required")
        params: dict = {"blueprint_path": blueprint_path}
        if graph_name:
            params["graph_name"] = graph_name
        return conn.call("blueprint.list_functions", params)

    @mcp.tool()
    def blueprint_connect_pins(asset_path: str, source_node_name: str, source_pin_name: str, target_node_name: str, target_pin_name: str) -> dict:
        """ConnectPins

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::ConnectPins; no other plugin needed."""
        return conn.call("blueprint.connect_pins", {"asset_path": asset_path, "source_node_name": source_node_name, "source_pin_name": source_pin_name, "target_node_name": target_node_name, "target_pin_name": target_pin_name})

    @mcp.tool()
    def blueprint_connect_pins_in_graph(asset_path: str, graph_name: str, source_node_name: str, source_pin_name: str, target_node_name: str, target_pin_name: str) -> dict:
        """특정 그래프 스코프 내에서 핀 연결. 같은 이름 노드가 여러 그래프에 있을 때 사용.
        ConnectPins와 달리 지정한 그래프 내 노드만 검색.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::ConnectPinsInGraph; no other plugin needed."""
        return conn.call("blueprint.connect_pins_in_graph", {"asset_path": asset_path, "graph_name": graph_name, "source_node_name": source_node_name, "source_pin_name": source_pin_name, "target_node_name": target_node_name, "target_pin_name": target_pin_name})

    @mcp.tool()
    def blueprint_disconnect_pin(asset_path: str, node_name: str, pin_name: str) -> dict:
        """DisconnectPin

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::DisconnectPin; no other plugin needed."""
        return conn.call("blueprint.disconnect_pin", {"asset_path": asset_path, "node_name": node_name, "pin_name": pin_name})

    @mcp.tool()
    def blueprint_set_pin_default_value(asset_path: str, node_name: str, pin_name: str, new_value: str) -> dict:
        """SetPinDefaultValue

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::SetPinDefaultValue; no other plugin needed."""
        return conn.call("blueprint.set_pin_default_value", {"asset_path": asset_path, "node_name": node_name, "pin_name": pin_name, "new_value": new_value})

    @mcp.tool()
    def blueprint_set_pin_default_value_by_graph_index(asset_path: str, graph_index: int, node_index: int, pin_name: str, new_value: str) -> dict:
        """그래프/노드 인덱스 기반 핀 기본값 설정. 같은 이름 노드가 여럿일 때 사용.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::SetPinDefaultValueByGraphIndex; no other plugin needed."""
        return conn.call("blueprint.set_pin_default_value_by_graph_index", {"asset_path": asset_path, "graph_index": graph_index, "node_index": node_index, "pin_name": pin_name, "new_value": new_value})

    @mcp.tool()
    def blueprint_remove_node(asset_path: str, node_name: str) -> dict:
        """RemoveNode

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::RemoveNode; no other plugin needed."""
        return conn.call("blueprint.remove_node", {"asset_path": asset_path, "node_name": node_name})

    @mcp.tool()
    def blueprint_add_call_function_node(asset_path: str, graph_name: str, function_name: str, pos_x: int, pos_y: int) -> dict:
        """AddCallFunctionNode

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::AddCallFunctionNode; no other plugin needed."""
        return conn.call("blueprint.add_call_function_node", {"asset_path": asset_path, "graph_name": graph_name, "function_name": function_name, "pos_x": pos_x, "pos_y": pos_y})

    @mcp.tool()
    def blueprint_add_variable_get_node(asset_path: str, graph_name: str, var_name: str, pos_x: int, pos_y: int) -> dict:
        """변수 Getter 노드(K2Node_VariableGet) 를 그래프에 추가. VarName = Blueprint 멤버 변수 이름. 반환: 생성 노드 이름 (빈 문자열 = 실패).

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::AddVariableGetNode; no other plugin needed."""
        return conn.call("blueprint.add_variable_get_node", {"asset_path": asset_path, "graph_name": graph_name, "var_name": var_name, "pos_x": pos_x, "pos_y": pos_y})

    @mcp.tool()
    def blueprint_add_variable_set_node(asset_path: str, graph_name: str, var_name: str, pos_x: int, pos_y: int) -> dict:
        """변수 Setter 노드(K2Node_VariableSet) 를 그래프에 추가.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::AddVariableSetNode; no other plugin needed."""
        return conn.call("blueprint.add_variable_set_node", {"asset_path": asset_path, "graph_name": graph_name, "var_name": var_name, "pos_x": pos_x, "pos_y": pos_y})

    @mcp.tool()
    def blueprint_add_array_get_item_node(asset_path: str, graph_name: str, pos_x: int, pos_y: int) -> dict:
        """Array Get Item 노드(K2Node_GetArrayItem) 를 그래프에 추가. UE Editor 에서 변수 핀 드래그→Get 으로 만든 것과 동일 — wildcard 자동 type 추론 지원. 반환: 생성 노드 이름.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::AddArrayGetItemNode; no other plugin needed."""
        return conn.call("blueprint.add_array_get_item_node", {"asset_path": asset_path, "graph_name": graph_name, "pos_x": pos_x, "pos_y": pos_y})

    @mcp.tool()
    def blueprint_reconstruct_node(asset_path: str, node_name: str) -> dict:
        """노드 이름으로 재구성 (핀 재생성).

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::ReconstructNode; no other plugin needed."""
        return conn.call("blueprint.reconstruct_node", {"asset_path": asset_path, "node_name": node_name})

    @mcp.tool()
    def blueprint_reconstruct_node_by_graph_index(asset_path: str, graph_index: int, node_index: int) -> dict:
        """그래프/노드 인덱스로 노드 재구성. Enum 변경 후 BlendPoses 등 핀 에러 복구에 사용. RemovedPins도 클리어.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::ReconstructNodeByGraphIndex; no other plugin needed."""
        return conn.call("blueprint.reconstruct_node_by_graph_index", {"asset_path": asset_path, "graph_index": graph_index, "node_index": node_index})

    @mcp.tool()
    def blueprint_get_graphs(asset_path: str) -> dict:
        """GetBlueprintGraphs

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::GetBlueprintGraphs; no other plugin needed."""
        return conn.call("blueprint.get_graphs", {"asset_path": asset_path})

    @mcp.tool()
    def blueprint_get_graph_nodes_by_index(asset_path: str, graph_index: int) -> dict:
        """그래프 인덱스로 노드 목록 반환. 같은 이름 그래프가 여럿일 때 인덱스로 구분.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::GetGraphNodesByIndex; no other plugin needed."""
        return conn.call("blueprint.get_graph_nodes_by_index", {"asset_path": asset_path, "graph_index": graph_index})

    @mcp.tool()
    def blueprint_get_all_nodes(asset_path: str) -> dict:
        """GetAllNodesInBlueprint

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::GetAllNodesInBlueprint; no other plugin needed."""
        return conn.call("blueprint.get_all_nodes", {"asset_path": asset_path})

    @mcp.tool()
    def blueprint_duplicate_with_remap(source_asset_path: str, dest_asset_path: str, new_skeleton: str, asset_substitutions: dict[str, str]) -> dict:
        """소스 Blueprint 를 복제하고 모든 자산 참조를 AssetSubstitutions 맵 따라 일괄 치환.
        
        동작:
          1. 소스 BP 로드 → DestAssetPath 위치에 DuplicateLoadedAsset
          2. 새 BP 의 GeneratedClass CDO + SCS 컴포넌트 템플릿 + 모든 그래프 노드 +
             핀 DefaultObject / DefaultValue 를 reflection 으로 walk
          3. AssetSubstitutions 의 (old path → new path) 따라 ObjectProperty / SoftObjectProperty /
             ClassProperty / 핀 DefaultObject / 핀 DefaultValue 치환
          4. NewSkeleton 이 비어있지 않고 BP 가 UAnimBlueprint 면 TargetSkeleton 변경
          5. 컴파일 + 저장
        
        source_asset_path: 소스 Blueprint 의 asset path (예: "/Game/.../F_TaoistAnimBP.F_TaoistAnimBP")
        dest_asset_path: 생성할 대상 BP 의 asset path (이미 존재하면 실패)
        new_skeleton: AnimBP 인 경우 TargetSkeleton 변경 (빈 문자열이면 원본 스켈레톤 유지)
        asset_substitutions: 자산 치환 맵 (key=old path, value=new path).
                                   path 는 "/Game/.../Asset.Asset" 또는 "/Game/.../Asset" (확장 자동).
        Returns: 성공 시 새 BP 의 asset path. 실패 시 빈 문자열.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::DuplicateBlueprintWithRemap; no other plugin needed."""
        return conn.call("blueprint.duplicate_with_remap", {"source_asset_path": source_asset_path, "dest_asset_path": dest_asset_path, "new_skeleton": new_skeleton, "asset_substitutions": asset_substitutions})

    @mcp.tool()
    def blueprint_reparent_checked(blueprint_path: str, expected_old_parent: str, new_parent: str) -> dict:
        """Checked Blueprint parent replacement, compile, save, and readback.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::ReparentBlueprintChecked; no other plugin needed."""
        return conn.call("blueprint.reparent_checked", {"blueprint_path": blueprint_path, "expected_old_parent": expected_old_parent, "new_parent": new_parent})

    @mcp.tool()
    def blueprint_get_header(asset_path: str) -> dict:
        """AnimBP의 헤더(부모 클래스/스켈레톤/인터페이스/변수/그래프 이름) 추출.
        UE 4.27 Python API에서 noexpose/protected 처리된 UBlueprint 프로퍼티들을 cpp에서 우회하여 노출.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::GetAnimBlueprintHeader; no other plugin needed."""
        return conn.call("blueprint.get_header", {"asset_path": asset_path})

    @mcp.tool()
    def blueprint_dump_asset_references(asset_path: str) -> dict:
        """DumpAssetReferences

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::DumpAssetReferences; no other plugin needed."""
        return conn.call("blueprint.dump_asset_references", {"asset_path": asset_path})

    @mcp.tool()
    def blueprint_dump_cdo_references(asset_path: str) -> dict:
        """BP 의 GeneratedClass CDO 와 SimpleConstructionScript 의 모든 컴포넌트 템플릿을
        reflection 으로 재귀 walk 해서 자산 패키지 경로(`/Game/...`, `/Engine/...`)
        를 평탄 list 로 dump. ObjectProperty / SoftObjectProperty / ClassProperty /
        Array / Struct / Map / Set 모두 처리.
        
        BP graph 노드에 안 보이는 자산 참조 (Mesh Component 의 default StaticMesh,
        ParticleSystem Component 의 Template, Character 의 SkeletalMesh / AnimBP,
        UPROPERTY(EditDefaultsOnly) UTexture2D* 등) 를 회수하는 데 사용.
        
        자기 자신 패키지는 결과에서 제외. 중복 제거 후 정렬.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::DumpCDOReferences; no other plugin needed."""
        return conn.call("blueprint.dump_cdo_references", {"asset_path": asset_path})

    @mcp.tool()
    def blueprint_dump_function_calls(asset_path: str) -> dict:
        """DumpFunctionCalls

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::DumpFunctionCalls; no other plugin needed."""
        return conn.call("blueprint.dump_function_calls", {"asset_path": asset_path})

    @mcp.tool()
    def blueprint_dump_variable_usages(asset_path: str) -> dict:
        """DumpVariableUsages

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::DumpVariableUsages; no other plugin needed."""
        return conn.call("blueprint.dump_variable_usages", {"asset_path": asset_path})

    @mcp.tool()
    def blueprint_dump_event_entries(asset_path: str) -> dict:
        """DumpEventEntries

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::DumpEventEntries; no other plugin needed."""
        return conn.call("blueprint.dump_event_entries", {"asset_path": asset_path})

    @mcp.tool()
    def blueprint_dump_function_signatures(asset_path: str) -> dict:
        """DumpFunctionSignatures

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::DumpFunctionSignatures; no other plugin needed."""
        return conn.call("blueprint.dump_function_signatures", {"asset_path": asset_path})

    @mcp.tool()
    def blueprint_dump_mpc_bindings(blueprint_path: str, collection_path: str) -> dict:
        """Exact Blueprint MPC calls bound to CollectionPath, including node/pin identity. Read-only.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::DumpBlueprintMPCBindingsJson; no other plugin needed."""
        return conn.call("blueprint.dump_mpc_bindings", {"blueprint_path": blueprint_path, "collection_path": collection_path})

    @mcp.tool()
    def blueprint_remap_mpc_call_checked(blueprint_path: str, graph_name: str, node_guid: str, collection_path: str, expected_source_name: str, retained_target_name: str, expected_shared_guid: str) -> dict:
        """Checked literal ParameterName remap on one positively identified MPC call node.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::RemapMPCBlueprintCallChecked; no other plugin needed."""
        return conn.call("blueprint.remap_mpc_call_checked", {"blueprint_path": blueprint_path, "graph_name": graph_name, "node_guid": node_guid, "collection_path": collection_path, "expected_source_name": expected_source_name, "retained_target_name": retained_target_name, "expected_shared_guid": expected_shared_guid})

    @mcp.tool()
    def blueprint_create_interface(asset_path: str) -> dict:
        """Blueprint Interface authoring (missing from the official UE MCP toolsets and the editor Python API):
        create Blueprint Interface assets, add interface functions with typed parameters,
        implement/remove interfaces on Blueprints, list implemented interfaces.
        
        Pin type strings: bool, byte, int, int64, float, double, string, name, text,
        vector, rotator, transform, linearcolor, object:<ClassPath>, class:<ClassPath>,
        softobject:<ClassPath>, struct:<StructPath>, enum:<EnumPath>, and the prefix "array:" e.g. "array:int".
        Parameter list format: "Name:Type,Name2:Type2" (empty string = none).
        Changes are compiled but NOT saved; save with the official asset tools or SaveBlueprint.
        /
        UCLASS(BlueprintType)
        class BLUEPRINTINSPECTOR_API UBPIInterfaceToolset : public UToolsetDefinition
        {
        	GENERATED_BODY()
        
        public:
        	virtual FString GetToolsetVersion() const override { return TEXT("1.0"); }
        
        	/**
        Create a new Blueprint Interface asset.
        asset_path: Package path of the new asset, e.g. "/Game/Interfaces/BPI_Interactable". Fails if it exists.
        Returns: Object path of the created interface, or an "ERROR|..." string.

        Built-in port of BlueprintInspector's BPIInterfaceToolset::CreateBlueprintInterface; no other plugin needed."""
        return conn.call("blueprint.create_interface", {"asset_path": asset_path})

    @mcp.tool()
    def blueprint_add_interface_function(interface_path: str, function_name: str, inputs: str, outputs: str) -> dict:
        """Add a function to a Blueprint Interface. Functions without outputs become implementable events.
        interface_path: Blueprint Interface asset path.
        function_name: New function name.
        inputs: "Name:Type,..." input parameters.
        outputs: "Name:Type,..." output parameters (makes it a function instead of an event).
        Returns: "OK" or "ERROR|...".

        Built-in port of BlueprintInspector's BPIInterfaceToolset::AddInterfaceFunction; no other plugin needed."""
        return conn.call("blueprint.add_interface_function", {"interface_path": interface_path, "function_name": function_name, "inputs": inputs, "outputs": outputs})

    @mcp.tool()
    def blueprint_implement_interface(blueprint_path: str, interface_path: str) -> dict:
        """Make a Blueprint implement an interface (Blueprint Interface asset path or native interface class path like "/Script/Engine.Interface_AssetUserData").
        Adds stub graphs for interface functions. Compiles the Blueprint.
        Returns: "OK" or "ERROR|...".

        Built-in port of BlueprintInspector's BPIInterfaceToolset::ImplementInterface; no other plugin needed."""
        return conn.call("blueprint.implement_interface", {"blueprint_path": blueprint_path, "interface_path": interface_path})

    @mcp.tool()
    def blueprint_remove_interface(blueprint_path: str, interface_path: str, preserve_functions: bool) -> dict:
        """Remove an implemented interface from a Blueprint.
        preserve_functions: Keep the implemented function graphs as normal functions.
        Returns: "OK" or "ERROR|...".

        Built-in port of BlueprintInspector's BPIInterfaceToolset::RemoveInterface; no other plugin needed."""
        return conn.call("blueprint.remove_interface", {"blueprint_path": blueprint_path, "interface_path": interface_path, "preserve_functions": preserve_functions})

    @mcp.tool()
    def blueprint_list_implemented_interfaces(blueprint_path: str) -> dict:
        """List interfaces implemented by a Blueprint (including inherited ones, marked).
        Returns: JSON array: [{"interface":path,"inherited":bool,"graphs":[names]}]

        Built-in port of BlueprintInspector's BPIInterfaceToolset::ListImplementedInterfaces; no other plugin needed."""
        return conn.call("blueprint.list_implemented_interfaces", {"blueprint_path": blueprint_path})
