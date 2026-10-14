#include "Commands/MontageCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimCompositeBase.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/Skeleton.h"
#include "Commands/InspectorInvoke.h"

namespace
{
    UAnimMontage* LoadMontage(const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        FString Path;
        if (!UnrealMCPBridge::Json::RequireString(Params, TEXT("montage_path"), Path, OutError))
        {
            return nullptr;
        }
        UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, *Path);
        if (!Montage)
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("Could not load montage: %s"), *Path);
        }
        return Montage;
    }

    /**
     * 세그먼트를 건드린 뒤 반드시 부른다.
     *
     * 몽타주 길이는 저장된 값이 아니라 슬롯 세그먼트에서 **파생**되는데, 배열에 넣는 것만으로는 다시
     * 계산되지 않는다. 빠뜨리면 애셋 길이가 첫 세그먼트 그대로 남아 뒤에 붙인 구간이 재생 중 잘린다 —
     * 에디터에서 열어 보기 전까지는 성공한 것처럼 보인다.
     *
     * UpdateLinkableElements 는 그 다음이다: 섹션과 노티파이는 절대 시각이 아니라 세그먼트에 링크된
     * 상대 위치로 저장돼 있어서, 세그먼트가 움직이면 따라와야 한다.
     */
    void RebuildMontage(UAnimMontage* Montage)
    {
        Montage->SetCompositeLength(Montage->CalculateSequenceLength());
        Montage->UpdateLinkableElements();

        // **섹션을 직접 정렬한다.** AddAnimCompositeSection 도 PostEditChange 도 정렬해 주지 않는다 —
        // 실제로 확인했다: 9.8초 섹션 뒤에 7.1초 섹션을 넣었더니 배열 순서가 넣은 순서 그대로 남고,
        // GetSectionStartAndEndTime 이 "start 9.8, end 7.1" 을 돌려줬다. 구간 판정(GetSectionIndexFromPosition,
        // GetAnimCompositeSectionIndexFromPos)이 전부 시각 오름차순을 전제하므로 이대로 두면 재생이 어긋난다.
        //
        // 엔진의 SortAnimCompositeSectionByPos 는 private 이지만 CompositeSections 자체는 public 이다
        // (Python 이 이걸 protected 라고 하는 것은 스크립트 접근 권한 얘기지 C++ 접근이 아니다).
        // 링크는 이름으로 걸려 있어 자리를 옮겨도 그대로 유효하다.
        Montage->CompositeSections.Sort([](const FCompositeSection& A, const FCompositeSection& B)
        {
            return A.GetTime() < B.GetTime();
        });

        Montage->RefreshCacheData();
        // 에디터가 애셋을 손댄 뒤 하는 것과 같은 마무리.
        Montage->PostEditChange();
        Montage->MarkPackageDirty();
    }

    FSlotAnimationTrack* FindSlot(UAnimMontage* Montage, FName SlotName)
    {
        for (FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
        {
            if (Track.SlotName == SlotName)
            {
                return &Track;
            }
        }
        return nullptr;
    }

    /** 슬롯 이름을 읽는다. 없으면 DefaultSlot — 이 프로젝트 몽타주가 전부 쓰는 슬롯이다. */
    FName ReadSlotName(const TSharedPtr<FJsonObject>& Params)
    {
        FString SlotName;
        if (Params->TryGetStringField(TEXT("slot"), SlotName) && !SlotName.IsEmpty())
        {
            return FName(*SlotName);
        }
        return FName(TEXT("DefaultSlot"));
    }

    /** 세그먼트가 채우고 있는 구간의 끝 = 그 슬롯에 이어붙일 위치. */
    float SlotEndTime(const FSlotAnimationTrack& Track)
    {
        float End = 0.f;
        for (const FAnimSegment& Segment : Track.AnimTrack.AnimSegments)
        {
            End = FMath::Max(End, Segment.StartPos + Segment.GetLength());
        }
        return End;
    }

    /** "section"(이름) 또는 "index"로 섹션 하나를 고른다. 실패하면 INDEX_NONE + OutError. */
    int32 ResolveSection(UAnimMontage* Montage, const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        FString SectionName;
        if (Params->TryGetStringField(TEXT("section"), SectionName) && !SectionName.IsEmpty())
        {
            const int32 Index = Montage->GetSectionIndex(FName(*SectionName));
            if (Index == INDEX_NONE)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("no section named '%s' (see montage.get_info)"), *SectionName);
            }
            return Index;
        }

        double IndexValue = -1.0;
        if (Params->TryGetNumberField(TEXT("index"), IndexValue))
        {
            const int32 Index = static_cast<int32>(IndexValue);
            if (!Montage->IsValidSectionIndex(Index))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("section index %d out of range (%d sections)"), Index, Montage->GetNumSections());
                return INDEX_NONE;
            }
            return Index;
        }

        OutError.Code = MCPProtocol::FMCPError::InvalidParams;
        OutError.Message = TEXT("section (name) or index is required");
        return INDEX_NONE;
    }

    TSharedPtr<FJsonObject> MakeSectionJson(const UAnimMontage* Montage, int32 Index)
    {
        const FCompositeSection& Section = Montage->GetAnimCompositeSection(Index);
        float Start = 0.f, End = 0.f;
        Montage->GetSectionStartAndEndTime(Index, Start, End);

        auto Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("index"), Index);
        Entry->SetStringField(TEXT("name"), Section.SectionName.ToString());
        Entry->SetNumberField(TEXT("start_time"), Start);
        Entry->SetNumberField(TEXT("end_time"), End);
        Entry->SetStringField(TEXT("next"), Section.NextSectionName.IsNone() ? FString() : Section.NextSectionName.ToString());
        return Entry;
    }
}

void FMontageCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    // ---- BEGIN commands forwarded to BlueprintInspector (generated from its function headers) ----
    Registry.Register(TEXT("montage.dump_structure"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BlueprintInspectorLibrary"), TEXT("DumpMontageStructure"), { TEXT("asset_path") }));
    Registry.Register(TEXT("montage.create_from_sequence"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BlueprintInspectorLibrary"), TEXT("CreateMontageFromSequence"), { TEXT("sequence_path"), TEXT("dest_asset_path"), TEXT("slot_name"), TEXT("first_section_name") }));
    Registry.Register(TEXT("montage.add_slot_track"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BlueprintInspectorLibrary"), TEXT("AddMontageSlotTrack"), { TEXT("asset_path"), TEXT("slot_name") }));
    Registry.Register(TEXT("montage.set_slot_track_name"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BlueprintInspectorLibrary"), TEXT("SetMontageSlotTrackName"), { TEXT("asset_path"), TEXT("track_index"), TEXT("slot_name") }));
    Registry.Register(TEXT("montage.set_section_next"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BlueprintInspectorLibrary"), TEXT("SetMontageSectionNext"), { TEXT("asset_path"), TEXT("section_name"), TEXT("next_section_name") }));
    // ---- END forwarded commands ----

    // montage.get_info — 몽타주 한 장의 슬롯/세그먼트/섹션 전부. Params: montage_path.
    // Returns { path, skeleton, play_length, blend_in, blend_out,
    //           slots: [ { slot, length, segments: [ { index, animation, start_pos, anim_start, anim_end,
    //                                                  play_rate, loop_count, length } ] } ],
    //           sections: [ { index, name, start_time, end_time, next } ] }
    Registry.Register(TEXT("montage.get_info"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimMontage* Montage = LoadMontage(Params, OutError); if (!Montage) return nullptr;

            TArray<TSharedPtr<FJsonValue>> SlotsJson;
            for (const FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
            {
                TArray<TSharedPtr<FJsonValue>> SegmentsJson;
                for (int32 i = 0; i < Track.AnimTrack.AnimSegments.Num(); ++i)
                {
                    const FAnimSegment& Segment = Track.AnimTrack.AnimSegments[i];
                    const UAnimSequenceBase* Anim = Segment.GetAnimReference();

                    auto Entry = MakeShared<FJsonObject>();
                    Entry->SetNumberField(TEXT("index"), i);
                    Entry->SetStringField(TEXT("animation"), Anim ? Anim->GetPathName() : FString());
                    Entry->SetNumberField(TEXT("start_pos"), Segment.StartPos);
                    Entry->SetNumberField(TEXT("anim_start"), Segment.AnimStartTime);
                    Entry->SetNumberField(TEXT("anim_end"), Segment.AnimEndTime);
                    Entry->SetNumberField(TEXT("play_rate"), Segment.AnimPlayRate);
                    Entry->SetNumberField(TEXT("loop_count"), Segment.LoopingCount);
                    Entry->SetNumberField(TEXT("length"), Segment.GetLength());
                    SegmentsJson.Add(MakeShared<FJsonValueObject>(Entry));
                }

                auto SlotJson = MakeShared<FJsonObject>();
                SlotJson->SetStringField(TEXT("slot"), Track.SlotName.ToString());
                SlotJson->SetNumberField(TEXT("length"), Track.AnimTrack.GetLength());
                SlotJson->SetArrayField(TEXT("segments"), SegmentsJson);
                SlotsJson.Add(MakeShared<FJsonValueObject>(SlotJson));
            }

            TArray<TSharedPtr<FJsonValue>> SectionsJson;
            for (int32 i = 0; i < Montage->GetNumSections(); ++i)
            {
                SectionsJson.Add(MakeShared<FJsonValueObject>(MakeSectionJson(Montage, i)));
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("path"), Montage->GetPathName());
            Result->SetStringField(TEXT("skeleton"), Montage->GetSkeleton() ? Montage->GetSkeleton()->GetPathName() : FString());
            Result->SetNumberField(TEXT("play_length"), Montage->GetPlayLength());
            Result->SetNumberField(TEXT("blend_in"), Montage->BlendIn.GetBlendTime());
            Result->SetNumberField(TEXT("blend_out"), Montage->BlendOut.GetBlendTime());
            Result->SetArrayField(TEXT("slots"), SlotsJson);
            Result->SetArrayField(TEXT("sections"), SectionsJson);
            return Result;
        });

    // montage.add_segment — 슬롯 트랙에 애님 시퀀스를 한 구간 얹는다. Params: montage_path, animation_path,
    // slot (optional, default DefaultSlot), start_pos (optional, 기본은 그 슬롯 끝에 이어붙임),
    // play_rate (optional, 1.0), loop_count (optional, 1).
    // Returns { ok, slot, index, start_pos, length, play_length }.
    Registry.Register(TEXT("montage.add_segment"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimMontage* Montage = LoadMontage(Params, OutError); if (!Montage) return nullptr;

            FString AnimPath;
            if (!UnrealMCPBridge::Json::RequireString(Params, TEXT("animation_path"), AnimPath, OutError))
            {
                return nullptr;
            }
            UAnimSequenceBase* Anim = LoadObject<UAnimSequenceBase>(nullptr, *AnimPath);
            if (!Anim)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("Could not load animation: %s"), *AnimPath);
                return nullptr;
            }
            // 스켈레톤이 다르면 IsValidToAdd 도 막지만, 그쪽 사유 문자열은 원인을 짚어주지 않는다.
            if (Anim->GetSkeleton() != Montage->GetSkeleton())
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("skeleton mismatch: montage is on %s, %s is on %s"),
                    *GetNameSafe(Montage->GetSkeleton()), *Anim->GetName(), *GetNameSafe(Anim->GetSkeleton()));
                return nullptr;
            }

            const FName SlotName = ReadSlotName(Params);
            Montage->Modify();

            FSlotAnimationTrack* Track = FindSlot(Montage, SlotName);
            if (!Track)
            {
                // AddSlot 이 배열을 재할당할 수 있으므로 반환된 참조를 그대로 쓴다.
                Track = &Montage->AddSlot(SlotName);
            }

            FText Reason;
            if (!Track->AnimTrack.IsValidToAdd(Anim, &Reason))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("cannot add %s to slot %s: %s"),
                    *Anim->GetName(), *SlotName.ToString(), *Reason.ToString());
                return nullptr;
            }

            double StartPos = 0.0;
            if (!Params->TryGetNumberField(TEXT("start_pos"), StartPos))
            {
                StartPos = SlotEndTime(*Track);
            }
            double PlayRate = 1.0;
            Params->TryGetNumberField(TEXT("play_rate"), PlayRate);
            if (FMath::IsNearlyZero(PlayRate))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("play_rate must not be 0");
                return nullptr;
            }
            double LoopCount = 1.0;
            Params->TryGetNumberField(TEXT("loop_count"), LoopCount);

            FAnimSegment NewSegment;
            // bInitialize=true 가 AnimStartTime/AnimEndTime 을 애셋 전체 길이로 채운다. 직접 넣으면
            // 0..0 인 채로 들어가 길이 0짜리 구간이 된다.
            NewSegment.SetAnimReference(Anim, /*bInitialize*/ true);
            NewSegment.StartPos = static_cast<float>(StartPos);
            NewSegment.AnimPlayRate = static_cast<float>(PlayRate);
            NewSegment.LoopingCount = FMath::Max(1, static_cast<int32>(LoopCount));
#if WITH_EDITOR
            NewSegment.UpdateCachedPlayLength();
#endif

            Track->AnimTrack.AnimSegments.Add(NewSegment);
            Track->AnimTrack.SortAnimSegments();
            RebuildMontage(Montage);

            // RebuildMontage 가 PostEditChange 를 태우므로 슬롯 참조를 다시 잡는다 — 배열이 재할당되면
            // 위에서 들고 있던 포인터가 남의 메모리를 가리킨다.
            Track = FindSlot(Montage, SlotName);
            if (!Track)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = FString::Printf(TEXT("slot '%s' disappeared while rebuilding the montage"), *SlotName.ToString());
                return nullptr;
            }

            // 정렬 뒤 인덱스가 밀리므로 넣은 자리를 다시 찾는다 — 부른 쪽이 곧바로 remove_segment 로
            // 되돌릴 때 엉뚱한 구간을 지우지 않게.
            int32 FinalIndex = INDEX_NONE;
            for (int32 i = 0; i < Track->AnimTrack.AnimSegments.Num(); ++i)
            {
                const FAnimSegment& Segment = Track->AnimTrack.AnimSegments[i];
                if (Segment.GetAnimReference() == Anim && FMath::IsNearlyEqual(Segment.StartPos, static_cast<float>(StartPos)))
                {
                    FinalIndex = i;
                    break;
                }
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            Result->SetStringField(TEXT("slot"), SlotName.ToString());
            Result->SetNumberField(TEXT("index"), FinalIndex);
            Result->SetNumberField(TEXT("start_pos"), StartPos);
            Result->SetNumberField(TEXT("length"), Track->AnimTrack.AnimSegments.IsValidIndex(FinalIndex)
                ? Track->AnimTrack.AnimSegments[FinalIndex].GetLength() : 0.f);
            Result->SetNumberField(TEXT("play_length"), Montage->GetPlayLength());
            return Result;
        });

    // montage.remove_segment — Params: montage_path, index, slot (optional, default DefaultSlot).
    // Returns { ok, slot, play_length }.
    Registry.Register(TEXT("montage.remove_segment"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimMontage* Montage = LoadMontage(Params, OutError); if (!Montage) return nullptr;

            const FName SlotName = ReadSlotName(Params);
            FSlotAnimationTrack* Track = FindSlot(Montage, SlotName);
            if (!Track)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("no slot named '%s'"), *SlotName.ToString());
                return nullptr;
            }

            double IndexValue = -1.0;
            if (!Params->TryGetNumberField(TEXT("index"), IndexValue))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("index is required (see montage.get_info)");
                return nullptr;
            }
            const int32 Index = static_cast<int32>(IndexValue);
            if (!Track->AnimTrack.AnimSegments.IsValidIndex(Index))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("segment index %d out of range (%d segments in %s)"),
                    Index, Track->AnimTrack.AnimSegments.Num(), *SlotName.ToString());
                return nullptr;
            }

            Montage->Modify();
            Track->AnimTrack.AnimSegments.RemoveAt(Index);
            RebuildMontage(Montage);

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            Result->SetStringField(TEXT("slot"), SlotName.ToString());
            Result->SetNumberField(TEXT("play_length"), Montage->GetPlayLength());
            return Result;
        });

    // montage.add_section — 섹션 하나를 만든다. Params: montage_path, name, start_time (optional, 0),
    // next (optional; 자기 이름을 주면 그 자리에서 루프한다).
    // Returns { ok, index, name, start_time, end_time, next }.
    //
    // 엔진이 삽입 시점에 **바로 앞 섹션의 비어 있던 next 를 새 섹션으로 채운다.** 단계가 갈리는 지점
    // (탈출/사망처럼 서로 이어지면 안 되는 섹션)에서는 그 뒤에 set_section 의 next="" 로 끊어야 한다.
    // 이미 채워져 있는 next 는 건드리지 않으므로, 루프처럼 명시해 둔 링크는 안전하다.
    //
    // 이 자동 링크는 **RebuildMontage 의 정렬 전** 배열 위치를 본다. 시각 역순으로 넣으면 정렬이 끝난 뒤
    // 링크가 앞뒤로 튄다(2.5초 → 1.0초 순으로 넣었더니 Default→Late, Late→Early 가 됐다). 재생 순서대로
    // 넣거나, 다 넣은 뒤 next 를 전부 명시하는 쪽이 안전하다.
    Registry.Register(TEXT("montage.add_section"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimMontage* Montage = LoadMontage(Params, OutError); if (!Montage) return nullptr;

            FString Name;
            if (!UnrealMCPBridge::Json::RequireString(Params, TEXT("name"), Name, OutError))
            {
                return nullptr;
            }
            double StartTime = 0.0;
            Params->TryGetNumberField(TEXT("start_time"), StartTime);

            const float PlayLength = Montage->GetPlayLength();
            if (StartTime < 0.0 || StartTime > PlayLength + KINDA_SMALL_NUMBER)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("start_time %.3f falls outside the montage (0..%.3f)"), StartTime, PlayLength);
                return nullptr;
            }

            Montage->Modify();
            if (Montage->AddAnimCompositeSection(FName(*Name), static_cast<float>(StartTime)) == INDEX_NONE)
            {
                // 엔진이 INDEX_NONE 을 내는 사유는 이름 중복 하나뿐이다.
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("a section named '%s' already exists"), *Name);
                return nullptr;
            }

            // next 는 섹션이 생긴 뒤에 건다. 자기 자신을 가리키는 루프도 여기서 성립한다.
            FString Next;
            if (Params->TryGetStringField(TEXT("next"), Next) && !Next.IsEmpty())
            {
                if (Montage->GetSectionIndex(FName(*Next)) == INDEX_NONE)
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = FString::Printf(TEXT("next '%s' is not a section on this montage"), *Next);
                    return nullptr;
                }
                // 삽입 위치에 따라 정렬되므로 인덱스를 다시 찾는다.
                Montage->GetAnimCompositeSection(Montage->GetSectionIndex(FName(*Name))).NextSectionName = FName(*Next);
            }

            RebuildMontage(Montage);

            const int32 FinalIndex = Montage->GetSectionIndex(FName(*Name));
            auto Result = MakeSectionJson(Montage, FinalIndex);
            Result->SetBoolField(TEXT("ok"), true);
            return Result;
        });

    // montage.set_section — 기존 섹션의 이름/다음 섹션을 바꾼다. Params: montage_path,
    // section (이름) 또는 index, name (optional, 이름 변경), next (optional; ""를 주면 링크를 끊어
    // 그 섹션 끝에서 몽타주가 멈춘다).
    // Returns { ok, index, name, start_time, end_time, next }.
    //
    // start_time 은 여기서 안 받는다. 시각을 옮기려면 remove_section 후 add_section 을 쓴다 —
    // RebuildMontage 가 시각 순으로 다시 정렬하므로 넣는 순서는 상관없다.
    Registry.Register(TEXT("montage.set_section"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimMontage* Montage = LoadMontage(Params, OutError); if (!Montage) return nullptr;

            const int32 Index = ResolveSection(Montage, Params, OutError);
            if (Index == INDEX_NONE)
            {
                return nullptr;
            }

            FString NewName, Next;
            const bool bRename = Params->TryGetStringField(TEXT("name"), NewName) && !NewName.IsEmpty();
            const bool bSetNext = Params->TryGetStringField(TEXT("next"), Next);
            if (!bRename && !bSetNext)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("provide name and/or next");
                return nullptr;
            }

            const FName OldName = Montage->GetSectionName(Index);

            if (bRename && FName(*NewName) != OldName && Montage->GetSectionIndex(FName(*NewName)) != INDEX_NONE)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("a section named '%s' already exists"), *NewName);
                return nullptr;
            }
            // next 검증은 이름 변경보다 **먼저** 한다. 나중에 하면 이름만 바뀐 채 실패해 애셋이 반쪽으로 남는다.
            if (bSetNext && !Next.IsEmpty())
            {
                const bool bTargetsRenamed = bRename && Next == NewName;
                if (!bTargetsRenamed && Montage->GetSectionIndex(FName(*Next)) == INDEX_NONE)
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = FString::Printf(TEXT("next '%s' is not a section on this montage"), *Next);
                    return nullptr;
                }
            }

            Montage->Modify();

            if (bRename)
            {
                Montage->GetAnimCompositeSection(Index).SectionName = FName(*NewName);
                // 링크는 이름으로 걸려 있다 — 이 섹션을 가리키던 다른 섹션의 next 를 같이 고치지 않으면
                // 그쪽이 조용히 끊긴 링크가 되어 몽타주가 거기서 멈춘다.
                for (int32 i = 0; i < Montage->GetNumSections(); ++i)
                {
                    FCompositeSection& Other = Montage->GetAnimCompositeSection(i);
                    if (Other.NextSectionName == OldName)
                    {
                        Other.NextSectionName = FName(*NewName);
                    }
                }
            }

            if (bSetNext)
            {
                Montage->GetAnimCompositeSection(Index).NextSectionName = Next.IsEmpty() ? NAME_None : FName(*Next);
            }

            RebuildMontage(Montage);

            const int32 FinalIndex = Montage->GetSectionIndex(bRename ? FName(*NewName) : OldName);
            auto Result = MakeSectionJson(Montage, FinalIndex);
            Result->SetBoolField(TEXT("ok"), true);
            return Result;
        });

    // montage.remove_section — Params: montage_path, section (이름) 또는 index.
    // Returns { ok, removed, sections: [...] }.
    Registry.Register(TEXT("montage.remove_section"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimMontage* Montage = LoadMontage(Params, OutError); if (!Montage) return nullptr;

            const int32 Index = ResolveSection(Montage, Params, OutError);
            if (Index == INDEX_NONE)
            {
                return nullptr;
            }
            const FName Removed = Montage->GetSectionName(Index);

            Montage->Modify();
            if (!Montage->DeleteAnimCompositeSection(Index))
            {
                // 엔진은 마지막 한 개는 지우지 못하게 막는다 — 섹션 없는 몽타주는 재생 시작점이 없다.
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("could not delete section '%s' (a montage must keep at least one section — rename it instead)"), *Removed.ToString());
                return nullptr;
            }

            // 지운 섹션을 가리키던 링크는 끊어 둔다. 그대로 두면 존재하지 않는 이름을 가리켜, 재생이
            // 거기서 멈추는데 이유가 애셋 어디에도 안 보인다.
            for (int32 i = 0; i < Montage->GetNumSections(); ++i)
            {
                FCompositeSection& Other = Montage->GetAnimCompositeSection(i);
                if (Other.NextSectionName == Removed)
                {
                    Other.NextSectionName = NAME_None;
                }
            }

            RebuildMontage(Montage);

            TArray<TSharedPtr<FJsonValue>> SectionsJson;
            for (int32 i = 0; i < Montage->GetNumSections(); ++i)
            {
                SectionsJson.Add(MakeShared<FJsonValueObject>(MakeSectionJson(Montage, i)));
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            Result->SetStringField(TEXT("removed"), Removed.ToString());
            Result->SetArrayField(TEXT("sections"), SectionsJson);
            return Result;
        });
}
