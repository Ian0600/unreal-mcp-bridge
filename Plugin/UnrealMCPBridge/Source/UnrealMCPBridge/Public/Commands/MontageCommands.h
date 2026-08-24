#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Handles montage.* JSON-RPC methods — composing an AnimMontage: slot segments and the section table.
//
// 이 두 가지는 **Python으로 못 한다.** editor_python_exec 에서:
//  - `CompositeSections` 는 protected 라 get/set_editor_property 가 둘 다 거부한다.
//  - `SlotAnimTracks` 는 쓰기가 되지만 `sequence_length` 가 Read-Only 라, Python 으로 세그먼트를 여러 개
//    얹으면 길이가 첫 세그먼트 그대로 남는다. 애셋은 만들어지는데 뒤쪽 구간이 재생되지 않는다.
// 엔진 C++ 쪽은 AddAnimCompositeSection / CalculateSequenceLength / SetCompositeLength 가 전부 열려 있어서,
// 브릿지에서는 그냥 된다. animnotify.* 가 UAnimSequenceBase::Notifies 때문에 여기 있는 것과 같은 이유다.
//
// 섹션을 쓰는 이유는 잡기·처형처럼 **단계가 이어지는 연출** 때문이다. 단계마다 몽타주를 따로 두면 단계
// 전환마다 블렌드 아웃/인이 끼고, UIFSRAnimInstance::PickMontageForTag 가 한 태그의 몽타주 배열을 연출
// 변형(A/B)으로 보고 커서로 돌리기 때문에 순서대로 재생되지도 않는다. 한 몽타주 + 섹션이면 태그 하나로
// 해결되고, FIFSRMontageSet::Section 과 UAbilityTask_PlayMontageAndWait 의 StartSection 이 이미 그 모양을
// 받게 돼 있다.
class FMontageCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
