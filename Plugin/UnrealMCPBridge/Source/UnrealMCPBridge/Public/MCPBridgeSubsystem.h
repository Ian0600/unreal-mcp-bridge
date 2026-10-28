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

    // 포트마다 리스너 하나(Port + AdditionalPorts). 리스너 하나가 클라이언트를 여럿 받는다.
    TArray<TUniquePtr<FMCPServerRunnable>> Servers;
    TSharedPtr<FMCPCommandRegistry> Registry;
    FDelegateHandle OnExitHandle;

    // Domain handler instances — domain-implementer populates these.
    TArray<TSharedPtr<IMCPCommandHandler>> Handlers;
};
