// BlueprintInspector — AnimAsset 카테고리 구현 (같은 UMCPInspectorLibrary 의 멤버를 별도 TU 에서 정의)
// 설계: docs/AnimNiagara_Extension_DESIGN.md §3, 부록 A
// 원칙: 엔진이 이미 Python 노출한 것 재구현 금지 — 여기 있는 것은 전부 C++ 전용 갭의 브릿지.
// mutate 는 수정 + dirty 마크까지. 저장은 호출자 (기존 규약).

#include "MCPInspectorLibrary.h"
#include "BPInspectorCommon.h"
#include "Runtime/Launch/Resources/Version.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimCompositeBase.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimLinkableElement.h"
#include "Animation/AnimTypes.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "AlphaBlend.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EditorAssetLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

// ─── 로컬 헬퍼 ──────────────────────────────────────────────────────────────

namespace
{
	FString BPIA_AlphaBlendOptionToStr(EAlphaBlendOption Opt)
	{
		const UEnum* E = StaticEnum<EAlphaBlendOption>();
		return E ? E->GetNameStringByValue((int64)Opt) : FString::FromInt((int32)Opt);
	}

	UAnimMontage* BPIA_LoadMontage(const FString& AssetPath)
	{
		UAnimMontage* M = BPICommon::LoadTypedAsset<UAnimMontage>(AssetPath);
		if (!M)
		{
			UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: AnimMontage 로드 실패: %s"), *AssetPath);
		}
		return M;
	}

	UBlendSpace* BPIA_LoadBlendSpace(const FString& AssetPath)
	{
		// UBlendSpace1D / UAimOffsetBlendSpace 모두 UBlendSpace 파생
		UBlendSpace* BS = BPICommon::LoadTypedAsset<UBlendSpace>(AssetPath);
		if (!BS)
		{
			UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: BlendSpace 로드 실패: %s"), *AssetPath);
		}
		return BS;
	}

	// BlendSpace mutate 공통 후처리 — 삼각분할/그리드 재계산 (이걸 건너뛰면 에셋 손상)
	void BPIA_PostBlendSpaceEdit(UBlendSpace* BS)
	{
		BS->ValidateSampleData();
		BS->ResampleData();
		BPICommon::MarkAssetDirty(BS);
	}

	// 몽타주 세그먼트 변경 공통 후처리 — 길이 재계산 + 링크 요소 갱신 + 노티파이 트랙 캐시 갱신
	void BPIA_PostMontageSegmentEdit(UAnimMontage* M)
	{
		M->SetCompositeLength(M->CalculateSequenceLength());
		M->UpdateLinkableElements();
		M->RefreshCacheData();
		BPICommon::MarkAssetDirty(M);
	}
}

// ─── AnimAsset 조회 (read-only) ─────────────────────────────────────────────

FMCPInspectorMontageInfo UMCPInspectorLibrary::DumpMontageStructure(const FString& AssetPath)
{
	FMCPInspectorMontageInfo Info;
	Info.AssetPath = AssetPath;

	UAnimMontage* M = BPIA_LoadMontage(AssetPath);
	if (!M)
	{
		return Info;
	}

	Info.SkeletonPath = M->GetSkeleton() ? M->GetSkeleton()->GetPathName() : FString();
	Info.PlayLength = M->GetPlayLength();
	Info.BlendInTime = M->BlendIn.GetBlendTime();
	Info.BlendInOption = BPIA_AlphaBlendOptionToStr(M->BlendIn.GetBlendOption());
	Info.BlendOutTime = M->BlendOut.GetBlendTime();
	Info.BlendOutOption = BPIA_AlphaBlendOptionToStr(M->BlendOut.GetBlendOption());
	Info.BlendOutTriggerTime = M->BlendOutTriggerTime;
	Info.SyncGroup = M->SyncGroup.ToString();
	Info.SyncSlotIndex = M->SyncSlotIndex;
	Info.NotifyCount = M->Notifies.Num();

	for (const FCompositeSection& Section : M->CompositeSections)
	{
		FMCPInspectorMontageSectionInfo S;
		S.SectionName = Section.SectionName.ToString();
		S.NextSectionName = (Section.NextSectionName != NAME_None) ? Section.NextSectionName.ToString() : FString();
		S.StartTime = Section.GetTime();
		Info.Sections.Add(S);
	}

	for (const FSlotAnimationTrack& Track : M->SlotAnimTracks)
	{
		FMCPInspectorMontageSlotTrackInfo T;
		T.SlotName = Track.SlotName.ToString();

		for (const FAnimSegment& Seg : Track.AnimTrack.AnimSegments)
		{
			FMCPInspectorMontageSegmentInfo G;
			const UAnimSequenceBase* Ref = Seg.GetAnimReference();
			G.AnimPath = Ref ? Ref->GetPathName() : FString();
			G.StartPos = Seg.StartPos;
			G.AnimStartTime = Seg.AnimStartTime;
			G.AnimEndTime = Seg.AnimEndTime;
			G.PlayRate = Seg.AnimPlayRate;
			G.LoopingCount = Seg.LoopingCount;
			T.Segments.Add(G);
		}
		Info.SlotTracks.Add(T);
	}

	if (const UEnum* BlendModeEnum = StaticEnum<EMontageBlendMode>())
	{
		Info.ExtraKVPairs.Add(FString::Printf(TEXT("BlendModeIn=%s"), *BlendModeEnum->GetNameStringByValue((int64)M->BlendModeIn)));
		Info.ExtraKVPairs.Add(FString::Printf(TEXT("BlendModeOut=%s"), *BlendModeEnum->GetNameStringByValue((int64)M->BlendModeOut)));
	}

	return Info;
}

TArray<FMCPInspectorNotifyInfo> UMCPInspectorLibrary::DumpNotifyDetails(const FString& AssetPath)
{
	TArray<FMCPInspectorNotifyInfo> Result;

	UAnimSequenceBase* Seq = BPICommon::LoadTypedAsset<UAnimSequenceBase>(AssetPath);
	if (!Seq)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: AnimSequenceBase 로드 실패: %s"), *AssetPath);
		return Result;
	}

	const UAnimMontage* AsMontage = Cast<UAnimMontage>(Seq);

	for (int32 Idx = 0; Idx < Seq->Notifies.Num(); ++Idx)
	{
		const FAnimNotifyEvent& Event = Seq->Notifies[Idx];

		FMCPInspectorNotifyInfo N;
		N.NotifyIndex = Idx;
		N.NotifyName = Event.NotifyName.ToString();
		N.TriggerTime = Event.GetTriggerTime();
		N.TrackIndex = Event.TrackIndex;

		const UObject* Instance = nullptr;
		if (Event.NotifyStateClass)
		{
			N.NotifyKind = TEXT("NotifyState");
			N.Duration = Event.GetDuration();
			Instance = Event.NotifyStateClass;
		}
		else if (Event.Notify)
		{
			N.NotifyKind = TEXT("Notify");
			Instance = Event.Notify;
		}
		else
		{
			N.NotifyKind = TEXT("NameOnly"); // 스켈레톤 노티파이 (오브젝트 없음)
		}

		if (Instance)
		{
			N.NotifyClassPath = Instance->GetClass()->GetPathName();
			BPICommon::DumpObjectEditProperties(Instance, N.PropKVPairs);
		}

		if (AsMontage && AsMontage->SlotAnimTracks.IsValidIndex(Event.GetSlotIndex()))
		{
			N.LinkedSlot = AsMontage->SlotAnimTracks[Event.GetSlotIndex()].SlotName.ToString();
		}

		Result.Add(N);
	}

	return Result;
}

FMCPInspectorBlendSpaceInfo UMCPInspectorLibrary::DumpBlendSpaceStructure(const FString& AssetPath)
{
	FMCPInspectorBlendSpaceInfo Info;
	Info.AssetPath = AssetPath;

	UBlendSpace* BS = BPIA_LoadBlendSpace(AssetPath);
	if (!BS)
	{
		return Info;
	}

	Info.BlendSpaceClass = BS->GetClass()->GetName();
	Info.SkeletonPath = BS->GetSkeleton() ? BS->GetSkeleton()->GetPathName() : FString();

	// bLoop / NotifyTriggerMode / 보간 설정은 protected UPROPERTY → 리플렉션으로 회수
	Info.bLoop = BPICommon::ExportPropertyByPath(BS, TEXT("bLoop")).Equals(TEXT("True"), ESearchCase::IgnoreCase);
	Info.NotifyTriggerMode = BPICommon::ExportPropertyByPath(BS, TEXT("NotifyTriggerMode"));
	Info.ExtraKVPairs.Add(TEXT("TargetWeightInterpolationSpeedPerSec=") + BPICommon::ExportPropertyByPath(BS, TEXT("TargetWeightInterpolationSpeedPerSec")));
	Info.ExtraKVPairs.Add(TEXT("bAllowMeshSpaceBlending=") + BPICommon::ExportPropertyByPath(BS, TEXT("bAllowMeshSpaceBlending")));

	const bool bIs1D = BS->IsA<UBlendSpace1D>();
	const int32 AxisCount = bIs1D ? 1 : 2;
	for (int32 AxisIdx = 0; AxisIdx < AxisCount; ++AxisIdx)
	{
		const FBlendParameter& Param = BS->GetBlendParameter(AxisIdx);
		FMCPInspectorBlendAxisInfo A;
		A.AxisName = Param.DisplayName;
		A.Min = Param.Min;
		A.Max = Param.Max;
		A.GridNum = Param.GridNum;
		A.bSnapToGrid = Param.bSnapToGrid;
		A.bWrapInput = Param.bWrapInput;
		Info.Axes.Add(A);
	}

	const TArray<FBlendSample>& Samples = BS->GetBlendSamples();
	for (int32 SampleIdx = 0; SampleIdx < Samples.Num(); ++SampleIdx)
	{
		const FBlendSample& Sample = Samples[SampleIdx];
		FMCPInspectorBlendSampleInfo S;
		S.SampleIndex = SampleIdx;
		S.AnimPath = Sample.Animation ? Sample.Animation->GetPathName() : FString();
		S.X = (float)Sample.SampleValue.X;
		S.Y = (float)Sample.SampleValue.Y;
		S.RateScale = Sample.RateScale;
		S.bValid = (Sample.Animation != nullptr);
		Info.Samples.Add(S);
	}

	return Info;
}

FMCPInspectorSkeletonInfo UMCPInspectorLibrary::DumpSkeletonStructure(const FString& AssetPath)
{
	FMCPInspectorSkeletonInfo Info;
	Info.AssetPath = AssetPath;

	USkeleton* S = BPICommon::LoadTypedAsset<USkeleton>(AssetPath);
	if (!S)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: Skeleton 로드 실패: %s"), *AssetPath);
		return Info;
	}

	Info.BoneCount = S->GetReferenceSkeleton().GetNum();

	for (const FAnimSlotGroup& Group : S->GetSlotGroups())
	{
		FMCPInspectorSkeletonSlotGroupInfo G;
		G.GroupName = Group.GroupName.ToString();
		for (const FName& SlotName : Group.SlotNames)
		{
			G.SlotNames.Add(SlotName.ToString());
		}
		Info.SlotGroups.Add(G);
	}

	for (const FVirtualBone& VB : S->GetVirtualBones())
	{
		FMCPInspectorVirtualBoneInfo V;
		V.VirtualBoneName = VB.VirtualBoneName.ToString();
		V.SourceBone = VB.SourceBoneName.ToString();
		V.TargetBone = VB.TargetBoneName.ToString();
		Info.VirtualBones.Add(V);
	}

	for (const USkeletalMeshSocket* Socket : S->Sockets)
	{
		if (!Socket) continue;
		FMCPInspectorSocketInfo K;
		K.SocketName = Socket->SocketName.ToString();
		K.BoneName = Socket->BoneName.ToString();
		K.RelativeLocation = Socket->RelativeLocation;
		K.RelativeRotation = Socket->RelativeRotation;
		K.RelativeScale = Socket->RelativeScale;
		Info.Sockets.Add(K);
	}

	TArray<FName> CurveNames;
	S->GetCurveMetaDataNames(CurveNames);
	for (const FName& CurveName : CurveNames)
	{
		Info.CurveNames.Add(CurveName.ToString());
	}

	for (const TSoftObjectPtr<USkeleton>& Compat : S->GetCompatibleSkeletons())
	{
		Info.CompatibleSkeletons.Add(Compat.ToString());
	}

	if (const USkeletalMesh* PreviewMesh = S->GetPreviewMesh())
	{
		Info.PreviewMesh = PreviewMesh->GetPathName();
	}

	return Info;
}

TArray<FMCPInspectorLinkedLayerInfo> UMCPInspectorLibrary::DumpLinkedAnimLayerUsage(const FString& AssetPath)
{
	TArray<FMCPInspectorLinkedLayerInfo> Result;

	UBlueprint* BP = BPICommon::LoadBlueprintFromPath(AssetPath);
	if (!BP)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: Failed to load blueprint at %s"), *AssetPath);
		return Result;
	}

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	for (const UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;

			const FString NodeClassName = Node->GetClass()->GetName();
			const bool bLinkedGraph = NodeClassName == TEXT("AnimGraphNode_LinkedAnimGraph");
			const bool bLinkedLayer = NodeClassName == TEXT("AnimGraphNode_LinkedAnimLayer");
			if (!bLinkedGraph && !bLinkedLayer) continue;

			FMCPInspectorLinkedLayerInfo L;
			L.NodeClass = NodeClassName;
			L.GraphName = Graph->GetName();
			L.NodeName = Node->GetName();
			// AnimGraph 모듈 헤더 include 없이 리플렉션으로 회수 (클래스 미노출 대비)
			L.InstanceClassPath = BPICommon::ExportPropertyByPath(Node, TEXT("Node.InstanceClass"));
			if (bLinkedLayer)
			{
				L.InterfacePath = BPICommon::ExportPropertyByPath(Node, TEXT("Node.Interface"));
				L.LayerName = BPICommon::ExportPropertyByPath(Node, TEXT("Node.Layer"));
			}

			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input)
				{
					L.InputPoseNames.Add(Pin->PinName.ToString());
				}
			}

			Result.Add(L);
		}
	}

	return Result;
}

// ─── Montage 저작 (mutate) ──────────────────────────────────────────────────

FString UMCPInspectorLibrary::CreateMontageFromSequence(const FString& SequencePath, const FString& DestAssetPath, const FString& SlotName, const FString& FirstSectionName)
{
	UAnimSequence* Seq = BPICommon::LoadTypedAsset<UAnimSequence>(SequencePath);
	if (!Seq || !Seq->GetSkeleton())
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: 시퀀스 로드 실패 또는 스켈레톤 없음: %s"), *SequencePath);
		return FString();
	}

	// "/Game/.../Foo.Foo" → "/Game/.../Foo"
	FString PackageName = DestAssetPath;
	int32 DotIdx;
	if (PackageName.FindChar(TEXT('.'), DotIdx))
	{
		PackageName = PackageName.Left(DotIdx);
	}

	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: 잘못된 패키지 경로: %s"), *PackageName);
		return FString();
	}
	if (UEditorAssetLibrary::DoesAssetExist(PackageName))
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: 대상이 이미 존재 (덮어쓰기 안 함): %s"), *PackageName);
		return FString();
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return FString();
	}
	Package->FullyLoad();

	UAnimMontage* M = NewObject<UAnimMontage>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (!M)
	{
		return FString();
	}

	M->SetSkeleton(Seq->GetSkeleton());

	// 슬롯트랙 + 전체 구간 세그먼트 (UAnimMontageFactory::FactoryCreateNew 와 동일 구성)
	const FName SlotFName = SlotName.IsEmpty() ? FAnimSlotGroup::DefaultSlotName : FName(*SlotName);
	FSlotAnimationTrack& Track = M->AddSlot(SlotFName);

	FAnimSegment Seg;
	Seg.SetAnimReference(Seq, /*bInitialize*/ true);
	Seg.StartPos = 0.0f;
	Seg.AnimStartTime = 0.0f;
	Seg.AnimEndTime = Seq->GetPlayLength();
	Seg.AnimPlayRate = 1.0f;
	Seg.LoopingCount = 1;
	Track.AnimTrack.AnimSegments.Add(Seg);

	M->SetCompositeLength(M->CalculateSequenceLength());

	const FName SectionFName = FirstSectionName.IsEmpty() ? FName(TEXT("Default")) : FName(*FirstSectionName);
	M->AddAnimCompositeSection(SectionFName, 0.0f);

	M->UpdateLinkableElements();
	M->RefreshCacheData();

	FAssetRegistryModule::AssetCreated(M);
	Package->MarkPackageDirty();

	UE_LOG(LogTemp, Log, TEXT("BlueprintInspector: 몽타주 생성 → %s (slot=%s, section=%s)"), *M->GetPathName(), *SlotFName.ToString(), *SectionFName.ToString());
	return M->GetPathName();
}



bool UMCPInspectorLibrary::SetMontageSectionNext(const FString& AssetPath, const FString& SectionName, const FString& NextSectionName)
{
	UAnimMontage* M = BPIA_LoadMontage(AssetPath);
	if (!M)
	{
		return false;
	}

	const int32 SectionIdx = M->GetSectionIndex(FName(*SectionName));
	if (SectionIdx == INDEX_NONE)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: 섹션 없음: %s"), *SectionName);
		return false;
	}

	if (!NextSectionName.IsEmpty() && M->GetSectionIndex(FName(*NextSectionName)) == INDEX_NONE)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: 다음 섹션 없음: %s"), *NextSectionName);
		return false;
	}

	M->Modify();
	M->CompositeSections[SectionIdx].NextSectionName = NextSectionName.IsEmpty() ? NAME_None : FName(*NextSectionName);
	BPICommon::MarkAssetDirty(M);
	return true;
}

int32 UMCPInspectorLibrary::AddMontageSlotTrack(const FString& AssetPath, const FString& SlotName)
{
	UAnimMontage* M = BPIA_LoadMontage(AssetPath);
	if (!M || SlotName.IsEmpty())
	{
		return -1;
	}

	M->Modify();
	M->AddSlot(FName(*SlotName));
	BPICommon::MarkAssetDirty(M);
	return M->SlotAnimTracks.Num() - 1;
}

bool UMCPInspectorLibrary::SetMontageSlotTrackName(const FString& AssetPath, int32 TrackIndex, const FString& SlotName)
{
	UAnimMontage* M = BPIA_LoadMontage(AssetPath);
	if (!M || SlotName.IsEmpty() || !M->SlotAnimTracks.IsValidIndex(TrackIndex))
	{
		return false;
	}

	M->Modify();
	M->SlotAnimTracks[TrackIndex].SlotName = FName(*SlotName);
	BPICommon::MarkAssetDirty(M);
	return true;
}


// ─── BlendSpace 저작 (mutate) ───────────────────────────────────────────────

int32 UMCPInspectorLibrary::AddBlendSpaceSample(const FString& AssetPath, const FString& SequencePath, float X, float Y)
{
	UBlendSpace* BS = BPIA_LoadBlendSpace(AssetPath);
	if (!BS)
	{
		return -1;
	}

	UAnimSequence* Seq = BPICommon::LoadTypedAsset<UAnimSequence>(SequencePath);
	if (!Seq)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: 시퀀스 로드 실패: %s"), *SequencePath);
		return -1;
	}

	const bool bIs1D = BS->IsA<UBlendSpace1D>();
	const FVector SampleValue(X, bIs1D ? 0.0f : Y, 0.0f);

	BS->Modify();
	const int32 NewIdx = BS->AddSample(Seq, SampleValue);
	if (NewIdx == INDEX_NONE)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: 샘플 추가 실패 (%s @ %.2f, %.2f) — additive/스켈레톤 호환 확인"), *SequencePath, X, Y);
		return -1;
	}

	BPIA_PostBlendSpaceEdit(BS);
	return NewIdx;
}

bool UMCPInspectorLibrary::MoveBlendSpaceSample(const FString& AssetPath, int32 SampleIndex, float X, float Y)
{
	UBlendSpace* BS = BPIA_LoadBlendSpace(AssetPath);
	if (!BS || !BS->IsValidBlendSampleIndex(SampleIndex))
	{
		return false;
	}

	const bool bIs1D = BS->IsA<UBlendSpace1D>();

	BS->Modify();
	if (!BS->EditSampleValue(SampleIndex, FVector(X, bIs1D ? 0.0f : Y, 0.0f)))
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: 샘플 이동 실패 (idx=%d) — 중복 위치/범위 확인"), SampleIndex);
		return false;
	}

	BPIA_PostBlendSpaceEdit(BS);
	return true;
}

bool UMCPInspectorLibrary::ReplaceBlendSpaceSampleAnimation(const FString& AssetPath, int32 SampleIndex, const FString& SequencePath)
{
	UBlendSpace* BS = BPIA_LoadBlendSpace(AssetPath);
	if (!BS || !BS->IsValidBlendSampleIndex(SampleIndex))
	{
		return false;
	}

	UAnimSequence* Seq = BPICommon::LoadTypedAsset<UAnimSequence>(SequencePath);
	if (!Seq)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: 시퀀스 로드 실패: %s"), *SequencePath);
		return false;
	}

	BS->Modify();
	if (!BS->ReplaceSampleAnimation(SampleIndex, Seq))
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: 샘플 애니메이션 교체 실패 (idx=%d)"), SampleIndex);
		return false;
	}

	BPIA_PostBlendSpaceEdit(BS);
	return true;
}

bool UMCPInspectorLibrary::RemoveBlendSpaceSample(const FString& AssetPath, int32 SampleIndex)
{
	UBlendSpace* BS = BPIA_LoadBlendSpace(AssetPath);
	if (!BS || !BS->IsValidBlendSampleIndex(SampleIndex))
	{
		return false;
	}

	BS->Modify();
	if (!BS->DeleteSample(SampleIndex))
	{
		return false;
	}

	BPIA_PostBlendSpaceEdit(BS);
	return true;
}

bool UMCPInspectorLibrary::SetBlendSpaceSampleRateScale(const FString& AssetPath, int32 SampleIndex, float RateScale)
{
	UBlendSpace* BS = BPIA_LoadBlendSpace(AssetPath);
	if (!BS || !BS->IsValidBlendSampleIndex(SampleIndex))
	{
		return false;
	}

	BS->Modify();
	// GetBlendSamples 는 SampleData 의 const 참조 — 전용 setter 가 없어 const_cast 로 기록
	// (RateScale 은 삼각분할과 무관하지만 규약대로 Validate 는 수행)
	FBlendSample& Sample = const_cast<FBlendSample&>(BS->GetBlendSample(SampleIndex));
	Sample.RateScale = RateScale;

	BS->ValidateSampleData();
	BPICommon::MarkAssetDirty(BS);
	return true;
}

bool UMCPInspectorLibrary::SetBlendSpaceAxis(const FString& AssetPath, int32 AxisIndex, const FString& AxisName, float Min, float Max, int32 GridNum, bool bSnapToGrid, bool bWrapInput)
{
	UBlendSpace* BS = BPIA_LoadBlendSpace(AssetPath);
	if (!BS)
	{
		return false;
	}

	const bool bIs1D = BS->IsA<UBlendSpace1D>();
	const int32 MaxAxis = bIs1D ? 0 : 1;
	if (AxisIndex < 0 || AxisIndex > MaxAxis)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: AxisIndex %d 범위 밖 (%s 는 0..%d)"), AxisIndex, *BS->GetClass()->GetName(), MaxAxis);
		return false;
	}
	if (Min >= Max || GridNum < 1)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: 잘못된 축 파라미터 (Min=%.2f Max=%.2f GridNum=%d)"), Min, Max, GridNum);
		return false;
	}

	// BlendParameters 는 protected UPROPERTY(C-array) → 리플렉션으로 요소 접근
	FProperty* Prop = FindFProperty<FProperty>(BS->GetClass(), TEXT("BlendParameters"));
	if (!Prop || Prop->ArrayDim <= AxisIndex)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: BlendParameters 프로퍼티 접근 실패"));
		return false;
	}

	BS->Modify();
	FBlendParameter* Param = Prop->ContainerPtrToValuePtr<FBlendParameter>(BS, AxisIndex);
	if (!AxisName.IsEmpty())
	{
		Param->DisplayName = AxisName;
	}
	Param->Min = Min;
	Param->Max = Max;
	Param->GridNum = GridNum;
	Param->bSnapToGrid = bSnapToGrid;
	Param->bWrapInput = bWrapInput;

	BPIA_PostBlendSpaceEdit(BS);
	return true;
}

// ─── Notify 보완 (mutate) ───────────────────────────────────────────────────

bool UMCPInspectorLibrary::SetNotifyEventTiming(const FString& AssetPath, int32 NotifyIndex, float NewTriggerTime, float NewDuration)
{
	UAnimSequenceBase* Seq = BPICommon::LoadTypedAsset<UAnimSequenceBase>(AssetPath);
	if (!Seq)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: AnimSequenceBase 로드 실패: %s"), *AssetPath);
		return false;
	}
	if (!Seq->Notifies.IsValidIndex(NotifyIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: NotifyIndex %d 범위 밖 (num=%d)"), NotifyIndex, Seq->Notifies.Num());
		return false;
	}

	Seq->Modify();
	FAnimNotifyEvent& Event = Seq->Notifies[NotifyIndex];
	Event.SetTime(FMath::Clamp(NewTriggerTime, 0.0f, Seq->GetPlayLength()));
	if (NewDuration >= 0.0f && Event.NotifyStateClass)
	{
		Event.SetDuration(FMath::Max(NewDuration, 1.0f / 30.0f)); // 최소 1프레임
	}

	// RefreshCacheData 가 시간순 재정렬 → NotifyIndex 무효화 가능 (재덤프 필요 — 헤더 주석 참조)
	Seq->RefreshCacheData();
	BPICommon::MarkAssetDirty(Seq);
	return true;
}

// ─── AnimGraph 노드 프로퍼티 (BP 그래프 도메인) ─────────────────────────────

TArray<FString> UMCPInspectorLibrary::ListAnimGraphNodeProperties(const FString& AssetPath, int32 GraphIndex, int32 NodeIndex)
{
	TArray<FString> Result;

	UBlueprint* BP = BPICommon::LoadBlueprintFromPath(AssetPath);
	UEdGraph* Graph = BPICommon::GetGraphByIndex(BP, GraphIndex);
	UEdGraphNode* Node = BPICommon::GetNodeByIndex(Graph, NodeIndex);
	if (!Node)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: 노드 조회 실패 (g=%d n=%d) @ %s"), GraphIndex, NodeIndex, *AssetPath);
		return Result;
	}

	auto ExportOne = [](const FProperty* Prop, const void* ValuePtr, const FString& PathPrefix, TArray<FString>& Out)
	{
		FString Value;
		Prop->ExportText_Direct(Value, ValuePtr, ValuePtr, nullptr, PPF_None);
		if (Value.Len() > 256)
		{
			Value = Value.Left(256) + TEXT("...");
		}
		Out.Add(FString::Printf(TEXT("%s%s=%s=%s"), *PathPrefix, *Prop->GetName(), *Prop->GetClass()->GetName(), *Value));
	};

	for (TFieldIterator<FProperty> It(Node->GetClass()); It; ++It)
	{
		FProperty* Prop = *It;
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Node);

		// FAnimNode_* 를 담는 "Node" struct 는 1단계 내부까지 나열 ("Node.Sequence" 식 경로 발견용)
		const FStructProperty* StructProp = CastField<FStructProperty>(Prop);
		if (StructProp && Prop->GetName() == TEXT("Node"))
		{
			for (TFieldIterator<FProperty> InnerIt(StructProp->Struct); InnerIt; ++InnerIt)
			{
				FProperty* InnerProp = *InnerIt;
				const void* InnerPtr = InnerProp->ContainerPtrToValuePtr<void>(ValuePtr);
				ExportOne(InnerProp, InnerPtr, TEXT("Node."), Result);
			}
		}
		else
		{
			ExportOne(Prop, ValuePtr, TEXT(""), Result);
		}
	}

	return Result;
}

FString UMCPInspectorLibrary::GetAnimGraphNodePropertyByIndex(const FString& AssetPath, int32 GraphIndex, int32 NodeIndex, const FString& PropertyPath)
{
	UBlueprint* BP = BPICommon::LoadBlueprintFromPath(AssetPath);
	UEdGraph* Graph = BPICommon::GetGraphByIndex(BP, GraphIndex);
	UEdGraphNode* Node = BPICommon::GetNodeByIndex(Graph, NodeIndex);
	if (!Node)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: 노드 조회 실패 (g=%d n=%d) @ %s"), GraphIndex, NodeIndex, *AssetPath);
		return FString();
	}

	return BPICommon::ExportPropertyByPath(Node, PropertyPath);
}

bool UMCPInspectorLibrary::SetAnimGraphNodePropertyByIndex(const FString& AssetPath, int32 GraphIndex, int32 NodeIndex, const FString& PropertyPath, const FString& ValueText)
{
	UBlueprint* BP = BPICommon::LoadBlueprintFromPath(AssetPath);
	UEdGraph* Graph = BPICommon::GetGraphByIndex(BP, GraphIndex);
	UEdGraphNode* Node = BPICommon::GetNodeByIndex(Graph, NodeIndex);
	if (!Node)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintInspector: 노드 조회 실패 (g=%d n=%d) @ %s"), GraphIndex, NodeIndex, *AssetPath);
		return false;
	}

	if (!BPICommon::ImportPropertyByPath(Node, PropertyPath, ValueText))
	{
		return false;
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	return true;
}
