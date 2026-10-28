#include "Commands/DebugRunCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "Commands/SceneTools.h"
#include "MCPCommandRegistry.h"
#include "UnrealMCPBridgeModule.h"
#include "CollisionQueryParams.h"
#include "Components/PrimitiveComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/CollisionProfile.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/WorldSettings.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/StringOutputDevice.h"
#include "Misc/AutomationTest.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

namespace MCPDebugRun
{
    using UnrealMCPBridge::Json::RequireString;

    void Fail(MCPProtocol::FMCPError& OutError, const FString& Message, int32 Code = MCPProtocol::FMCPError::InvalidParams)
    {
        OutError.Code = Code;
        OutError.Message = Message;
    }

    /**
     * Keeps the last lines of the editor log with a running sequence number, so a caller can ask
     * "what was logged since cursor N" — the log file itself is shared with every other writer and
     * has no cursor. Log lines arrive on any thread, hence the lock.
     */
    class FLogCapture : public FOutputDevice
    {
    public:
        struct FLine
        {
            int64 Seq = 0;
            double Seconds = 0.0;
            FName Category;
            ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
            FString Text;
        };

        static constexpr int32 Capacity = 20000;

        FLogCapture()
        {
            Lines.SetNum(Capacity);
        }

        virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override
        {
            FScopeLock Lock(&Mutex);
            FLine& Line = Lines[NextSeq % Capacity];
            Line.Seq = NextSeq++;
            Line.Seconds = FPlatformTime::Seconds();
            Line.Category = Category;
            Line.Verbosity = static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
            Line.Text = Text;
        }

        virtual bool CanBeUsedOnAnyThread() const override { return true; }
        virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

        int64 GetNextSeq() const
        {
            FScopeLock Lock(&Mutex);
            return NextSeq;
        }

        /** Lines with Seq >= Since that pass the filter, oldest first, up to Max. */
        void Read(int64 Since, const FString& Contains, const FString& Category, ELogVerbosity::Type MaxVerbosity, int32 Max,
            TArray<FLine>& Out, int64& OutOldest, int64& OutNext) const
        {
            FScopeLock Lock(&Mutex);
            OutNext = NextSeq;
            OutOldest = FMath::Max<int64>(0, NextSeq - Capacity);
            for (int64 Seq = FMath::Max(Since, OutOldest); Seq < NextSeq; ++Seq)
            {
                const FLine& Line = Lines[Seq % Capacity];
                if (Line.Verbosity > MaxVerbosity) continue;
                if (!Category.IsEmpty() && !Line.Category.ToString().Equals(Category, ESearchCase::IgnoreCase)) continue;
                if (!Contains.IsEmpty() && !Line.Text.Contains(Contains, ESearchCase::IgnoreCase)) continue;
                Out.Add(Line);
                if (Out.Num() >= Max)
                {
                    // Stop here so the caller's next cursor continues right after the last returned line.
                    OutNext = Seq + 1;
                    return;
                }
            }
        }

    private:
        mutable FCriticalSection Mutex;
        TArray<FLine> Lines;
        int64 NextSeq = 0;
    };

    TUniquePtr<FLogCapture>& LogCapture()
    {
        static TUniquePtr<FLogCapture> Capture;
        return Capture;
    }

    ELogVerbosity::Type ParseVerbosity(const FString& Name)
    {
        if (Name.IsEmpty()) return ELogVerbosity::VeryVerbose;
        if (Name.Equals(TEXT("Fatal"), ESearchCase::IgnoreCase))   return ELogVerbosity::Fatal;
        if (Name.Equals(TEXT("Error"), ESearchCase::IgnoreCase))   return ELogVerbosity::Error;
        if (Name.Equals(TEXT("Warning"), ESearchCase::IgnoreCase)) return ELogVerbosity::Warning;
        if (Name.Equals(TEXT("Display"), ESearchCase::IgnoreCase)) return ELogVerbosity::Display;
        if (Name.Equals(TEXT("Log"), ESearchCase::IgnoreCase))     return ELogVerbosity::Log;
        if (Name.Equals(TEXT("Verbose"), ESearchCase::IgnoreCase)) return ELogVerbosity::Verbose;
        return ELogVerbosity::VeryVerbose;
    }

    TArray<TSharedPtr<FJsonValue>> LinesToJson(const TArray<FLogCapture::FLine>& Lines)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const FLogCapture::FLine& Line : Lines)
        {
            auto Obj = MakeShared<FJsonObject>();
            Obj->SetNumberField(TEXT("seq"), static_cast<double>(Line.Seq));
            Obj->SetStringField(TEXT("category"), Line.Category.ToString());
            Obj->SetStringField(TEXT("verbosity"), ToString(Line.Verbosity));
            Obj->SetStringField(TEXT("text"), Line.Text);
            Values.Add(MakeShared<FJsonValueObject>(Obj));
        }
        return Values;
    }

    /** pie.log_mark name -> the cursor of its mark line. Handlers run on the GameThread only. */
    TMap<FString, int64>& LogMarks()
    {
        static TMap<FString, int64> Marks;
        return Marks;
    }

    /** "Visibility" / "ECC_Visibility" / a project trace channel's display name -> channel. */
    bool ParseTraceChannel(const FString& Name, ECollisionChannel& Out)
    {
        if (Name.IsEmpty())
        {
            Out = ECC_Visibility;
            return true;
        }
        const UEnum* Enum = StaticEnum<ECollisionChannel>();
        const int64 ByName = Enum->GetValueByNameString(Name.StartsWith(TEXT("ECC_")) ? Name : TEXT("ECC_") + Name);
        if (ByName != INDEX_NONE)
        {
            Out = static_cast<ECollisionChannel>(ByName);
            return true;
        }
        if (const UCollisionProfile* Profile = UCollisionProfile::Get())
        {
            for (int32 Index = 0; Index < ECC_MAX; ++Index)
            {
                if (Profile->ReturnChannelNameFromContainerIndex(Index).ToString().Equals(Name, ESearchCase::IgnoreCase))
                {
                    Out = static_cast<ECollisionChannel>(Index);
                    return true;
                }
            }
        }
        return false;
    }

    FString ChannelName(ECollisionChannel Channel)
    {
        if (const UCollisionProfile* Profile = UCollisionProfile::Get())
        {
            const FName Name = Profile->ReturnChannelNameFromContainerIndex(static_cast<int32>(Channel));
            if (!Name.IsNone())
            {
                return Name.ToString();
            }
        }
        return StaticEnum<ECollisionChannel>()->GetNameStringByValue(Channel);
    }

    bool ReadPointParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FVector& Out)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Params.IsValid() || !Params->TryGetArrayField(Field, Values) || !Values || Values->Num() != 3)
        {
            return false;
        }
        Out = FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> VectorJson(const FVector& V)
    {
        return { MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z) };
    }

    TSharedPtr<FJsonObject> HitJson(const FHitResult& Hit, const FVector& From, ECollisionChannel Channel)
    {
        auto Obj = MakeShared<FJsonObject>();
        const AActor* Actor = Hit.GetActor();
        const UPrimitiveComponent* Component = Hit.GetComponent();
        Obj->SetStringField(TEXT("actor"), Actor ? Actor->GetActorNameOrLabel() : FString());
        Obj->SetStringField(TEXT("actor_class"), Actor ? Actor->GetClass()->GetName() : FString());
        Obj->SetStringField(TEXT("actor_path"), Actor ? Actor->GetPathName() : FString());
        Obj->SetStringField(TEXT("component"), Component ? Component->GetName() : FString());
        Obj->SetStringField(TEXT("component_class"), Component ? Component->GetClass()->GetName() : FString());
        Obj->SetStringField(TEXT("collision_profile"), Component ? Component->GetCollisionProfileName().ToString() : FString());
        Obj->SetStringField(TEXT("response"), Component
            ? StaticEnum<ECollisionResponse>()->GetNameStringByValue(Component->GetCollisionResponseToChannel(Channel))
            : FString());
        Obj->SetStringField(TEXT("bone"), Hit.BoneName.ToString());
        Obj->SetArrayField(TEXT("location"), VectorJson(Hit.ImpactPoint));
        Obj->SetNumberField(TEXT("distance"), FVector::Dist(From, Hit.ImpactPoint));
        Obj->SetBoolField(TEXT("blocking"), Hit.bBlockingHit);
        return Obj;
    }
}

FDebugRunCommandHandler::~FDebugRunCommandHandler()
{
    if (MCPDebugRun::LogCapture().IsValid())
    {
        if (GLog)
        {
            GLog->RemoveOutputDevice(MCPDebugRun::LogCapture().Get());
        }
        MCPDebugRun::LogCapture().Reset();
    }
}

void FDebugRunCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    using namespace MCPDebugRun;

    if (!LogCapture().IsValid() && GLog)
    {
        LogCapture() = MakeUnique<FLogCapture>();
        GLog->AddOutputDevice(LogCapture().Get());
    }

    // pie.exec_console — runs a console command and returns what it printed. Params: command
    // (required), world ("pie" = the PIE player's controller, so Exec cheats and the cheat manager
    // answer; "editor" = the editor world; default pie when PIE runs, else editor), player
    // (PIE player index, default 0). Returns { world, output, log_lines (what the log got while
    // the command ran), log_cursor (next pie.read_log since) }.
    // Note: IFSR's CVar cheats also write Config/DefaultDebug.ini when toggled.
    Registry.Register(TEXT("pie.exec_console"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString Command;
            if (!RequireString(Params, TEXT("command"), Command, OutError)) return nullptr;
            double PlayerIndex = 0.0;
            Params->TryGetNumberField(TEXT("player"), PlayerIndex);
            if (!GEditor)
            {
                Fail(OutError, TEXT("GEditor is not available"), MCPProtocol::FMCPError::InternalError);
                return nullptr;
            }

            bool bUsePie = false;
            UWorld* PlayWorld = UnrealMCPBridge::Scene::ResolveWorld(Params, UnrealMCPBridge::Scene::EDefaultWorld::PieIfRunning, bUsePie, OutError);
            if (!PlayWorld)
            {
                return nullptr;
            }

            const int64 LogBefore = LogCapture().IsValid() ? LogCapture()->GetNextSeq() : 0;
            FString Output;
            if (bUsePie)
            {
                APlayerController* Controller = UGameplayStatics::GetPlayerController(PlayWorld, static_cast<int32>(PlayerIndex));
                if (!Controller)
                {
                    Fail(OutError, FString::Printf(TEXT("PIE has no player controller %d"), static_cast<int32>(PlayerIndex)));
                    return nullptr;
                }
                Output = Controller->ConsoleCommand(Command, /*bWriteToLog=*/true);
            }
            else
            {
                FStringOutputDevice Device;
                GEditor->Exec(GEditor->GetEditorWorldContext().World(), *Command, Device);
                Output = Device;
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("world"), bUsePie ? TEXT("pie") : TEXT("editor"));
            Result->SetStringField(TEXT("command"), Command);
            Result->SetStringField(TEXT("output"), Output);
            if (LogCapture().IsValid())
            {
                TArray<FLogCapture::FLine> Lines;
                int64 Oldest = 0, Next = 0;
                LogCapture()->Read(LogBefore, FString(), FString(), ELogVerbosity::VeryVerbose, 500, Lines, Oldest, Next);
                Result->SetArrayField(TEXT("log_lines"), LinesToJson(Lines));
                Result->SetNumberField(TEXT("log_cursor"), static_cast<double>(Next));
            }
            return Result;
        });

    // pie.read_log — read-only. The editor log as captured by the bridge since it started (last
    // 20000 lines). Params: since (cursor from a previous call or pie.exec_console; omit = the last
    // max lines), contains (substring), category (exact, e.g. LogIFSRQuest), verbosity (most
    // verbose level to keep: Error / Warning / Display / Log / Verbose), max (default 200).
    // Returns { lines: [{seq, category, verbosity, text}], next (pass as since), dropped (lines
    // that scrolled out of the buffer before they were read) }.
    Registry.Register(TEXT("pie.read_log"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            if (!LogCapture().IsValid())
            {
                Fail(OutError, TEXT("Log capture is not running"), MCPProtocol::FMCPError::InternalError);
                return nullptr;
            }
            FString Contains, Category, Verbosity;
            double Since = -1.0, MaxLines = 200.0;
            if (Params.IsValid())
            {
                Params->TryGetStringField(TEXT("contains"), Contains);
                Params->TryGetStringField(TEXT("category"), Category);
                Params->TryGetStringField(TEXT("verbosity"), Verbosity);
                Params->TryGetNumberField(TEXT("since"), Since);
                Params->TryGetNumberField(TEXT("max"), MaxLines);
                FString SinceMark;
                if (Params->TryGetStringField(TEXT("since_mark"), SinceMark) && !SinceMark.IsEmpty())
                {
                    const int64* MarkSeq = LogMarks().Find(SinceMark);
                    if (!MarkSeq)
                    {
                        Fail(OutError, FString::Printf(TEXT("No log mark named '%s' (pie.log_mark first)"), *SinceMark));
                        return nullptr;
                    }
                    Since = static_cast<double>(*MarkSeq);
                }
            }
            const int32 Max = FMath::Clamp(static_cast<int32>(MaxLines), 1, 5000);

            int64 From = static_cast<int64>(Since);
            if (Since < 0.0)
            {
                // Tail mode: start far enough back to return the last Max matching lines. Without
                // a filter that is exactly Max lines back; with one, scan the whole buffer and keep the end.
                From = 0;
            }

            TArray<FLogCapture::FLine> Lines;
            int64 Oldest = 0, Next = 0;
            const bool bTail = Since < 0.0;
            LogCapture()->Read(From, Contains, Category, ParseVerbosity(Verbosity), bTail ? FLogCapture::Capacity : Max, Lines, Oldest, Next);
            if (bTail && Lines.Num() > Max)
            {
                Lines.RemoveAt(0, Lines.Num() - Max);
                Next = LogCapture()->GetNextSeq();
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("lines"), LinesToJson(Lines));
            Result->SetNumberField(TEXT("next"), static_cast<double>(Next));
            Result->SetNumberField(TEXT("dropped"), static_cast<double>(bTail ? 0 : FMath::Max<int64>(0, Oldest - From)));
            return Result;
        });

    // pie.log_mark — writes "MCP mark: <name>" to the log and remembers its cursor, so a test can
    // later read "everything since round 3 started" with pie.read_log since_mark=<name> instead of
    // keeping cursors itself. Marks live until the editor closes; reusing a name moves it.
    // Params: name (required). Returns { name, cursor }.
    Registry.Register(TEXT("pie.log_mark"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString Name;
            if (!RequireString(Params, TEXT("name"), Name, OutError)) return nullptr;
            const int64 Cursor = LogCapture().IsValid() ? LogCapture()->GetNextSeq() : 0;
            UE_LOG(LogMCPBridge, Display, TEXT("MCP mark: %s"), *Name);
            LogMarks().Add(Name, Cursor);

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("name"), Name);
            Result->SetNumberField(TEXT("cursor"), static_cast<double>(Cursor));
            return Result;
        });

    // pie.status — read-only. Whether PIE runs and whether it is ready to drive: the world has
    // begun play and player 0 possesses a pawn. pie.start only queues the session, so a caller
    // polls this (the python pie_start(wait_ready=True) does). Params: player (default 0).
    // Returns { running, world, map, begun_play, game_seconds, real_seconds, paused, time_dilation,
    // has_controller, pawn, pawn_class, pawn_location, ready }.
    Registry.Register(TEXT("pie.status"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& /*OutError*/) -> TSharedPtr<FJsonObject>
        {
            double PlayerIndex = 0.0;
            if (Params.IsValid())
            {
                Params->TryGetNumberField(TEXT("player"), PlayerIndex);
            }
            UWorld* PlayWorld = GEditor ? GEditor->PlayWorld.Get() : nullptr;

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("running"), PlayWorld != nullptr);
            if (!PlayWorld)
            {
                Result->SetBoolField(TEXT("ready"), false);
                return Result;
            }
            Result->SetStringField(TEXT("world"), PlayWorld->GetName());
            Result->SetStringField(TEXT("map"), PlayWorld->GetMapName());
            Result->SetBoolField(TEXT("begun_play"), PlayWorld->HasBegunPlay());
            Result->SetNumberField(TEXT("game_seconds"), PlayWorld->GetTimeSeconds());
            Result->SetNumberField(TEXT("real_seconds"), PlayWorld->GetRealTimeSeconds());
            Result->SetBoolField(TEXT("paused"), PlayWorld->IsPaused());
            Result->SetNumberField(TEXT("time_dilation"), PlayWorld->GetWorldSettings() ? PlayWorld->GetWorldSettings()->GetEffectiveTimeDilation() : 1.0);

            APlayerController* Controller = UGameplayStatics::GetPlayerController(PlayWorld, static_cast<int32>(PlayerIndex));
            const APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
            Result->SetBoolField(TEXT("has_controller"), Controller != nullptr);
            Result->SetStringField(TEXT("controller_class"), Controller ? Controller->GetClass()->GetName() : FString());
            Result->SetStringField(TEXT("pawn"), Pawn ? Pawn->GetActorNameOrLabel() : FString());
            Result->SetStringField(TEXT("pawn_class"), Pawn ? Pawn->GetClass()->GetName() : FString());
            if (Pawn)
            {
                Result->SetArrayField(TEXT("pawn_location"), VectorJson(Pawn->GetActorLocation()));
            }
            Result->SetBoolField(TEXT("ready"), PlayWorld->HasBegunPlay() && Pawn != nullptr);
            return Result;
        });

    // debug.line_of_sight — read-only (draws a debug line). Traces from -> to and reports what
    // blocks it first, and what it touched on the way. Params: from, to ([x,y,z], required),
    // channel ("Visibility" default, "Camera", "ECC_*" or a project trace channel name),
    // world ("pie" default when PIE runs, else "editor"), complex (bool, default false),
    // ignore (array of actor names/labels to skip), ignore_player (bool, default true in PIE:
    // skip player 0's pawn — a trace from its eyes otherwise starts inside its own capsule),
    // draw_seconds (default 3, 0 = no drawing).
    // Returns { world, channel, blocked, hit (first blocking hit or null), touches: [...] }.
    Registry.Register(TEXT("debug.line_of_sight"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FVector From, To;
            if (!ReadPointParam(Params, TEXT("from"), From) || !ReadPointParam(Params, TEXT("to"), To))
            {
                Fail(OutError, TEXT("from and to ([x, y, z]) are required"));
                return nullptr;
            }
            FString ChannelText;
            Params->TryGetStringField(TEXT("channel"), ChannelText);
            ECollisionChannel Channel = ECC_Visibility;
            if (!ParseTraceChannel(ChannelText, Channel))
            {
                Fail(OutError, FString::Printf(TEXT("Unknown channel '%s'"), *ChannelText));
                return nullptr;
            }
            bool bIsPie = false;
            UWorld* World = UnrealMCPBridge::Scene::ResolveWorld(Params, UnrealMCPBridge::Scene::EDefaultWorld::PieIfRunning, bIsPie, OutError);
            if (!World) return nullptr;

            bool bComplex = false;
            Params->TryGetBoolField(TEXT("complex"), bComplex);
            double DrawSeconds = 3.0;
            Params->TryGetNumberField(TEXT("draw_seconds"), DrawSeconds);

            FCollisionQueryParams Query(SCENE_QUERY_STAT(MCPLineOfSight), bComplex);
            bool bIgnorePlayer = bIsPie;
            Params->TryGetBoolField(TEXT("ignore_player"), bIgnorePlayer);
            if (bIgnorePlayer)
            {
                if (APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(World, 0))
                {
                    Query.AddIgnoredActor(PlayerPawn);
                }
            }
            const TArray<TSharedPtr<FJsonValue>>* IgnoreValues = nullptr;
            if (Params->TryGetArrayField(TEXT("ignore"), IgnoreValues) && IgnoreValues)
            {
                TSet<FString> IgnoreNames;
                for (const TSharedPtr<FJsonValue>& Value : *IgnoreValues)
                {
                    IgnoreNames.Add(Value->AsString());
                }
                for (TActorIterator<AActor> It(World); It; ++It)
                {
                    if (IgnoreNames.Contains(It->GetName()) || IgnoreNames.Contains(It->GetActorNameOrLabel()))
                    {
                        Query.AddIgnoredActor(*It);
                    }
                }
            }

            TArray<FHitResult> Hits;
            World->LineTraceMultiByChannel(Hits, From, To, Channel, Query);
            const FHitResult* Block = Hits.FindByPredicate([](const FHitResult& Hit) { return Hit.bBlockingHit; });

            TArray<TSharedPtr<FJsonValue>> Touches;
            for (const FHitResult& Hit : Hits)
            {
                if (!Hit.bBlockingHit)
                {
                    Touches.Add(MakeShared<FJsonValueObject>(HitJson(Hit, From, Channel)));
                }
            }

            if (DrawSeconds > 0.0)
            {
                const FVector End = Block ? FVector(Block->ImpactPoint) : To;
                DrawDebugLine(World, From, End, FColor::Green, false, DrawSeconds, 0, 2.f);
                if (Block)
                {
                    DrawDebugLine(World, End, To, FColor::Red, false, DrawSeconds, 0, 1.f);
                    DrawDebugPoint(World, End, 14.f, FColor::Red, false, DrawSeconds);
                }
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("world"), bIsPie ? TEXT("pie") : TEXT("editor"));
            Result->SetStringField(TEXT("channel"), ChannelName(Channel));
            Result->SetBoolField(TEXT("blocked"), Block != nullptr);
            if (Block)
            {
                Result->SetObjectField(TEXT("hit"), HitJson(*Block, From, Channel));
            }
            else
            {
                Result->SetField(TEXT("hit"), MakeShared<FJsonValueNull>());
            }
            Result->SetArrayField(TEXT("touches"), Touches);
            Result->SetNumberField(TEXT("length"), FVector::Dist(From, To));
            return Result;
        });

    // automation.run_test — runs registered automation tests whose full path starts with filter
    // (e.g. "IFSR.Quest", "IFSR.Quest.Validation.ProjectData"), one after another in this call.
    // Params: filter (required), max_tests (default 50), latent_seconds (time each test may spend
    // on latent commands, default 5 — tests that wait on frames cannot finish inside one call and
    // are reported as latent_timeout). Returns { tests: [{name, ok, errors, warnings, seconds,
    // messages: [{type, message}]}], passed, failed }.
    Registry.Register(TEXT("automation.run_test"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString Filter;
            if (!RequireString(Params, TEXT("filter"), Filter, OutError)) return nullptr;
            double MaxTests = 50.0, LatentSeconds = 5.0;
            Params->TryGetNumberField(TEXT("max_tests"), MaxTests);
            Params->TryGetNumberField(TEXT("latent_seconds"), LatentSeconds);

            FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
            // GetValidTestNames only lists tests matching the requested filter, which defaults to
            // smoke tests. Widen it for the listing; the Session Frontend sets its own on each request.
            Framework.SetRequestedTestFilter(EAutomationTestFlags_FilterMask);
            TArray<FAutomationTestInfo> AllTests;
            Framework.GetValidTestNames(AllTests);
            Framework.SetRequestedTestFilter(EAutomationTestFlags::SmokeFilter);

            TArray<FAutomationTestInfo> Selected;
            for (const FAutomationTestInfo& Info : AllTests)
            {
                if (Info.GetFullTestPath().StartsWith(Filter, ESearchCase::IgnoreCase))
                {
                    Selected.Add(Info);
                }
            }
            if (Selected.Num() == 0)
            {
                Fail(OutError, FString::Printf(TEXT("No automation test path starts with '%s'"), *Filter));
                return nullptr;
            }
            if (Selected.Num() > static_cast<int32>(MaxTests))
            {
                Fail(OutError, FString::Printf(TEXT("'%s' matches %d tests, more than max_tests=%d — narrow the filter"), *Filter, Selected.Num(), static_cast<int32>(MaxTests)));
                return nullptr;
            }

            int32 Passed = 0, Failed = 0;
            TArray<TSharedPtr<FJsonValue>> Tests;
            for (const FAutomationTestInfo& Info : Selected)
            {
                const double Start = FPlatformTime::Seconds();
                Framework.StartTestByName(Info.GetTestName(), /*InRoleIndex=*/0, Info.GetFullTestPath());

                bool bLatentDone = Framework.ExecuteLatentCommands();
                while (!bLatentDone && FPlatformTime::Seconds() - Start < LatentSeconds)
                {
                    bLatentDone = Framework.ExecuteLatentCommands();
                }
                if (!bLatentDone)
                {
                    Framework.DequeueAllCommands();
                }

                FAutomationTestExecutionInfo Execution;
                const bool bOk = Framework.StopTest(Execution) && bLatentDone;
                (bOk ? Passed : Failed)++;

                TArray<TSharedPtr<FJsonValue>> Messages;
                for (const FAutomationExecutionEntry& Entry : Execution.GetEntries())
                {
                    auto Message = MakeShared<FJsonObject>();
                    const TCHAR* Type = Entry.Event.Type == EAutomationEventType::Error ? TEXT("Error")
                        : Entry.Event.Type == EAutomationEventType::Warning ? TEXT("Warning") : TEXT("Info");
                    Message->SetStringField(TEXT("type"), Type);
                    Message->SetStringField(TEXT("message"), Entry.Event.Message);
                    Messages.Add(MakeShared<FJsonValueObject>(Message));
                }

                auto Test = MakeShared<FJsonObject>();
                Test->SetStringField(TEXT("name"), Info.GetFullTestPath());
                Test->SetBoolField(TEXT("ok"), bOk);
                Test->SetNumberField(TEXT("errors"), Execution.GetErrorTotal());
                Test->SetNumberField(TEXT("warnings"), Execution.GetWarningTotal());
                Test->SetNumberField(TEXT("seconds"), FPlatformTime::Seconds() - Start);
                if (!bLatentDone)
                {
                    Test->SetBoolField(TEXT("latent_timeout"), true);
                }
                Test->SetArrayField(TEXT("messages"), Messages);
                Tests.Add(MakeShared<FJsonValueObject>(Test));
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("tests"), Tests);
            Result->SetNumberField(TEXT("passed"), Passed);
            Result->SetNumberField(TEXT("failed"), Failed);
            return Result;
        });
}
