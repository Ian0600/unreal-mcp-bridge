#include "MCPBridgeSubsystem.h"
#include "UnrealMCPBridgeModule.h"
#include "MCPSettings.h"
#include "Editor.h"
#include "Misc/CoreDelegates.h"
#include "Commands/EditorCommands.h"
#include "Commands/ObjectCommands.h"
#include "Commands/BlueprintCommands.h"
#include "Commands/AssetCommands.h"
#include "Commands/CompileCommands.h"
#include "Commands/PIECommands.h"
#include "Commands/AnimGraphCommands.h"
#include "Commands/AnimNotifyCommands.h"
#include "Commands/MontageCommands.h"
#include "Commands/SkeletalMeshCommands.h"
#include "Commands/StateTreeCommands.h"
#include "Commands/CameraRigCommands.h"
#include "Commands/CameraVariableCommands.h"
#include "Commands/MaterialCommands.h"
#include "Commands/NiagaraCommands.h"
#include "Commands/WidgetCommands.h"
#include "Commands/WidgetAnimCommands.h"
#include "Commands/LevelIOCommands.h"

void UMCPBridgeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    StartListener();
}

void UMCPBridgeSubsystem::StartListener()
{
    const UMCPSettings* Settings = GetDefault<UMCPSettings>();
    if (!Settings->bEnabled || Settings->Port <= 0)
    {
        UE_LOG(LogMCPBridge, Log,
            TEXT("MCPBridgeSubsystem: disabled (bEnabled=%s, Port=%d). Set both in Project Settings to activate."),
            Settings->bEnabled ? TEXT("true") : TEXT("false"),
            Settings->Port);
        return;
    }

    if (Servers.Num() > 0)
    {
        // Already listening — nothing to do.
        return;
    }

    // Create the command registry + domain handlers once; reuse across restarts.
    if (!Registry.IsValid())
    {
        Registry = MakeShared<FMCPCommandRegistry>();

        Handlers.Add(MakeShared<FEditorCommandHandler>());
        Handlers.Add(MakeShared<FObjectCommandHandler>());
        Handlers.Add(MakeShared<FBlueprintCommandHandler>());
        Handlers.Add(MakeShared<FAssetCommandHandler>());
        Handlers.Add(MakeShared<FCompileCommandHandler>());
        Handlers.Add(MakeShared<FPIECommandHandler>());
        Handlers.Add(MakeShared<FAnimGraphCommandHandler>());
        Handlers.Add(MakeShared<FAnimNotifyCommandHandler>());
        Handlers.Add(MakeShared<FMontageCommandHandler>());
        Handlers.Add(MakeShared<FStateTreeCommandHandler>());
        Handlers.Add(MakeShared<FCameraRigCommandHandler>());
        Handlers.Add(MakeShared<FCameraVariableCommandHandler>());
        Handlers.Add(MakeShared<FMaterialCommandHandler>());
        Handlers.Add(MakeShared<FNiagaraCommandHandler>());
        Handlers.Add(MakeShared<FWidgetCommandHandler>());
        Handlers.Add(MakeShared<FWidgetAnimCommandHandler>());
        Handlers.Add(MakeShared<FLevelIOCommandHandler>());
        Handlers.Add(MakeShared<FSkeletalMeshCommandHandler>());

        for (const TSharedPtr<IMCPCommandHandler>& Handler : Handlers)
        {
            if (Handler.IsValid())
            {
                Handler->RegisterCommands(*Registry);
            }
        }
    }

    // 주 포트 + 추가 포트. 같은 포트로 러너블을 둘 띄우면 뒤쪽 Bind가 실패해 리스너
    // 하나가 조용히 죽으므로, 중복과 0 이하는 여기서 걸러낸다.
    TArray<int32> PortsToOpen;
    PortsToOpen.Add(Settings->Port);
    for (const int32 ExtraPort : Settings->AdditionalPorts)
    {
        if (ExtraPort > 0)
        {
            PortsToOpen.AddUnique(ExtraPort);
        }
    }

    // 포트마다 리스너를 하나씩 띄운다. 리스너 하나가 클라이언트 하나만 받으므로
    // (FMCPServerRunnable::Run의 per-client 루프가 블로킹) 여러 MCP 클라이언트를
    // 동시에 붙이려면 이 방법뿐이다. 레지스트리는 하나를 공유해도 안전하다 —
    // 실제 작업은 전부 DispatchToGameThread가 GameThread로 직렬화하기 때문이다.
    for (const int32 PortToOpen : PortsToOpen)
    {
        TUniquePtr<FMCPServerRunnable> Listener = MakeUnique<FMCPServerRunnable>(
            Settings->Host,
            PortToOpen,
            Settings->MaxLineBytes,
            Settings->GameThreadDispatchTimeoutSeconds,
            Registry.ToSharedRef());
        Listener->StartThread();
        Servers.Add(MoveTemp(Listener));

        UE_LOG(LogMCPBridge, Log,
            TEXT("MCPBridgeSubsystem: started listener on %s:%d."), *Settings->Host, PortToOpen);
    }

    // Ensure clean shutdown when the editor exits.
    if (!OnExitHandle.IsValid())
    {
        OnExitHandle = FCoreDelegates::OnPreExit.AddUObject(this, &UMCPBridgeSubsystem::HandleEditorExit);
    }
}

void UMCPBridgeSubsystem::StopListener()
{
    if (Servers.Num() == 0)
    {
        return;
    }

    UE_LOG(LogMCPBridge, Log, TEXT("MCPBridgeSubsystem: stopping %d listener(s)."), Servers.Num());
    for (TUniquePtr<FMCPServerRunnable>& Listener : Servers)
    {
        if (Listener)
        {
            Listener->RequestStopAndWait();
        }
    }
    Servers.Reset();
}

void UMCPBridgeSubsystem::RestartFromSettings()
{
    StopListener();
    StartListener();
}

void UMCPBridgeSubsystem::Deinitialize()
{
    HandleEditorExit();
    Super::Deinitialize();
}

void UMCPBridgeSubsystem::HandleEditorExit()
{
    StopListener();

    if (OnExitHandle.IsValid())
    {
        FCoreDelegates::OnPreExit.Remove(OnExitHandle);
        OnExitHandle.Reset();
    }
}
