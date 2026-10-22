#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

/*
 physicsasset.* JSON-RPC 처리 - 피직스 애셋의 **바디(콜리전 도형)를 읽고 만들고 지운다.**

 **파이썬으로는 만들 수가 없다.** `USkeletalBodySetup` 이 스크립팅 계층에 노출돼 있지 않아
 `unreal.SkeletalBodySetup` 자체가 없고(`hasattr` 이 False), 그래서 `new_object` 로 바디를 만들 길이
 없다. 읽는 쪽은 반쯤 열려 있다 - 서브오브젝트를 `SkeletalBodySetup_%d` 이름으로 직접 잡으면
 `bone_name` / `physics_type` / `agg_geom` 은 읽고 쓸 수 있다. 즉 **있는 바디를 고치는 것은 되는데
 새로 넣는 것만 안 된다.** 이 핸들러는 그 구멍을 메운다.

 왜 필요했는가(2026-08-26): 리타 머리카락 클로스가 몸을 통과했다. 클로스는 아무 메시하고나 부딪히지
 않고 **지정된 피직스 애셋의 바디만** 콜라이더로 쓰는데, `PA_Rita_Hair` 에는 `head` 와 `neck_02`
 밖에 없어 목 아래로 막을 것이 없었다. 필요한 도형(등·어깨·팔)은 같은 스켈레톤의
 `PA_Rita_Equipment` 에 이미 저작돼 있었으므로, 그 값을 읽어 그대로 옮기는 것이 답이었다.

 **컨스트레인트는 만들지 않는다.** 이 커맨드가 넣는 바디는 콜라이더 용도(`Kinematic`)를 상정한다 -
 부모에 물릴 조인트가 없어도 본을 따라다니기만 하면 되고, 시뮬레이션할 바디를 체인으로 엮는 것은
 도형 하나 넣는 것과 성격이 다른 작업이라 피직스 애셋 에디터가 맞다.
*/
class FPhysicsAssetCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
