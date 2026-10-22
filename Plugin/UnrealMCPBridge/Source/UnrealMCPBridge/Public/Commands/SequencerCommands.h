#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Handles sequencer.* JSON-RPC methods — Level Sequence bindings, sections, marked frames and
// baking a binding to an AnimSequence, all without opening the Sequencer editor.
//
// 애니 팀이 Python 으로 하다 막힌 것들이다(2026-10-19 요청 2차).
// - add_spawnable_from_instance 에 스켈레탈 메시 애셋을 주면 액터 스폰어블 바인딩이 아니라 디렉터
//   블루프린트 바인딩이 생겨 아무것도 스폰되지 않는다(UMovieSceneSpawnableActorBinding 은 액터·액터
//   클래스만 받는다). 여기선 임시 SkeletalMeshActor 를 만들어 그걸 템플릿으로 넘긴다.
// - export_anim_sequence 를 open_level_sequence 와 같은 호출에서 부르면 실패한다. 엔진 함수는 스폰 키가
//   일정한 스폰어블이면 플레이어를 평가하지 않아(스폰 안 됨) 열린 시퀀서가 대신 스폰해 둬야만 굽힌다.
//   여기선 플레이어를 직접 띄워 시작 프레임을 평가한 뒤 MovieSceneToolHelpers::ExportToAnimSequence 로 굽는다.
// - get_marked_frames 는 deprecated, 섹션 조회는 첫 섹션만 보기 쉬워 Additive 레이어를 놓친다.
class FSequencerCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
