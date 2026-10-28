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

    // 추가 리스너 포트. 리스너는 클라이언트마다 스레드를 띄우므로 포트 하나에 여러 MCP 클라이언트가
    // 동시에 붙는다(2026-10-28 이전에는 포트당 하나라 포트를 나눠야 했다). 지금은 옛 플러그인을 쓰는
    // 클라이언트 설정(UMCP_PORT=30101 등)이 계속 붙도록 남겨 둔 것이다.
    // Port와 중복되는 값과 0 이하는 무시된다.
    UPROPERTY(EditAnywhere, config, Category="MCP")
    TArray<int32> AdditionalPorts;

    // GameThread 디스패치를 기다리는 시간(초). 초과하면 InternalError를 반환한다.
    UPROPERTY(EditAnywhere, config, Category="MCP")
    int32 GameThreadDispatchTimeoutSeconds = 30;

    // 허용하는 JSON 한 줄의 최대 길이(바이트).
    UPROPERTY(EditAnywhere, config, Category="MCP")
    int32 MaxLineBytes = 16777216;

    // scene.spawn_temp 가 임시로 놓아도 되는 블루프린트. 패키지 경로 접두사(/Game/Dev/Props/)나
    // 정확한 애셋 경로(/Game/Dev/BP_Crate.BP_Crate)를 적는다. 비어 있으면 블루프린트는 전부 거절한다 —
    // construction script 가 다른 레벨 액터를 만들거나 바꿀 수 있어서, 「임시」를 C++ 이 보장하려면
    // 사람이 확인한 것만 열어야 한다. 메시 · 라이트 · 블록아웃은 이 목록과 상관없이 된다.
    UPROPERTY(EditAnywhere, config, Category="MCP|Scene")
    TArray<FString> TempSpawnBlueprintAllowList;

    // scene.pose_character 의 preset 을 읽는 json 파일. 비우면 <Project>/Saved/MCPBridge/pose_presets.json.
    // 상대 경로는 프로젝트 폴더 기준이다. 형식은 PROTOCOL.md 의 scene.pose_character 절.
    UPROPERTY(EditAnywhere, config, Category="MCP|Scene")
    FString PosePresetFile;

    // scene.capture_view 가 파일을 써도 되는 폴더(절대 경로 또는 프로젝트 기준 상대 경로).
    // <Project>/Saved 는 항상 허용된다. Content 아래를 넣으면 Perforce 에 없는 파일이 생기니 넣지 않는다.
    UPROPERTY(EditAnywhere, config, Category="MCP|Scene")
    TArray<FString> CaptureOutputRoots;

    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

#if WITH_EDITOR
    // 설정이 바뀔 때마다 브리지 리스너를 재시작한다. 덕분에 Project Settings에서
    // bEnabled / Port / AdditionalPorts를 바꾸면 에디터를 다시 켜지 않아도 즉시 반영된다.
    virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
