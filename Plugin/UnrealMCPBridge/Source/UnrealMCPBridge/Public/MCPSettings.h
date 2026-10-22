#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MCPSettings.generated.h"

// Project Settings > Plugins > Unreal MCP Bridge.
// 값은 [/Script/UnrealMCPBridge.MCPSettings] 섹션에 저장되며, DefaultEngine.ini가 아니라
// 플러그인 전용 config 브랜치를 쓴다. config 이름은 반드시 플러그인 이름과 같아야 한다 —
// 그래야 엔진이 Plugins/UnrealMCPBridge/Config/DefaultUnrealMCPBridge.ini를 기본값 레이어로 읽는다.
// Project Settings에서 바꾼 값은 <Project>/Saved/Config/<Platform>/UnrealMCPBridge.ini에 기록되며,
// 플러그인 폴더로는 절대 되쓰이지 않는다.
UCLASS(config=UnrealMCPBridge, meta=(DisplayName="Unreal MCP Bridge"))
class UNREALMCPBRIDGE_API UMCPSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    // 에디터 시작 시 TCP 리스너를 켠다.
    UPROPERTY(EditAnywhere, config, Category="MCP")
    bool bEnabled = false;

    // 리스너가 바인딩할 주소.
    UPROPERTY(EditAnywhere, config, Category="MCP")
    FString Host = TEXT("127.0.0.1");

    // 리스너 포트. 0이면 비활성이며, 0보다 큰 값을 넣어야 동작한다.
    UPROPERTY(EditAnywhere, config, Category="MCP")
    int32 Port = 0;

    // 추가 리스너 포트. 리스너 하나는 클라이언트 하나만 받는다 — FMCPServerRunnable::Run의
    // per-client 루프가 블로킹이라 그 클라이언트가 끊기기 전에는 accept로 돌아오지 못하고,
    // 뒤에 붙으려는 쪽은 connection refused를 맞는다. 그래서 MCP 클라이언트를 동시에
    // 여러 개 붙이려면 포트를 나눠야 한다(예: Claude Code는 Port=30100, Codex는 여기 30101).
    // Port와 중복되는 값과 0 이하는 무시된다.
    UPROPERTY(EditAnywhere, config, Category="MCP")
    TArray<int32> AdditionalPorts;

    // GameThread 디스패치를 기다리는 시간(초). 초과하면 InternalError를 반환한다.
    UPROPERTY(EditAnywhere, config, Category="MCP")
    int32 GameThreadDispatchTimeoutSeconds = 30;

    // 허용하는 JSON 한 줄의 최대 길이(바이트).
    UPROPERTY(EditAnywhere, config, Category="MCP")
    int32 MaxLineBytes = 16777216;

    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

#if WITH_EDITOR
    // 설정이 바뀔 때마다 브리지 리스너를 재시작한다. 덕분에 Project Settings에서
    // bEnabled / Port / AdditionalPorts를 바꾸면 에디터를 다시 켜지 않아도 즉시 반영된다.
    virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
