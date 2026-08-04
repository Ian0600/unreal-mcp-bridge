#include "MCPServerRunnable.h"
#include "UnrealMCPBridgeModule.h"
#include "MCPProtocol.h"
#include "Async/Async.h"
#include "Async/Future.h"
#include "SocketSubsystem.h"
#include "SocketTypes.h"
#include "IPAddress.h"
#include "Sockets.h"
#include "Common/TcpSocketBuilder.h"

namespace
{
    // Max bytes pulled from the socket per Recv call. Larger reduces syscall overhead.
    constexpr int32 RecvChunkBytes = 4096;
    // Polling cadence for read/write readiness; smaller = faster Stop response, more CPU.
    constexpr double ReadPollMillis = 200.0;
    // Hard cap so SendResponse cannot wedge if the peer stops draining.
    constexpr double SendWaitSeconds = 5.0;
}

FMCPServerRunnable::FMCPServerRunnable(
    const FString& Host,
    int32 Port,
    int32 MaxLineBytes,
    int32 DispatchTimeoutSeconds,
    TSharedRef<FMCPCommandRegistry> Registry)
    : BindHost(Host)
    , BindPort(Port)
    , MaxLineBytesLimit(MaxLineBytes)
    , DispatchTimeoutSec(DispatchTimeoutSeconds)
    , CommandRegistry(Registry)
    , bStopRequested(false)
{
}

FMCPServerRunnable::~FMCPServerRunnable()
{
    // Final safety: resources should already be released via RequestStopAndWait/Exit.
    if (ListenSocket)
    {
        ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        if (SocketSubsystem)
        {
            SocketSubsystem->DestroySocket(ListenSocket);
        }
        ListenSocket = nullptr;
    }
}

bool FMCPServerRunnable::Init()
{
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!SocketSubsystem)
    {
        UE_LOG(LogMCPBridge, Error, TEXT("MCPServerRunnable: no socket subsystem available."));
        return false;
    }

    bool bIsValid = false;
    TSharedRef<FInternetAddr> BindAddr = SocketSubsystem->CreateInternetAddr();
    BindAddr->SetIp(*BindHost, bIsValid);
    if (!bIsValid)
    {
        UE_LOG(LogMCPBridge, Error, TEXT("MCPServerRunnable: invalid bind address '%s'."), *BindHost);
        return false;
    }
    BindAddr->SetPort(BindPort);

    ListenSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("MCPBridgeListener"), false);
    if (!ListenSocket)
    {
        UE_LOG(LogMCPBridge, Error, TEXT("MCPServerRunnable: failed to create listen socket."));
        return false;
    }

    ListenSocket->SetReuseAddr(true);
    ListenSocket->SetNonBlocking(false);

    if (!ListenSocket->Bind(*BindAddr))
    {
        UE_LOG(LogMCPBridge, Error, TEXT("MCPServerRunnable: bind failed on %s:%d."), *BindHost, BindPort);
        SocketSubsystem->DestroySocket(ListenSocket);
        ListenSocket = nullptr;
        return false;
    }

    if (!ListenSocket->Listen(1))
    {
        UE_LOG(LogMCPBridge, Error, TEXT("MCPServerRunnable: listen failed."));
        SocketSubsystem->DestroySocket(ListenSocket);
        ListenSocket = nullptr;
        return false;
    }

    UE_LOG(LogMCPBridge, Log, TEXT("MCPServerRunnable: listening on %s:%d."), *BindHost, BindPort);
    return true;
}

uint32 FMCPServerRunnable::Run()
{
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);

    while (!bStopRequested)
    {
        // Accept with a short poll interval so we notice stop requests promptly.
        bool bHasPendingConnection = false;
        if (!ListenSocket->WaitForPendingConnection(bHasPendingConnection, FTimespan::FromMilliseconds(100.0)))
        {
            // Socket error or closed — exit loop.
            break;
        }

        if (!bHasPendingConnection)
        {
            continue;
        }

        TSharedRef<FInternetAddr> ClientAddr = SocketSubsystem->CreateInternetAddr();
        FSocket* ClientSocket = ListenSocket->Accept(*ClientAddr, TEXT("MCPClient"));
        if (!ClientSocket)
        {
            UE_LOG(LogMCPBridge, Warning, TEXT("MCPServerRunnable: Accept returned null."));
            continue;
        }

        UE_LOG(LogMCPBridge, Log, TEXT("MCPServerRunnable: client connected from %s."),
            *ClientAddr->ToString(true));

        // Fresh receive buffer for each new client to avoid cross-session bleed.
        ReceiveBuffer.Reset();

        // Persistent per-client loop.
        while (!bStopRequested)
        {
            FString Line;
            bool bExceededLimit = false;

            if (!ReceiveLine(ClientSocket, Line, bExceededLimit))
            {
                if (bExceededLimit)
                {
                    // Send error and drop the connection.
                    MCPProtocol::FMCPResponse ErrResponse = MCPProtocol::MakeError(
                        nullptr,
                        MCPProtocol::FMCPError::InvalidRequest,
                        TEXT("Request line exceeded maximum allowed size"));
                    SendResponse(ClientSocket, MCPProtocol::SerializeResponse(ErrResponse));
                }
                UE_LOG(LogMCPBridge, Log, TEXT("MCPServerRunnable: client disconnected."));
                break;
            }

            if (Line.IsEmpty())
            {
                continue;
            }

            MCPProtocol::FMCPRequest Request;
            MCPProtocol::FMCPError ParseError;
            if (!MCPProtocol::ParseRequest(Line, Request, ParseError))
            {
                if (!Request.IsNotification())
                {
                    MCPProtocol::FMCPResponse ErrResponse = MCPProtocol::MakeError(
                        Request.Id, ParseError.Code, ParseError.Message);
                    SendResponse(ClientSocket, MCPProtocol::SerializeResponse(ErrResponse));
                }
                continue;
            }

            MCPProtocol::FMCPResponse Response = DispatchToGameThread(Request);

            // Notifications never receive a response.
            if (!Request.IsNotification())
            {
                SendResponse(ClientSocket, MCPProtocol::SerializeResponse(Response));
            }
        }

        SocketSubsystem->DestroySocket(ClientSocket);
    }

    return 0;
}

void FMCPServerRunnable::Stop()
{
    bStopRequested = true;
    // Close the listen socket to unblock WaitForPendingConnection.
    if (ListenSocket)
    {
        ListenSocket->Close();
    }
}

void FMCPServerRunnable::Exit()
{
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (ListenSocket && SocketSubsystem)
    {
        SocketSubsystem->DestroySocket(ListenSocket);
        ListenSocket = nullptr;
    }
    UE_LOG(LogMCPBridge, Log, TEXT("MCPServerRunnable: thread exited and resources released."));
}

void FMCPServerRunnable::StartThread()
{
    Thread = FRunnableThread::Create(this, TEXT("MCPBridgeListener"), 0, TPri_Normal);
    if (!Thread)
    {
        UE_LOG(LogMCPBridge, Error, TEXT("MCPServerRunnable: failed to create listener thread."));
    }
}

void FMCPServerRunnable::RequestStopAndWait()
{
    Stop();
    if (Thread)
    {
        Thread->WaitForCompletion();
        delete Thread;
        Thread = nullptr;
    }
}

bool FMCPServerRunnable::ReceiveLine(FSocket* ClientSocket, FString& OutLine, bool& bOutExceededLimit)
{
    bOutExceededLimit = false;
    const FTimespan PollWindow = FTimespan::FromMilliseconds(ReadPollMillis);

    while (!bStopRequested)
    {
        // Drain any complete line already sitting in the buffer before touching the socket.
        for (int32 i = 0; i < ReceiveBuffer.Num(); ++i)
        {
            if (ReceiveBuffer[i] == '\n')
            {
                TArray<uint8> LineBytes;
                LineBytes.Append(ReceiveBuffer.GetData(), i);
                LineBytes.Add(0);
                OutLine = FString(UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(LineBytes.GetData())));
                OutLine.TrimEndInline();

                TArray<uint8> Remaining;
                const int32 LeftoverCount = ReceiveBuffer.Num() - (i + 1);
                if (LeftoverCount > 0)
                {
                    Remaining.Append(ReceiveBuffer.GetData() + i + 1, LeftoverCount);
                }
                ReceiveBuffer = MoveTemp(Remaining);
                return true;
            }
        }

        if (ReceiveBuffer.Num() > MaxLineBytesLimit)
        {
            bOutExceededLimit = true;
            return false;
        }

        // Poll for read readiness with a short window so we can react to Stop quickly
        // and never block indefinitely on a quiet socket.
        if (!ClientSocket->Wait(ESocketWaitConditions::WaitForRead, PollWindow))
        {
            // No data within window. Verify the connection is still up; if not, bail out.
            const ESocketConnectionState State = ClientSocket->GetConnectionState();
            if (State != SCS_Connected)
            {
                return false;
            }
            continue;
        }

        uint8 Chunk[RecvChunkBytes];
        int32 BytesRead = 0;
        if (!ClientSocket->Recv(Chunk, sizeof(Chunk), BytesRead))
        {
            return false;
        }
        if (BytesRead == 0)
        {
            // Peer closed gracefully.
            return false;
        }

        ReceiveBuffer.Append(Chunk, BytesRead);
    }

    return false;
}

bool FMCPServerRunnable::SendResponse(FSocket* ClientSocket, const FString& Serialized)
{
    // Bound the send so a stalled peer cannot wedge the worker thread.
    if (!ClientSocket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromSeconds(SendWaitSeconds)))
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("MCPServerRunnable: socket not writable within timeout."));
        return false;
    }

    FTCHARToUTF8 Converter(*Serialized);
    const uint8* Data = reinterpret_cast<const uint8*>(Converter.Get());
    const int32 Length = Converter.Length();

    int32 BytesSent = 0;
    bool bOk = ClientSocket->Send(Data, Length, BytesSent);
    if (!bOk || BytesSent != Length)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("MCPServerRunnable: failed to send response body."));
        return false;
    }

    const uint8 Newline = '\n';
    bOk = ClientSocket->Send(&Newline, 1, BytesSent);
    if (!bOk)
    {
        UE_LOG(LogMCPBridge, Warning, TEXT("MCPServerRunnable: failed to send response newline."));
        return false;
    }
    return true;
}

MCPProtocol::FMCPResponse FMCPServerRunnable::DispatchToGameThread(const MCPProtocol::FMCPRequest& Request)
{
    // Capture by value what the lambda needs; avoid referencing stack locals after timeout.
    FString Method   = Request.Method;
    TSharedPtr<FJsonObject> Params = Request.Params;
    TSharedPtr<FJsonValue>  Id     = Request.Id;
    TSharedRef<FMCPCommandRegistry> Registry = CommandRegistry;

    TPromise<MCPProtocol::FMCPResponse> Promise;
    TFuture<MCPProtocol::FMCPResponse>  Future = Promise.GetFuture();

    AsyncTask(ENamedThreads::GameThread, [Method, Params, Id, Registry, Promise = MoveTemp(Promise)]() mutable
    {
        TSharedPtr<FJsonObject> Result;
        MCPProtocol::FMCPError DispatchError;

        if (Registry->Dispatch(Method, Params, Result, DispatchError))
        {
            Promise.SetValue(MCPProtocol::MakeResult(Id, Result));
        }
        else
        {
            // Forward the handler's optional diagnostic payload too. MakeError only takes
            // code and message, so rebuilding through it would drop Data on the floor —
            // PROTOCOL.md documents "data" on error responses, and handlers rely on it to
            // explain partial failures (which assets were skipped, and why).
            MCPProtocol::FMCPResponse ErrorResponse = MCPProtocol::MakeError(Id, DispatchError.Code, DispatchError.Message);
            ErrorResponse.Error->Data = DispatchError.Data;
            Promise.SetValue(MoveTemp(ErrorResponse));
        }
    });

    const FTimespan Timeout = FTimespan::FromSeconds(static_cast<double>(DispatchTimeoutSec));
    if (Future.WaitFor(Timeout))
    {
        return Future.Get();
    }

    UE_LOG(LogMCPBridge, Warning,
        TEXT("MCPServerRunnable: GameThread dispatch timed out for method '%s'."), *Method);
    return MCPProtocol::MakeError(Id,
        MCPProtocol::FMCPError::InternalError,
        TEXT("GameThread dispatch timed out"));
}
