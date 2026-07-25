#include "MCPBridgeSubsystem.h"
#include "UnrealMCPBridgeModule.h"
#include "MCPSettings.h"
#include "Editor.h"
#include "Misc/CoreDelegates.h"
#include "Commands/EditorCommands.h"
#include "Commands/BlueprintCommands.h"
#include "Commands/AssetCommands.h"
#include "Commands/CompileCommands.h"
#include "Commands/PIECommands.h"
#include "Commands/AnimGraphCommands.h"
#include "Commands/AnimNotifyCommands.h"
#include "Commands/StateTreeCommands.h"
#include "Commands/CameraRigCommands.h"
#include "Commands/CameraVariableCommands.h"
#include "Commands/MaterialCommands.h"

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

    if (Server)
    {
        // Already listening — nothing to do.
        return;
    }

    // Create the command registry + domain handlers once; reuse across restarts.
    if (!Registry.IsValid())
    {
        Registry = MakeShared<FMCPCommandRegistry>();

        Handlers.Add(MakeShared<FEditorCommandHandler>());
        Handlers.Add(MakeShared<FBlueprintCommandHandler>());
        Handlers.Add(MakeShared<FAssetCommandHandler>());
        Handlers.Add(MakeShared<FCompileCommandHandler>());
        Handlers.Add(MakeShared<FPIECommandHandler>());
        Handlers.Add(MakeShared<FAnimGraphCommandHandler>());
        Handlers.Add(MakeShared<FAnimNotifyCommandHandler>());
        Handlers.Add(MakeShared<FStateTreeCommandHandler>());
        Handlers.Add(MakeShared<FCameraRigCommandHandler>());
        Handlers.Add(MakeShared<FCameraVariableCommandHandler>());
        Handlers.Add(MakeShared<FMaterialCommandHandler>());

        for (const TSharedPtr<IMCPCommandHandler>& Handler : Handlers)
        {
            if (Handler.IsValid())
            {
                Handler->RegisterCommands(*Registry);
            }
        }
    }

    // Start the TCP listener.
    Server = MakeUnique<FMCPServerRunnable>(
        Settings->Host,
        Settings->Port,
        Settings->MaxLineBytes,
        Settings->GameThreadDispatchTimeoutSeconds,
        Registry.ToSharedRef());
    Server->StartThread();

    // Ensure clean shutdown when the editor exits.
    if (!OnExitHandle.IsValid())
    {
        OnExitHandle = FCoreDelegates::OnPreExit.AddUObject(this, &UMCPBridgeSubsystem::HandleEditorExit);
    }

    UE_LOG(LogMCPBridge, Log,
        TEXT("MCPBridgeSubsystem: started listener on %s:%d."), *Settings->Host, Settings->Port);
}

void UMCPBridgeSubsystem::StopListener()
{
    if (Server)
    {
        UE_LOG(LogMCPBridge, Log, TEXT("MCPBridgeSubsystem: stopping listener."));
        Server->RequestStopAndWait();
        Server.Reset();
    }
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
