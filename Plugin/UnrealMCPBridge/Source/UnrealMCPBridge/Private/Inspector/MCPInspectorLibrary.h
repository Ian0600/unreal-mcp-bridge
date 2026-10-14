#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MCPInspectorLibrary.generated.h"

USTRUCT(BlueprintType)
struct FMCPInspectorPinTypeInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString Category;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString SubCategory;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString SubCategoryObject;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString ContainerType;  // None/Array/Set/Map

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString ValueCategory;  // Map용

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString ValueSubCategoryObject;  // Map용

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	bool bIsReference = false;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	bool bIsConst = false;
};

USTRUCT(BlueprintType)
struct FMCPInspectorPinInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString PinName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString PinType;  // 레거시: PinCategory만. 신규 코드는 PinTypeFull 사용 권장

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FMCPInspectorPinTypeInfo PinTypeFull;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString Direction;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString DefaultValue;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString PinId;  // 안정 GUID

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	bool bIsConnected = false;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FString> ConnectedTo;  // 레거시: NodeTitle.PinName

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FString> ConnectedToStable;  // 신규: NodeName.PinName (안정 ID)
};

USTRUCT(BlueprintType)
struct FMCPInspectorNodeInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeClass;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeTitle;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeComment;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	int32 PosX = 0;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	int32 PosY = 0;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeGuid;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString GraphName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FMCPInspectorPinInfo> Pins;
};

// ─── AnimBP 헤더/변수/함수 ─────────────────────────────────────────────────

USTRUCT(BlueprintType)
struct FMCPInspectorVariableInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FMCPInspectorPinTypeInfo Type;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString DefaultValue;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString Category;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString Guid;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	int64 PropertyFlags = 0;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString Tooltip;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString CDODefault;  // CDO 에서 직렬화한 실제 기본값 (FString)

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	bool bIsReplicated = false;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString RepNotifyFunction;
};

USTRUCT(BlueprintType)
struct FMCPInspectorAnimBPHeader
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString AssetPath;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString ParentClass;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString TargetSkeleton;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString BlueprintType;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString GeneratedClass;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FString> ImplementedInterfaces;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FMCPInspectorVariableInfo> Variables;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FString> FunctionGraphNames;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FString> MacroGraphNames;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FString> UbergraphNames;
};

// ─── AnimGraph 노드 internal 상세 ─────────────────────────────────────────

USTRUCT(BlueprintType)
struct FMCPInspectorAnimGraphNodeDetail
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString GraphName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeClass;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeGuid;

	// Asset 참조 (SequencePlayer.Sequence / BlendSpacePlayer.BlendSpace)
	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString AnimAssetPath;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	bool bLoop = false;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	float PlayRate = 1.0f;

	// Slot
	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString SlotName;

	// StateMachine 참조 그래프 (DumpStateMachineStructure 로 상세)
	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString StateMachineGraphName;

	// 임의 추가 정보 (key=value 줄단위)
	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FString> ExtraKVPairs;
};

// ─── StateMachine 구조 ────────────────────────────────────────────────────

USTRUCT(BlueprintType)
struct FMCPInspectorStateInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString StateName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString StateClass;  // AnimStateNode / AnimStateConduitNode / AnimStateEntryNode

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString BoundGraphName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	int32 BoundGraphIndex = -1;  // GetBlueprintGraphs 결과 인덱스 (-1 = 없음)

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeGuid;
};

USTRUCT(BlueprintType)
struct FMCPInspectorTransitionInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString TransitionName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString FromStateName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString ToStateName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	float CrossfadeDuration = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString BlendMode;  // EAlphaBlendOption

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	int32 PriorityOrder = 0;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	bool bAutomaticRule = false;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString BoundGraphName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	int32 BoundGraphIndex = -1;  // GetBlueprintGraphs 결과 인덱스 (transition rule 그래프)

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeGuid;
};

// ─── 평탄 인덱스 (검색용) ─────────────────────────────────────────────────

USTRUCT(BlueprintType)
struct FMCPInspectorAssetRef
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString Category;  // Sequence / BlendSpace / Slot / Montage / Curve / Other

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString AssetPath;  // Slot 의 경우 SlotName

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString GraphName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeClass;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeGuid;
};

USTRUCT(BlueprintType)
struct FMCPInspectorFunctionCallRef
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString FunctionName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString TargetClass;  // 호출 대상 클래스 path

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString GraphName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeGuid;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	bool bIsPure = false;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	bool bIsParent = false;
};

USTRUCT(BlueprintType)
struct FMCPInspectorVariableUsageRef
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString VariableName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString MemberScope;  // Self / 외부 클래스 path

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString AccessType;  // Get / Set

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString GraphName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeGuid;
};

USTRUCT(BlueprintType)
struct FMCPInspectorEventEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString EventName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString EventOwnerClass;  // 이벤트 정의된 클래스 (override 인 경우)

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeClass;  // K2Node_Event / K2Node_FunctionEntry / K2Node_CustomEvent

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString GraphName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeGuid;
};

USTRUCT(BlueprintType)
struct FMCPInspectorFunctionParam
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FMCPInspectorPinTypeInfo Type;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString Direction;  // Input / Output

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString DefaultValue;
};

USTRUCT(BlueprintType)
struct FMCPInspectorFunctionSignature
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString FunctionName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString GraphName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	int32 GraphIndex = -1;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString GraphKind;  // Function / Macro

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	bool bIsPure = false;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	bool bIsConst = false;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FMCPInspectorFunctionParam> Inputs;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FMCPInspectorFunctionParam> Outputs;
};

USTRUCT(BlueprintType)
struct FMCPInspectorAnimBPSettings
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	bool bUseMultiThreadedAnimationUpdate = false;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	bool bWarnAboutBlueprintUsage = false;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString PreviewSkeletalMesh;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FString> ExtraKVPairs;
};

USTRUCT(BlueprintType)
struct FMCPInspectorStateMachineStructure
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString SMNodeName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString MachineGraphName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString EntryStateName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FMCPInspectorStateInfo> States;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FMCPInspectorTransitionInfo> Transitions;
};

USTRUCT(BlueprintType)
struct FMCPInspectorGraphInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString GraphName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString GraphType;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	int32 NodeCount = 0;
};

// ─── Material expression 덤프 (read-only) ─────────────────────────────────



USTRUCT(BlueprintType)
struct FMCPInspectorMaterialPropertyLink
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString Property;             // "BaseColor"/"Metallic"/"Normal"/...

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString ConnectedNode;        // 연결 expression NodeId (빈=상수/미연결)

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	int32 OutputIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	bool bUsesConstant = false;
};


// ─── Niagara LightRenderer 덤프 (read-only) ─────────────────────────────


// ─── Animation 에셋 (Montage / Notify / BlendSpace / Skeleton / LinkedLayer) ──

USTRUCT(BlueprintType)
struct FMCPInspectorMontageSegmentInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString AnimPath;             // 세그먼트가 재생하는 시퀀스 경로

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	float StartPos = 0.0f;        // 몽타주 타임라인상 시작(초)

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	float AnimStartTime = 0.0f;   // 원본 시퀀스 내 재생 시작(초)

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	float AnimEndTime = 0.0f;     // 원본 시퀀스 내 재생 끝(초)

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	float PlayRate = 1.0f;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	int32 LoopingCount = 1;
};

USTRUCT(BlueprintType)
struct FMCPInspectorMontageSlotTrackInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString SlotName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FMCPInspectorMontageSegmentInfo> Segments;
};

USTRUCT(BlueprintType)
struct FMCPInspectorMontageSectionInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString SectionName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NextSectionName;      // 빈 = 체인 끝

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	float StartTime = 0.0f;
};

USTRUCT(BlueprintType)
struct FMCPInspectorMontageInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString AssetPath;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString SkeletonPath;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	float PlayLength = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	float BlendInTime = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString BlendInOption;        // EAlphaBlendOption

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	float BlendOutTime = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString BlendOutOption;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	float BlendOutTriggerTime = -1.0f;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString SyncGroup;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	int32 SyncSlotIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FMCPInspectorMontageSectionInfo> Sections;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FMCPInspectorMontageSlotTrackInfo> SlotTracks;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	int32 NotifyCount = 0;        // 상세는 DumpNotifyDetails

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FString> ExtraKVPairs; // BlendModeIn/Out 등
};

USTRUCT(BlueprintType)
struct FMCPInspectorNotifyInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	int32 NotifyIndex = -1;       // Notifies 배열 인덱스 (mutate 대상 지정용 — mutate 후 재덤프)

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NotifyName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NotifyKind;           // "Notify" | "NotifyState" | "NameOnly"

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NotifyClassPath;      // 인스턴스 클래스 경로 (커스텀 노티파이 식별)

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	float TriggerTime = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	float Duration = 0.0f;        // NotifyState 만 > 0

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	int32 TrackIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString LinkedSlot;           // 몽타주 한정 — 링크된 슬롯트랙 이름

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FString> PropKVPairs;  // 인스턴스 EditAnywhere 프로퍼티 key=value
};

USTRUCT(BlueprintType)
struct FMCPInspectorBlendAxisInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString AxisName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	float Min = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	float Max = 100.0f;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	int32 GridNum = 4;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	bool bSnapToGrid = false;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	bool bWrapInput = false;
};

USTRUCT(BlueprintType)
struct FMCPInspectorBlendSampleInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	int32 SampleIndex = -1;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString AnimPath;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	float X = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	float Y = 0.0f;               // 1D 는 0

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	float RateScale = 1.0f;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	bool bValid = false;          // Animation 참조 유효 여부
};

USTRUCT(BlueprintType)
struct FMCPInspectorBlendSpaceInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString AssetPath;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString BlendSpaceClass;      // BlendSpace / BlendSpace1D / AimOffsetBlendSpace...

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString SkeletonPath;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	bool bLoop = false;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FMCPInspectorBlendAxisInfo> Axes;   // 1D=1개, 2D=2개

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FMCPInspectorBlendSampleInfo> Samples;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NotifyTriggerMode;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FString> ExtraKVPairs; // 보간 설정 등
};

USTRUCT(BlueprintType)
struct FMCPInspectorSkeletonSlotGroupInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString GroupName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FString> SlotNames;
};

USTRUCT(BlueprintType)
struct FMCPInspectorVirtualBoneInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString VirtualBoneName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString SourceBone;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString TargetBone;
};

USTRUCT(BlueprintType)
struct FMCPInspectorSocketInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString SocketName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString BoneName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FVector RelativeLocation = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FRotator RelativeRotation = FRotator::ZeroRotator;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FVector RelativeScale = FVector::OneVector;
};

USTRUCT(BlueprintType)
struct FMCPInspectorSkeletonInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString AssetPath;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	int32 BoneCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FMCPInspectorSkeletonSlotGroupInfo> SlotGroups;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FMCPInspectorVirtualBoneInfo> VirtualBones;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FMCPInspectorSocketInfo> Sockets;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FString> CurveNames;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FString> CompatibleSkeletons;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString PreviewMesh;
};

USTRUCT(BlueprintType)
struct FMCPInspectorLinkedLayerInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeClass;            // AnimGraphNode_LinkedAnimGraph / AnimGraphNode_LinkedAnimLayer

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString GraphName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString NodeName;

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString InterfacePath;        // LinkedAnimLayer 의 인터페이스(ALI)

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString InstanceClassPath;    // 대상 인스턴스 클래스

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	FString LayerName;            // LinkedAnimLayer 의 레이어 이름

	UPROPERTY(BlueprintReadOnly, Category = "BlueprintInspector")
	TArray<FString> InputPoseNames;
};

UCLASS()
class UMCPInspectorLibrary : public UObject
{
	GENERATED_BODY()

public:
	// ─── Graph / Node 조회 ────────────────────────────────────────────────

	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static TArray<FMCPInspectorGraphInfo> GetBlueprintGraphs(const FString& AssetPath);


	/** 그래프 인덱스로 노드 목록 반환. 같은 이름 그래프가 여럿일 때 인덱스로 구분. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static TArray<FMCPInspectorNodeInfo> GetGraphNodesByIndex(const FString& AssetPath, int32 GraphIndex);

	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static TArray<FMCPInspectorNodeInfo> GetAllNodesInBlueprint(const FString& AssetPath);

	// ─── 핀 연결 / 해제 ──────────────────────────────────────────────────

	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static bool ConnectPins(const FString& AssetPath, const FString& SourceNodeName, const FString& SourcePinName, const FString& TargetNodeName, const FString& TargetPinName);

	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static bool DisconnectPin(const FString& AssetPath, const FString& NodeName, const FString& PinName);

	/**
	 * 특정 그래프 스코프 내에서 핀 연결. 같은 이름 노드가 여러 그래프에 있을 때 사용.
	 * ConnectPins와 달리 지정한 그래프 내 노드만 검색.
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static bool ConnectPinsInGraph(const FString& AssetPath, const FString& GraphName, const FString& SourceNodeName, const FString& SourcePinName, const FString& TargetNodeName, const FString& TargetPinName);

	// ─── 핀 기본값 설정 ──────────────────────────────────────────────────

	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static bool SetPinDefaultValue(const FString& AssetPath, const FString& NodeName, const FString& PinName, const FString& NewValue);

	/** 그래프/노드 인덱스 기반 핀 기본값 설정. 같은 이름 노드가 여럿일 때 사용. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static bool SetPinDefaultValueByGraphIndex(const FString& AssetPath, int32 GraphIndex, int32 NodeIndex, const FString& PinName, const FString& NewValue);

	// ─── 노드 조작 ───────────────────────────────────────────────────────

	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static bool RemoveNode(const FString& AssetPath, const FString& NodeName);

	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static FString AddCallFunctionNode(const FString& AssetPath, const FString& GraphName, const FString& FunctionName, int32 PosX, int32 PosY);

	/** 변수 Getter 노드(K2Node_VariableGet) 를 그래프에 추가. VarName = Blueprint 멤버 변수 이름. 반환: 생성 노드 이름 (빈 문자열 = 실패). */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static FString AddVariableGetNode(const FString& AssetPath, const FString& GraphName, const FString& VarName, int32 PosX, int32 PosY);

	/** 변수 Setter 노드(K2Node_VariableSet) 를 그래프에 추가. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static FString AddVariableSetNode(const FString& AssetPath, const FString& GraphName, const FString& VarName, int32 PosX, int32 PosY);

	/** Array Get Item 노드(K2Node_GetArrayItem) 를 그래프에 추가. UE Editor 에서 변수 핀 드래그→Get 으로 만든 것과 동일 — wildcard 자동 type 추론 지원. 반환: 생성 노드 이름. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static FString AddArrayGetItemNode(const FString& AssetPath, const FString& GraphName, int32 PosX, int32 PosY);

	/** 노드 이름으로 재구성 (핀 재생성). */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static bool ReconstructNode(const FString& AssetPath, const FString& NodeName);

	/** 그래프/노드 인덱스로 노드 재구성. Enum 변경 후 BlendPoses 등 핀 에러 복구에 사용. RemovedPins도 클리어. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static bool ReconstructNodeByGraphIndex(const FString& AssetPath, int32 GraphIndex, int32 NodeIndex);

	// ─── AnimGraph 노드 생성 ─────────────────────────────────────────────


	// ─── State Machine 생성 ──────────────────────────────────────────────





	/**
	 * SM 내 모든 Transition의 bAutomaticRuleBasedOnSequencePlayerInState 활성화.
	 * 노드 연결 없이 "will never be taken" 경고 제거용.
	 * @return 처리된 Transition 수
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static int32 SetAllTransitionsAutomatic(const FString& AssetPath, const FString& SMNodeName);

	// ─── BlendListByEnum 관리 ────────────────────────────────────────────

	/**
	 * AnimBP의 모든 BlendListByEnum 노드에서 지정 Enum의 구버전 항목을 제거하고 재구성.
	 * Enum 경로 변경 후 VisibleEnumEntries 정리에 사용.
	 * @param EnumPath  UEnum 오브젝트 경로 (예: "/Game/Enums/E_MyEnum.E_MyEnum")
	 * @return 처리된 노드 수, 음수이면 에러 (-1: BP 로드 실패, -2: Enum 로드 실패)
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static int32 FixBlendListByEnumNodes(const FString& AssetPath, const FString& EnumPath);

	/**
	 * AnimBP의 모든 BlendListByEnum 노드의 VisibleEnumEntries를 EntryNames로 완전 재설정.
	 * @return 처리된 노드 수
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static int32 RebuildBlendListEnumEntries(const FString& AssetPath, const FString& EnumPath, const TArray<FString>& EntryNames);

	/**
	 * 특정 그래프/노드 인덱스의 BlendListByEnum 노드 VisibleEnumEntries 설정 및 재구성.
	 * RemovedPins도 클리어.
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static bool SetBlendListEnumEntriesByIndex(const FString& AssetPath, int32 GraphIndex, int32 NodeIndex, const TArray<FString>& EntryNames);

	/**
	 * AnimBP의 모든 BlendListByEnum 노드 정보 덤프.
	 * 출력 형식: "g[idx]=GraphName n[idx] enum=EnumName entries=[A,B,...] pin_count=N"
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static TArray<FString> DumpBlendListEnumInfo(const FString& AssetPath);

	// ─── 컴파일 / 저장 ───────────────────────────────────────────────────



	// ─── 네스티드 State Machine ──────────────────────────────────────────

	/**
	 * 특정 State 내부 그래프에 네스티드 StateMachine 삽입.
	 * 부모 State의 BoundGraph 안에 UAnimGraphNode_StateMachine을 생성하고
	 * 출력 포즈를 StateResult.Result 핀에 자동 연결.
	 * 이미 Result에 연결된 노드가 있으면 연결을 끊고 교체.
	 *
	 * @param ParentSMNodeName   부모 SM 노드 이름 (AddStateMachine 반환값)
	 * @param ParentStateName    내부 그래프를 사용할 State 이름 (AddStateToStateMachine 반환값)
	 * @param NestedMachineName  네스티드 SM 그래프 표시 이름
	 * @return 생성된 네스티드 SM 노드 이름, 실패 시 빈 문자열
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static FString AddNestedStateMachine(
		const FString& AssetPath,
		const FString& ParentSMNodeName,
		const FString& ParentStateName,
		const FString& NestedMachineName,
		int32 PosX,
		int32 PosY);

	// ─── 데이터 추출 (read-only) ────────────────────────────────────────

	/**
	 * AnimBP의 헤더(부모 클래스/스켈레톤/인터페이스/변수/그래프 이름) 추출.
	 * UE 4.27 Python API에서 noexpose/protected 처리된 UBlueprint 프로퍼티들을 cpp에서 우회하여 노출.
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Extract", meta = (AICallable))
	static FMCPInspectorAnimBPHeader GetAnimBlueprintHeader(const FString& AssetPath);

	/**
	 * AnimBP 내 모든 AnimGraph 노드(SequencePlayer/BlendSpacePlayer/Slot/StateMachine 등)의
	 * internal property(참조 에셋, slot, loop, play_rate 등)를 구조화 dump.
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Extract", meta = (AICallable))
	static TArray<FMCPInspectorAnimGraphNodeDetail> DumpAnimGraphNodeDetails(const FString& AssetPath);

	/**
	 * 특정 StateMachine 노드의 상태/전이/진입 구조 dump.
	 * @param SMNodeName  AddStateMachine 반환값 또는 GetAllNodesInBlueprint 의 NodeName
	 *                    (빈 문자열이면 첫 StateMachine 사용)
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Extract", meta = (AICallable))
	static FMCPInspectorStateMachineStructure DumpStateMachineStructure(const FString& AssetPath, const FString& SMNodeName);

	/**
	 * AnimBP 의 모든 StateMachine 구조를 한 번에 dump.
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Extract", meta = (AICallable))
	static TArray<FMCPInspectorStateMachineStructure> DumpAllStateMachineStructures(const FString& AssetPath);

	// ─── 평탄 인덱스 (검색용) ────────────────────────────────────────────

	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Extract", meta = (AICallable))
	static TArray<FMCPInspectorAssetRef> DumpAssetReferences(const FString& AssetPath);

	/**
	 * BP 의 GeneratedClass CDO 와 SimpleConstructionScript 의 모든 컴포넌트 템플릿을
	 * reflection 으로 재귀 walk 해서 자산 패키지 경로(`/Game/...`, `/Engine/...`)
	 * 를 평탄 list 로 dump. ObjectProperty / SoftObjectProperty / ClassProperty /
	 * Array / Struct / Map / Set 모두 처리.
	 *
	 * BP graph 노드에 안 보이는 자산 참조 (Mesh Component 의 default StaticMesh,
	 * ParticleSystem Component 의 Template, Character 의 SkeletalMesh / AnimBP,
	 * UPROPERTY(EditDefaultsOnly) UTexture2D* 등) 를 회수하는 데 사용.
	 *
	 * 자기 자신 패키지는 결과에서 제외. 중복 제거 후 정렬.
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Extract", meta = (AICallable))
	static TArray<FString> DumpCDOReferences(const FString& AssetPath);

	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Extract", meta = (AICallable))
	static TArray<FMCPInspectorFunctionCallRef> DumpFunctionCalls(const FString& AssetPath);

	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Extract", meta = (AICallable))
	static TArray<FMCPInspectorVariableUsageRef> DumpVariableUsages(const FString& AssetPath);

	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Extract", meta = (AICallable))
	static TArray<FMCPInspectorEventEntry> DumpEventEntries(const FString& AssetPath);

	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Extract", meta = (AICallable))
	static TArray<FMCPInspectorFunctionSignature> DumpFunctionSignatures(const FString& AssetPath);

	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Extract", meta = (AICallable))
	static FMCPInspectorAnimBPSettings GetAnimBlueprintSettings(const FString& AssetPath);

	// ─── Duplicate + Remap (Phase 6) ─────────────────────────────────────────

	/**
	 * 소스 Blueprint 를 복제하고 모든 자산 참조를 AssetSubstitutions 맵 따라 일괄 치환.
	 *
	 * 동작:
	 *   1. 소스 BP 로드 → DestAssetPath 위치에 DuplicateLoadedAsset
	 *   2. 새 BP 의 GeneratedClass CDO + SCS 컴포넌트 템플릿 + 모든 그래프 노드 +
	 *      핀 DefaultObject / DefaultValue 를 reflection 으로 walk
	 *   3. AssetSubstitutions 의 (old path → new path) 따라 ObjectProperty / SoftObjectProperty /
	 *      ClassProperty / 핀 DefaultObject / 핀 DefaultValue 치환
	 *   4. NewSkeleton 이 비어있지 않고 BP 가 UAnimBlueprint 면 TargetSkeleton 변경
	 *   5. 컴파일 + 저장
	 *
	 * @param SourceAssetPath  소스 Blueprint 의 asset path (예: "/Game/.../F_TaoistAnimBP.F_TaoistAnimBP")
	 * @param DestAssetPath    생성할 대상 BP 의 asset path (이미 존재하면 실패)
	 * @param NewSkeleton      AnimBP 인 경우 TargetSkeleton 변경 (빈 문자열이면 원본 스켈레톤 유지)
	 * @param AssetSubstitutions 자산 치환 맵 (key=old path, value=new path).
	 *                            path 는 "/Game/.../Asset.Asset" 또는 "/Game/.../Asset" (확장 자동).
	 * @return 성공 시 새 BP 의 asset path. 실패 시 빈 문자열.
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Mutate", meta = (AICallable))
	static FString DuplicateBlueprintWithRemap(
		const FString& SourceAssetPath,
		const FString& DestAssetPath,
		const FString& NewSkeleton,
		const TMap<FString, FString>& AssetSubstitutions);

	// ─── Material 조회 (read-only) ────────────────────────────────────────



	/** 머티리얼 최종속성(BaseColor/Metallic/Normal/...)이 어느 expression에 연결됐는지. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Material", meta = (AICallable))
	static TArray<FMCPInspectorMaterialPropertyLink> DumpMaterialPropertyConnections(const FString& AssetPath);

	/** 주어진 MaterialParameterCollection 을 CollectionParameter 노드로 읽는 UMaterial 경로 목록(역참조). */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Material", meta = (AICallable))
	static TArray<FString> FindMaterialsUsingCollection(const FString& CollectionPath);

	/** Version-qualified material-function input/output GUID contract as JSON. Read-only. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|UDS97", meta = (AICallable))
	static FString DumpMaterialFunctionContractJson(const FString& AssetPath);

	/** Material/function call-site GUID and connection contract as JSON. Read-only. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|UDS97", meta = (AICallable))
	static FString DumpMaterialFunctionCallerContractJson(const FString& CallerPath, const TArray<FString>& FunctionPaths);

	/** Full MPC GUID/name/type/default/order contract as JSON. Read-only. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|UDS97", meta = (AICallable))
	static FString DumpMPCContractJson(const FString& CollectionPath);

	/** Exact material/function CollectionParameter bindings, including serialized ParameterId. Read-only. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|UDS97", meta = (AICallable))
	static FString DumpMaterialMPCBindingsJson(const FString& AssetPath, const FString& CollectionPath);

	/** Exact Blueprint MPC calls bound to CollectionPath, including node/pin identity. Read-only. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|UDS97", meta = (AICallable))
	static FString DumpBlueprintMPCBindingsJson(const FString& BlueprintPath, const FString& CollectionPath);

	/** Checked, additive MPC merge. Existing target entries are immutable; source-only GUIDs append. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|UDS97", meta = (AICallable))
	static FString MergeMaterialParameterCollectionAdditiveChecked(
		const FString& TargetPath,
		const FString& SourcePath,
		const FString& ExpectedTargetContractJson);

	/** Checked literal ParameterName remap on one positively identified MPC call node. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|UDS97", meta = (AICallable))
	static FString RemapMPCBlueprintCallChecked(
		const FString& BlueprintPath,
		const FString& GraphName,
		const FString& NodeGuid,
		const FString& CollectionPath,
		const FString& ExpectedSourceName,
		const FString& RetainedTargetName,
		const FString& ExpectedSharedGuid);

	/** Checked Blueprint parent replacement, compile, save, and readback. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|UDS97", meta = (AICallable))
	static FString ReparentBlueprintChecked(
		const FString& BlueprintPath,
		const FString& ExpectedOldParent,
		const FString& NewParent);

	/** Restore one material-function call from a complete checked JSON connection specification. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|UDS97", meta = (AICallable))
	static FString ReconnectMaterialFunctionCallChecked(
		const FString& CallerPath,
		const FString& NodeId,
		const FString& ExpectedFunctionPath,
		const FString& ExpectedFunctionContractJson,
		const FString& ReconnectSpecJson);

	// ─── Niagara 조회 (read-only) ─────────────────────────────────────────


	/** 지정 이미터 GraphSource의 FunctionCall 이름, 스크립트 경로, 노출 입력을 읽기 전용으로 반환한다. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Niagara", meta = (AICallable))
	static TArray<FString> InspectNiagaraFunctionCalls(const FString& SystemAssetPath, const FString& EmitterName);





	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Niagara", meta = (AICallable))
	static FString InspectNiagaraModuleGraph(const FString& ModuleScriptPath);



	// ─── Material 저작 (mutate) ───────────────────────────────────────────
	// 저장은 호출자 (EditorAssetLibrary.save_asset / p4 edit 선행).

	/**
	 * 머티리얼 결과(Root) 노드 위치 설정 — UMaterial::EditorX/EditorY.
	 * 결과 노드는 UMaterialExpression 이 아니라 UMaterial 에 위치가 저장되며(EditorX/Y 는 플래그 없는
	 * UPROPERTY 라 Python set_editor_property 로 접근 불가), cpp 에서만 세팅 가능.
	 * layout_material_expressions 가 익스프레션을 음수 X 로 배치하므로, 루트를 익스프레션 우측
	 * (예: MakeMaterialAttributes 바로 오른쪽)에 붙이는 데 사용. 베이스 UMaterial 만 대상.
	 * @return 성공 여부 (베이스 UMaterial 아니면 false)
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Material", meta = (AICallable))
	static bool SetMaterialRootNodePosition(const FString& AssetPath, int32 PosX, int32 PosY);

	/**
	 * 머티리얼의 Custom Data 0/1 핀에 상수를 연결한다. 베이스 UMaterial 만 대상.
	 * Python 은 MaterialProperty 에 MP_CustomData0/1 을 노출하지 않아 이 핀에 접근할 수 없다.
	 * 주의: 핀이 비활성인 셰이딩 모델(Material.cpp 의 IsPropertyActive)에서는 연결해도 무시되고
	 * 하드 디폴트가 컴파일된다. 셰이딩 모델이 활성 목록에 있는지 먼저 확인할 것.
	 * @return 성공 여부 (베이스 UMaterial 아니거나 인덱스가 0/1 이 아니면 false)
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Material", meta = (AICallable))
	static bool SetMaterialCustomDataConstant(const FString& AssetPath, int32 CustomDataIndex, float Value);

	/**
	 * 머티리얼의 Custom Data 0/1 핀 상태를 문자열로 반환 (읽기 전용).
	 * 형식: "UseConstant=<0|1> Constant=<f> Expression=<노드명|None>"
	 * ★UseConstant 는 Expression 보다 우선한다(MaterialShared.cpp FScalarMaterialInput::CompileWithDefault).
	 *   핀이 비활성인 셰이딩 모델에서는 이 값이 무시되지만, 핀을 활성화하는 순간 되살아난다.
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Material", meta = (AICallable))
	static FString GetMaterialCustomDataInfo(const FString& AssetPath, int32 CustomDataIndex);

	// ─── AnimAsset 조회 (read-only) ──────────────────────────────────────

	/** 몽타주 구조 덤프: 섹션/슬롯트랙/세그먼트/블렌드/SyncGroup. 몽타주가 아니면 빈 값. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|AnimAsset", meta = (AICallable))
	static FMCPInspectorMontageInfo DumpMontageStructure(const FString& AssetPath);

	/**
	 * Sequence/Montage 공용 노티파이 상세 덤프.
	 * 엔진 Python 미노출 필드(TriggerTime/Duration/TrackIndex) + 인스턴스 노티파이의
	 * EditAnywhere 프로퍼티를 key=value 로 직렬화 (커스텀 노티파이 클래스 대응).
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|AnimAsset", meta = (AICallable))
	static TArray<FMCPInspectorNotifyInfo> DumpNotifyDetails(const FString& AssetPath);

	/** 블렌드스페이스 구조 덤프: 축/샘플(좌표·RateScale·유효성)/보간·루프 설정. 1D/2D/AimOffset 공용. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|AnimAsset", meta = (AICallable))
	static FMCPInspectorBlendSpaceInfo DumpBlendSpaceStructure(const FString& AssetPath);

	/** 스켈레톤 구조 덤프: 슬롯그룹/가상본/소켓/커브 이름/호환 스켈레톤/프리뷰 메시. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|AnimAsset", meta = (AICallable))
	static FMCPInspectorSkeletonInfo DumpSkeletonStructure(const FString& AssetPath);

	/** AnimBP 의 LinkedAnimGraph / LinkedAnimLayer 노드 덤프 (인터페이스/대상 클래스/레이어 바인딩). */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|AnimAsset", meta = (AICallable))
	static TArray<FMCPInspectorLinkedLayerInfo> DumpLinkedAnimLayerUsage(const FString& AssetPath);

	// ─── Montage 저작 (mutate) ───────────────────────────────────────────
	// 전 함수: 수정 + dirty 마크까지. 저장은 호출자 (EditorAssetLibrary.save_asset / p4 edit 선행).

	/**
	 * 시퀀스 1개로 몽타주 에셋 생성 (슬롯트랙 1개 + 전체 구간 세그먼트 + 시작 섹션).
	 * DestAssetPath 가 이미 존재하면 실패. SlotName 빈 = "DefaultSlot", FirstSectionName 빈 = "Default".
	 * @return 생성된 몽타주 asset path (실패 시 빈 문자열)
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|AnimAsset", meta = (AICallable))
	static FString CreateMontageFromSequence(const FString& SequencePath, const FString& DestAssetPath, const FString& SlotName, const FString& FirstSectionName);



	/** 섹션 링크 체인 설정. NextSectionName == 자기 자신 = 루프, 빈 문자열 = 체인 끊기. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|AnimAsset", meta = (AICallable))
	static bool SetMontageSectionNext(const FString& AssetPath, const FString& SectionName, const FString& NextSectionName);

	/** 슬롯트랙 추가. @return 새 트랙 인덱스 (-1 실패) */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|AnimAsset", meta = (AICallable))
	static int32 AddMontageSlotTrack(const FString& AssetPath, const FString& SlotName);

	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|AnimAsset", meta = (AICallable))
	static bool SetMontageSlotTrackName(const FString& AssetPath, int32 TrackIndex, const FString& SlotName);


	// ─── BlendSpace 저작 (mutate) ────────────────────────────────────────
	// 전 함수가 마지막에 ValidateSampleData + ResampleData 수행 — SampleData 직접 조작 금지의 대체 경로.

	/** 샘플 추가. 1D 는 Y 무시. @return 샘플 인덱스 (-1 실패) */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|AnimAsset", meta = (AICallable))
	static int32 AddBlendSpaceSample(const FString& AssetPath, const FString& SequencePath, float X, float Y);

	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|AnimAsset", meta = (AICallable))
	static bool MoveBlendSpaceSample(const FString& AssetPath, int32 SampleIndex, float X, float Y);

	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|AnimAsset", meta = (AICallable))
	static bool ReplaceBlendSpaceSampleAnimation(const FString& AssetPath, int32 SampleIndex, const FString& SequencePath);

	/** 샘플 삭제. 이후 SampleIndex 재배열 — 재덤프 필요. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|AnimAsset", meta = (AICallable))
	static bool RemoveBlendSpaceSample(const FString& AssetPath, int32 SampleIndex);

	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|AnimAsset", meta = (AICallable))
	static bool SetBlendSpaceSampleRateScale(const FString& AssetPath, int32 SampleIndex, float RateScale);

	/** 축 파라미터 설정. AxisIndex: 0=가로, 1=세로(2D 전용). 설정 후 Resample. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|AnimAsset", meta = (AICallable))
	static bool SetBlendSpaceAxis(const FString& AssetPath, int32 AxisIndex, const FString& AxisName, float Min, float Max, int32 GridNum, bool bSnapToGrid, bool bWrapInput);

	// ─── Notify 보완 (mutate) ────────────────────────────────────────────
	// 추가/삭제는 unreal.AnimationLibrary 사용 (재구현 금지). 아래는 엔진 미노출 갭만.

	/**
	 * 기존 노티파이의 시간/듀레이션 in-place 수정.
	 * (GetAnimationNotifyEvents 는 복사본을 반환해 Python 으로는 수정 불가한 갭)
	 * NewDuration < 0 = 듀레이션 유지. NotifyState 가 아니면 듀레이션 무시.
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|AnimAsset", meta = (AICallable))
	static bool SetNotifyEventTiming(const FString& AssetPath, int32 NotifyIndex, float NewTriggerTime, float NewDuration);

	// ─── AnimGraph 노드 프로퍼티 (BP 그래프 도메인 — 인덱스 함수군과 동일 규약) ──

	/** 노드의 UPROPERTY 목록을 "경로=타입=값" 줄로 반환. FAnimNode(Node) struct 는 1단계 재귀 ("Node.Sequence" 등). */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static TArray<FString> ListAnimGraphNodeProperties(const FString& AssetPath, int32 GraphIndex, int32 NodeIndex);

	/** PropertyPath("Node.Sequence", "Node.LayerSetup" 등) 값을 ExportText 문자열로 반환. */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static FString GetAnimGraphNodePropertyByIndex(const FString& AssetPath, int32 GraphIndex, int32 NodeIndex, const FString& PropertyPath);

	/**
	 * PropertyPath 에 ImportText 로 값 기록 — 기존 노드의 시퀀스 교체("Node.Sequence"),
	 * LayeredBlendPerBone 필터("Node.LayerSetup"), 싱크그룹("Node.GroupName"), 슬롯("Node.SlotName") 등 범용.
	 * 핀 구성이 변하는 프로퍼티는 변경 후 ReconstructNodeByGraphIndex 호출 권장.
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector", meta = (AICallable))
	static bool SetAnimGraphNodePropertyByIndex(const FString& AssetPath, int32 GraphIndex, int32 NodeIndex, const FString& PropertyPath, const FString& ValueText);

	// ---- BEGIN functions forwarding to unreal-mcp-bridge methods (generated from the bridge tool layer; BlueprintInspector has no dependency on that plugin) ----


































































	// ---- END forwarding functions ----
};
