#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "IMCPCommandHandler.h"
#include "MCPCommandRegistry.h"
#include "MCPServerRunnable.h"
#include "MCPBridgeSubsystem.generated.h"

// Editor subsystem that owns the TCP listener lifetime.
UCLASS()
class UNREALMCPBRIDGE_API UMCPBridgeSubsystem : public UEditorSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    // Stop then (re)start the listener according to the current UMCPSettings.
    // Called by UMCPSettings::PostEditChangeProperty so toggling the setting in
    // Project Settings takes effect immediately — no editor restart required.
    void RestartFromSettings();

private:
    // Start the TCP listeners if settings allow it (bEnabled && Port > 0).
    // Opens one listener per port: Port plus every valid entry in AdditionalPorts.
    // Idempotent: does nothing if already running or disabled.
    void StartListener();

    // Stop every running listener. Safe to call multiple times.
    void StopListener();

    // Gracefully stop the servers; safe to call multiple times (idempotent).
    void HandleEditorExit();

    // 포트마다 리스너 하나. 리스너 한 개가 클라이언트 한 개만 받으므로
    // (FMCPServerRunnable::Run의 per-client 루프가 블로킹) 동시 접속 클라이언트
    // 수만큼 포트가 필요하고, 그래서 단일 포인터가 아니라 배열이다.
    TArray<TUniquePtr<FMCPServerRunnable>> Servers;
    TSharedPtr<FMCPCommandRegistry> Registry;
    FDelegateHandle OnExitHandle;

    // Domain handler instances — domain-implementer populates these.
    TArray<TSharedPtr<IMCPCommandHandler>> Handlers;
};
