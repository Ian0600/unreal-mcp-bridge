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
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
    // Max bytes pulled from the socket per Recv call. Larger reduces syscall overhead.
    constexpr int32 RecvChunkBytes = 4096;
    // Polling cadence for read/write readiness; smaller = faster Stop response, more CPU.
    constexpr double ReadPollMillis = 200.0;
    // Hard cap so SendResponse cannot wedge if the peer stops draining.
    constexpr double SendWaitSeconds = 5.0;

    // GameThread 에서 지금 돌고 있는 명령. 타임아웃 때 "명령이 시작조차 못 했는지(앞 명령이 막고 있음)"
    // 와 "시작해서 안 끝나는지(모달 대기 등)"를 가려 알려 주려고 둔다. 리스너가 여럿이라 전역이다.
    FCriticalSection GInFlightLock;
    FString GInFlightMethod;
    double GInFlightStartSeconds = 0.0;

    // reload_packages 같은 호출이 확인 창을 띄우면 GameThread 는 그 창의 모달 루프에서 사용자를 기다리고,
    // 클라이언트는 "timed out" 만 받았다(2026-10-19 애니 팀 요청). Slate 모달 루프는 틱마다
    // ModalLoopTickEvent 를 쏘므로 거기서(GameThread) 모달 창 제목과 시각을 적어 두고, 타임아웃 때 읽는다.
    // 창 HWND 로는 못 가린다 — Slate 모달은 주인 창을 비활성화하지 않는다(시험 확인).
    FString GModalTitle;            // GInFlightLock 으로 보호
    double GLastModalTickSeconds = 0.0;
    int32 GModalHookRefs = 0;       // 리스너마다 잡고 놓는다. GameThread 에서만 건드린다.
    FDelegateHandle GModalTickHandle;

    void AcquireModalHook()
    {
        if (GModalHookRefs++ == 0 && FSlateApplication::IsInitialized())
        {
            GModalTickHandle = FSlateApplication::Get().GetOnModalLoopTickEvent().AddLambda([](float)
            {
                FString Title;
                if (const TSharedPtr<SWindow> Modal = FSlateApplication::Get().GetActiveModalWindow())
                {
                    Title = Modal->GetTitle().ToString();
                }
                FScopeLock Lock(&GInFlightLock);
                GModalTitle = Title;
                GLastModalTickSeconds = FPlatformTime::Seconds();
            });
        }
    }

    void ReleaseModalHook()
    {
        if (--GModalHookRefs == 0 && GModalTickHandle.IsValid())
        {
            if (FSlateApplication::IsInitialized())
            {
                FSlateApplication::Get().GetOnModalLoopTickEvent().Remove(GModalTickHandle);
            }
            GModalTickHandle.Reset();
        }
    }

    // 에디터 프로세스의 보이는 최상위 창 목록 — 무엇이 떠 있는지 같이 돌려준다. Win32 대화상자(#32770,
    // 네이티브 MessageBox)는 Slate 모달 루프를 안 거치므로 여기서 모달로 잡는다.
    //
    // 리스너 스레드에서 부르므로 GameThread 로 메시지를 보내는 API 는 쓰지 않는다 — GetWindowText 는
    // WM_GETTEXT 를 보내 GameThread 가 바쁘면 여기까지 멈춘다. InternalGetWindowText 는 보내지 않는다.
    TSharedPtr<FJsonValue> CollectProcessWindows(const FString& SlateModalTitle, bool& bOutModalSuspected, FString& OutModalTitle)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
#if PLATFORM_WINDOWS
        struct FEnumState
        {
            DWORD ProcessId;
            TArray<HWND> Windows;
        } State{ ::GetCurrentProcessId(), {} };

        ::EnumWindows([](HWND Hwnd, LPARAM Param) -> BOOL
        {
            FEnumState* S = reinterpret_cast<FEnumState*>(Param);
            DWORD Pid = 0;
            ::GetWindowThreadProcessId(Hwnd, &Pid);
            if (Pid == S->ProcessId && ::IsWindowVisible(Hwnd))
            {
                S->Windows.Add(Hwnd);
            }
            return 1;   // 계속 열거 (HideWindowsPlatformTypes 뒤라 TRUE 매크로가 없다)
        }, reinterpret_cast<LPARAM>(&State));

        for (HWND Hwnd : State.Windows)
        {
            WCHAR Title[256] = { 0 };
            WCHAR ClassName[128] = { 0 };
            ::InternalGetWindowText(Hwnd, Title, UE_ARRAY_COUNT(Title));
            ::GetClassNameW(Hwnd, ClassName, UE_ARRAY_COUNT(ClassName));
            const bool bEnabled = ::IsWindowEnabled(Hwnd) != 0;
            const HWND Owner = ::GetWindow(Hwnd, GW_OWNER);
            const bool bLooksModal = FCString::Strcmp(ClassName, TEXT("#32770")) == 0
                || (bOutModalSuspected && !SlateModalTitle.IsEmpty() && SlateModalTitle == Title);

            auto Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("title"), Title);
            Obj->SetStringField(TEXT("class"), ClassName);
            Obj->SetBoolField(TEXT("enabled"), bEnabled);
            Obj->SetBoolField(TEXT("owned"), Owner != nullptr);
            Obj->SetBoolField(TEXT("looks_modal"), bLooksModal);
            Out.Add(MakeShared<FJsonValueObject>(Obj));

            if (bLooksModal && OutModalTitle.IsEmpty())
            {
                bOutModalSuspected = true;
                OutModalTitle = Title;
            }
        }
#endif
        return MakeShared<FJsonValueArray>(Out);
    }
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
    AcquireModalHook();
}

FMCPServerRunnable::~FMCPServerRunnable()
{
    ReleaseModalHook();

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

        // 모달 루프 안에서 다른 명령이 다시 들어올 수 있어(중첩) 이전 값을 저장했다가 되돌린다.
        FString PrevMethod;
        double PrevStart = 0.0;
        {
            FScopeLock Lock(&GInFlightLock);
            PrevMethod = GInFlightMethod;
            PrevStart = GInFlightStartSeconds;
            GInFlightMethod = Method;
            GInFlightStartSeconds = FPlatformTime::Seconds();
        }
        ON_SCOPE_EXIT
        {
            FScopeLock Lock(&GInFlightLock);
            GInFlightMethod = PrevMethod;
            GInFlightStartSeconds = PrevStart;
        };

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

    FString InFlightMethod;
    double InFlightSeconds = 0.0;
    bool bModalSuspected = false;
    FString ModalTitle;
    {
        FScopeLock Lock(&GInFlightLock);
        const double Now = FPlatformTime::Seconds();
        InFlightMethod = GInFlightMethod;
        if (!InFlightMethod.IsEmpty())
        {
            InFlightSeconds = Now - GInFlightStartSeconds;
        }
        // 모달 루프는 프레임마다 틱한다 — 1초 안에 틱이 있었으면 지금 모달이 떠 있다.
        if (Now - GLastModalTickSeconds < 1.0)
        {
            bModalSuspected = true;
            ModalTitle = GModalTitle;
        }
    }
    const FString SlateModalTitle = ModalTitle;
    TSharedPtr<FJsonValue> Windows = CollectProcessWindows(SlateModalTitle, bModalSuspected, ModalTitle);

    FString Message = TEXT("GameThread dispatch timed out");
    if (bModalSuspected)
    {
        Message += FString::Printf(TEXT(" — waiting on a modal window \"%s\" (close it in the editor; the command may then finish on its own)"), *ModalTitle);
    }
    else if (InFlightMethod.IsEmpty())
    {
        Message += TEXT(" — the GameThread did not pick the command up (busy: loading, compiling, or a long tick)");
    }
    else if (InFlightMethod != Method)
    {
        Message += FString::Printf(TEXT(" — still running an earlier command '%s' (%.1fs)"), *InFlightMethod, InFlightSeconds);
    }
    else
    {
        Message += FString::Printf(TEXT(" — '%s' started but has not returned (%.1fs)"), *Method, InFlightSeconds);
    }

    UE_LOG(LogMCPBridge, Warning, TEXT("MCPServerRunnable: %s (method '%s')."), *Message, *Method);

    auto Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("method"), Method);
    Data->SetNumberField(TEXT("timeout_seconds"), DispatchTimeoutSec);
    Data->SetStringField(TEXT("in_flight_method"), InFlightMethod);
    Data->SetNumberField(TEXT("in_flight_seconds"), InFlightSeconds);
    Data->SetBoolField(TEXT("modal_suspected"), bModalSuspected);
    Data->SetStringField(TEXT("modal_title"), ModalTitle);
    Data->SetField(TEXT("windows"), Windows);

    MCPProtocol::FMCPResponse ErrorResponse = MCPProtocol::MakeError(Id, MCPProtocol::FMCPError::InternalError, Message);
    ErrorResponse.Error->Data = MakeShared<FJsonValueObject>(Data);
    return ErrorResponse;
}
