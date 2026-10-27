#include "Commands/DebugRunCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "MCPCommandRegistry.h"
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
            FString Command, Which;
            if (!RequireString(Params, TEXT("command"), Command, OutError)) return nullptr;
            Params->TryGetStringField(TEXT("world"), Which);
            double PlayerIndex = 0.0;
            Params->TryGetNumberField(TEXT("player"), PlayerIndex);
            if (!GEditor)
            {
                Fail(OutError, TEXT("GEditor is not available"), MCPProtocol::FMCPError::InternalError);
                return nullptr;
            }

            UWorld* PlayWorld = GEditor->PlayWorld.Get();
            const bool bUsePie = Which.IsEmpty() ? PlayWorld != nullptr : Which.Equals(TEXT("pie"), ESearchCase::IgnoreCase);
            if (bUsePie && !PlayWorld)
            {
                Fail(OutError, TEXT("world=pie but PIE is not running (pie.start first)"));
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
