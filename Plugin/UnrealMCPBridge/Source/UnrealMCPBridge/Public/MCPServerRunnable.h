#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/ThreadSafeCounter.h"
#include "Sockets.h"
#include "MCPCommandRegistry.h"

// Worker thread that accepts TCP connections and processes JSON-RPC 2.0 requests.
// Every accepted client is served on its own thread, so any number of clients can share one port;
// requests are still executed one at a time on the GameThread.
class UNREALMCPBRIDGE_API FMCPServerRunnable : public FRunnable
{
public:
    FMCPServerRunnable(
        const FString& Host,
        int32 Port,
        int32 MaxLineBytes,
        int32 DispatchTimeoutSeconds,
        TSharedRef<FMCPCommandRegistry> Registry);

    virtual ~FMCPServerRunnable();

    // FRunnable interface.
    virtual bool Init() override;
    virtual uint32 Run() override;
    virtual void Stop() override;
    virtual void Exit() override;

    // Spawn the worker thread and begin accepting connections.
    void StartThread();

    // Signal stop, block until the listener and every client thread finish, then release all resources.
    // Never calls Kill — always waits for clean completion.
    void RequestStopAndWait();

private:
    // Serve one accepted client until it disconnects or Stop is requested. Runs on its own
    // thread and owns (destroys) ClientSocket.
    void ServeClient(FSocket* ClientSocket, const FString& ClientName);

    // Read bytes from ClientSocket into ReceiveBuffer until '\n' is found.
    // ReceiveBuffer belongs to the calling client, so clients can be served concurrently.
    // Returns false when the connection is closed or the line exceeds MaxLineBytes.
    bool ReceiveLine(FSocket* ClientSocket, TArray<uint8>& ReceiveBuffer, FString& OutLine, bool& bOutExceededLimit);

    // Send a serialized response followed by '\n' to ClientSocket.
    bool SendResponse(FSocket* ClientSocket, const FString& Serialized);

    // Dispatch a parsed request to the GameThread and wait for the result.
    MCPProtocol::FMCPResponse DispatchToGameThread(const MCPProtocol::FMCPRequest& Request);

    const FString BindHost;
    const int32 BindPort;
    const int32 MaxLineBytesLimit;
    const int32 DispatchTimeoutSec;
    TSharedRef<FMCPCommandRegistry> CommandRegistry;

    FSocket* ListenSocket = nullptr;
    FRunnableThread* Thread = nullptr;
    FThreadSafeBool bStopRequested;

    // Number of client threads still running; RequestStopAndWait waits for it to reach zero.
    FThreadSafeCounter ActiveClients;
};
