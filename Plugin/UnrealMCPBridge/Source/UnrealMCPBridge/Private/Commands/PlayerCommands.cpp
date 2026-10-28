#include "Commands/PlayerCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "Commands/SceneTools.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"
#include "Components/CapsuleComponent.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "Kismet/GameplayStatics.h"
#include "WorldPartition/WorldPartitionSubsystem.h"

namespace MCPPlayer
{
    using UnrealMCPBridge::Json::RequireString;

    void PlayerFail(MCPProtocol::FMCPError& OutError, const FString& Message, int32 Code = MCPProtocol::FMCPError::InvalidParams)
    {
        OutError.Code = Code;
        OutError.Message = Message;
    }

    /** Multi-frame jobs (aim_at, teleport) by number. GameThread only: handlers and core tickers. */
    struct FJobs
    {
        int32 Counter = 0;
        TMap<int32, TSharedPtr<FJsonObject>> States;
    };

    FJobs& Jobs()
    {
        static FJobs Instance;
        return Instance;
    }

    int32 NewJob(const TCHAR* Kind)
    {
        const int32 Job = ++Jobs().Counter;
        auto State = MakeShared<FJsonObject>();
        State->SetNumberField(TEXT("job"), Job);
        State->SetStringField(TEXT("kind"), Kind);
        State->SetBoolField(TEXT("done"), false);
        Jobs().States.Add(Job, State);
        return Job;
    }

    TSharedPtr<FJsonObject> JobState(int32 Job)
    {
        const TSharedPtr<FJsonObject>* Found = Jobs().States.Find(Job);
        return Found ? *Found : nullptr;
    }

    TArray<TSharedPtr<FJsonValue>> VectorJson(const FVector& V)
    {
        return { MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z) };
    }

    bool ReadPlayerPoint(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FVector& Out)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Params.IsValid() || !Params->TryGetArrayField(Field, Values) || !Values || Values->Num() != 3)
        {
            return false;
        }
        Out = FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
        return true;
    }

    /** The PIE world and player N's controller. Everything here acts on the running game only. */
    APlayerController* PiePlayer(const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        UWorld* World = GEditor ? GEditor->PlayWorld.Get() : nullptr;
        if (!World)
        {
            PlayerFail(OutError, TEXT("PIE is not running (pie_start first)"));
            return nullptr;
        }
        double PlayerIndex = 0.0;
        if (Params.IsValid())
        {
            Params->TryGetNumberField(TEXT("player"), PlayerIndex);
        }
        APlayerController* Controller = UGameplayStatics::GetPlayerController(World, static_cast<int32>(PlayerIndex));
        if (!Controller)
        {
            PlayerFail(OutError, FString::Printf(TEXT("PIE has no player controller %d"), static_cast<int32>(PlayerIndex)));
        }
        return Controller;
    }

    /** "IA_GunAttack" (asset name, searched) or a full object path. */
    UInputAction* FindInputAction(const FString& Name)
    {
        if (Name.Contains(TEXT("/")))
        {
            return LoadObject<UInputAction>(nullptr, *Name);
        }
        IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        TArray<FAssetData> Assets;
        Registry.GetAssetsByClass(UInputAction::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);
        for (const FAssetData& Asset : Assets)
        {
            if (Asset.AssetName.ToString().Equals(Name, ESearchCase::IgnoreCase))
            {
                return Cast<UInputAction>(Asset.GetAsset());
            }
        }
        return nullptr;
    }

    /** JSON value -> the action's value type: bool, number (Axis1D), [x, y] (Axis2D), [x, y, z]. */
    bool ReadActionValue(const TSharedPtr<FJsonObject>& Params, const UInputAction* Action, FInputActionValue& Out, FString& OutError)
    {
        FVector Raw(1.0, 0.0, 0.0);   // no value = pressed
        const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
        if (Value.IsValid())
        {
            bool bBool = false;
            double Number = 0.0;
            const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
            if (Value->TryGetBool(bBool)) { Raw = FVector(bBool ? 1.0 : 0.0, 0.0, 0.0); }
            else if (Value->TryGetNumber(Number)) { Raw = FVector(Number, 0.0, 0.0); }
            else if (Value->TryGetArray(Array) && Array && Array->Num() >= 1 && Array->Num() <= 3)
            {
                Raw = FVector::ZeroVector;
                for (int32 Index = 0; Index < Array->Num(); ++Index)
                {
                    Raw[Index] = (*Array)[Index]->AsNumber();
                }
            }
            else
            {
                OutError = TEXT("value must be a bool, a number, or [x, y] / [x, y, z]");
                return false;
            }
        }
        Out = FInputActionValue(Action->ValueType, Raw);
        return true;
    }

    /** Point a job is about: an explicit target, or an actor's bounds center. */
    bool ReadTarget(const TSharedPtr<FJsonObject>& Params, UWorld* World, FVector& OutPoint, AActor*& OutActor, FString& OutError)
    {
        OutActor = nullptr;
        if (ReadPlayerPoint(Params, TEXT("target"), OutPoint))
        {
            return true;
        }
        FString ActorName;
        if (Params->TryGetStringField(TEXT("actor"), ActorName) && !ActorName.IsEmpty())
        {
            for (TActorIterator<AActor> It(World); It; ++It)
            {
                if (It->GetName() == ActorName || It->GetActorNameOrLabel() == ActorName)
                {
                    OutActor = *It;
                    FVector Extent;
                    OutActor->GetActorBounds(/*bOnlyCollidingComponents=*/false, OutPoint, Extent);
                    return true;
                }
            }
            OutError = FString::Printf(TEXT("No actor named '%s' in the PIE world"), *ActorName);
            return false;
        }
        OutError = TEXT("target ([x, y, z]) or actor (name / label) is required");
        return false;
    }

    /** Ground under Location in World (ignoring Ignore), or false. */
    bool TraceGround(UWorld* World, const FVector& Location, const AActor* Ignore, FVector& OutGround)
    {
        FCollisionQueryParams Query(SCENE_QUERY_STAT(MCPTeleportGround), false, Ignore);
        FHitResult Hit;
        if (World->LineTraceSingleByChannel(Hit, Location + FVector(0.0, 0.0, 500.0), Location - FVector(0.0, 0.0, 5000.0), ECC_Visibility, Query))
        {
            OutGround = Hit.ImpactPoint;
            return true;
        }
        return false;
    }
}

void FPlayerCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    using namespace MCPPlayer;

    // input.inject — feeds an Enhanced Input action to the PIE player, as if the bound key were used.
    // Params: action (asset name "IA_GunAttack" or full path, required), value (bool / number /
    // [x, y] / [x, y, z]; default pressed), seconds (0 = one frame, default; > 0 = hold that many
    // game seconds; < 0 = hold until release), release (bool: stop a held action), player (default 0).
    // The action only fires while a mapping context holding it is active — IA_GunAttack lives in
    // IMC_Aim, which the game adds only while aiming, so hold IA_Aim first.
    // Returns { action, value_type, mode ("tap" / "hold" / "hold_until_release" / "released") }.
    Registry.Register(TEXT("input.inject"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString ActionName;
            if (!RequireString(Params, TEXT("action"), ActionName, OutError)) return nullptr;
            APlayerController* Controller = PiePlayer(Params, OutError);
            if (!Controller) return nullptr;
            UEnhancedInputLocalPlayerSubsystem* Input = Controller->GetLocalPlayer()
                ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Controller->GetLocalPlayer())
                : nullptr;
            if (!Input)
            {
                PlayerFail(OutError, TEXT("The player has no Enhanced Input subsystem"), MCPProtocol::FMCPError::InternalError);
                return nullptr;
            }
            UInputAction* Action = FindInputAction(ActionName);
            if (!Action)
            {
                PlayerFail(OutError, FString::Printf(TEXT("No InputAction '%s'"), *ActionName));
                return nullptr;
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("action"), Action->GetPathName());
            Result->SetStringField(TEXT("value_type"), StaticEnum<EInputActionValueType>()->GetNameStringByValue(static_cast<int64>(Action->ValueType)));

            bool bRelease = false;
            Params->TryGetBoolField(TEXT("release"), bRelease);
            if (bRelease)
            {
                Input->StopContinuousInputInjectionForAction(Action);
                Result->SetStringField(TEXT("mode"), TEXT("released"));
                return Result;
            }

            FInputActionValue Value;
            FString ValueError;
            if (!ReadActionValue(Params, Action, Value, ValueError))
            {
                PlayerFail(OutError, ValueError);
                return nullptr;
            }
            double Seconds = 0.0;
            Params->TryGetNumberField(TEXT("seconds"), Seconds);

            if (Seconds == 0.0)
            {
                Input->InjectInputForAction(Action, Value, {}, {});
                Result->SetStringField(TEXT("mode"), TEXT("tap"));
                return Result;
            }

            Input->StartContinuousInputInjectionForAction(Action, Value, {}, {});
            if (Seconds < 0.0)
            {
                Result->SetStringField(TEXT("mode"), TEXT("hold_until_release"));
                return Result;
            }

            // Release after Seconds of game time (slomo and pause count like they do in the game).
            TWeakObjectPtr<UWorld> WeakWorld = Controller->GetWorld();
            TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> WeakInput = Input;
            TWeakObjectPtr<UInputAction> WeakAction = Action;
            const double ReleaseAt = Controller->GetWorld()->GetTimeSeconds() + Seconds;
            FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakWorld, WeakInput, WeakAction, ReleaseAt](float)
            {
                if (!WeakWorld.IsValid() || !WeakInput.IsValid() || !WeakAction.IsValid())
                {
                    return false;   // PIE ended
                }
                if (WeakWorld->GetTimeSeconds() < ReleaseAt)
                {
                    return true;
                }
                WeakInput->StopContinuousInputInjectionForAction(WeakAction.Get());
                return false;
            }));
            Result->SetStringField(TEXT("mode"), TEXT("hold"));
            Result->SetNumberField(TEXT("seconds"), Seconds);
            return Result;
        });

    // player.aim_at — turns the player's view toward a point at deg_per_sec, like a hand on a mouse.
    // Params: target ([x, y, z]) or actor (name / label — its bounds center), deg_per_sec (default
    // 360), error_deg (default 0: the aim settles that many degrees off in a random direction, picked
    // once per call), timeout (game seconds, default 5), player. Rotation is game-time based.
    // Returns { job } at once; player.job_status gives { done, reached, seconds, error_deg (final
    // angle between view and target) }.
    Registry.Register(TEXT("player.aim_at"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            APlayerController* Controller = PiePlayer(Params, OutError);
            if (!Controller) return nullptr;
            FVector Target;
            AActor* TargetActor = nullptr;
            FString TargetError;
            if (!ReadTarget(Params, Controller->GetWorld(), Target, TargetActor, TargetError))
            {
                PlayerFail(OutError, TargetError);
                return nullptr;
            }
            double DegPerSec = 360.0, ErrorDeg = 0.0, Timeout = 5.0;
            Params->TryGetNumberField(TEXT("deg_per_sec"), DegPerSec);
            Params->TryGetNumberField(TEXT("error_deg"), ErrorDeg);
            Params->TryGetNumberField(TEXT("timeout"), Timeout);
            // One random offset per call: a person misses by roughly the same amount for the whole motion.
            const double OffsetAngle = FMath::FRandRange(0.0, 2.0 * PI);
            const FRotator AimError(ErrorDeg * FMath::Sin(OffsetAngle), ErrorDeg * FMath::Cos(OffsetAngle), 0.0);

            const int32 Job = NewJob(TEXT("aim_at"));
            TWeakObjectPtr<APlayerController> WeakController = Controller;
            const double StartTime = Controller->GetWorld()->GetTimeSeconds();
            FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
                [Job, WeakController, Target, DegPerSec, AimError, Timeout, StartTime](float)
                {
                    TSharedPtr<FJsonObject> State = JobState(Job);
                    APlayerController* PC = WeakController.Get();
                    if (!State.IsValid()) return false;
                    if (!PC || !PC->GetWorld() || !PC->PlayerCameraManager)
                    {
                        State->SetBoolField(TEXT("done"), true);
                        State->SetStringField(TEXT("error"), TEXT("PIE ended"));
                        return false;
                    }
                    UWorld* World = PC->GetWorld();
                    const FVector CameraLocation = PC->PlayerCameraManager->GetCameraLocation();
                    const FRotator Desired = (Target - CameraLocation).Rotation() + AimError;
                    const FRotator Current = PC->GetControlRotation();
                    const FRotator Next = FMath::RInterpConstantTo(Current, Desired, World->GetDeltaSeconds(), static_cast<float>(DegPerSec));
                    PC->SetControlRotation(Next);

                    const double Elapsed = World->GetTimeSeconds() - StartTime;
                    const double ToTarget = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
                        Next.Vector() | (Target - CameraLocation).GetSafeNormal(), -1.0, 1.0)));
                    const bool bSettled = (Next - Desired).GetNormalized().IsNearlyZero(0.25);
                    if (bSettled || Elapsed >= Timeout)
                    {
                        State->SetBoolField(TEXT("done"), true);
                        State->SetBoolField(TEXT("reached"), bSettled);
                        State->SetNumberField(TEXT("seconds"), Elapsed);
                        State->SetNumberField(TEXT("error_deg"), ToTarget);
                        return false;
                    }
                    return true;
                }));

            auto Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("job"), Job);
            Result->SetArrayField(TEXT("target"), VectorJson(Target));
            return Result;
        });

    // player.teleport — moves the player and checks it stayed there. Holds the pawn in place (no
    // falling) until the World Partition cells around it have streamed in and a ground trace hits,
    // then stands it on that ground, waits settle_seconds, and measures where it really is.
    // Params: location ([x, y, z], required), yaw (optional), wait_streaming (default true),
    // timeout (game seconds, default 20), tolerance (cm, default 100), settle_seconds (default 0.5), player.
    // Returns { job }; player.job_status gives { done, ok, target, location (where it ended up),
    // distance, streaming_completed, ground_found, seconds, error, returned }. On a timeout or
    // missing ground the pawn is put back where it started (returned: true).
    Registry.Register(TEXT("player.teleport"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            APlayerController* Controller = PiePlayer(Params, OutError);
            if (!Controller) return nullptr;
            APawn* Pawn = Controller->GetPawn();
            if (!Pawn)
            {
                PlayerFail(OutError, TEXT("The player has no pawn"));
                return nullptr;
            }
            FVector Target;
            if (!ReadPlayerPoint(Params, TEXT("location"), Target))
            {
                PlayerFail(OutError, TEXT("location ([x, y, z]) is required"));
                return nullptr;
            }
            double Yaw = Controller->GetControlRotation().Yaw;
            const bool bSetYaw = Params->TryGetNumberField(TEXT("yaw"), Yaw);
            bool bWaitStreaming = true;
            Params->TryGetBoolField(TEXT("wait_streaming"), bWaitStreaming);
            double Timeout = 20.0, Tolerance = 100.0, Settle = 0.5;
            Params->TryGetNumberField(TEXT("timeout"), Timeout);
            Params->TryGetNumberField(TEXT("tolerance"), Tolerance);
            Params->TryGetNumberField(TEXT("settle_seconds"), Settle);

            // Park the pawn at the target with movement off, so it does not fall through unloaded ground.
            ACharacter* Character = Cast<ACharacter>(Pawn);
            UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr;
            const TEnumAsByte<EMovementMode> PreviousMode = Movement ? Movement->MovementMode : TEnumAsByte<EMovementMode>(MOVE_Walking);
            if (Movement)
            {
                Movement->StopMovementImmediately();
                Movement->DisableMovement();
            }
            const FVector Origin = Pawn->GetActorLocation();
            Pawn->SetActorLocation(Target, false, nullptr, ETeleportType::TeleportPhysics);
            if (bSetYaw)
            {
                Controller->SetControlRotation(FRotator(0.0, Yaw, 0.0));
            }

            const int32 Job = NewJob(TEXT("teleport"));
            TSharedPtr<FJsonObject> State = JobState(Job);
            State->SetArrayField(TEXT("target"), VectorJson(Target));

            enum class EPhase : uint8 { Waiting, Settling };
            struct FTeleportRun
            {
                EPhase Phase = EPhase::Waiting;
                double StartTime = 0.0;
                double SettleUntil = 0.0;
                bool bStreamingCompleted = false;
                bool bGroundFound = false;
            };
            TSharedRef<FTeleportRun> Run = MakeShared<FTeleportRun>();
            Run->StartTime = Controller->GetWorld()->GetTimeSeconds();

            TWeakObjectPtr<APawn> WeakPawn = Pawn;
            TWeakObjectPtr<UCharacterMovementComponent> WeakMovement = Movement;
            FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
                [Job, WeakPawn, WeakMovement, PreviousMode, Origin, Target, bWaitStreaming, Timeout, Tolerance, Settle, Run](float)
                {
                    TSharedPtr<FJsonObject> Out = JobState(Job);
                    APawn* P = WeakPawn.Get();
                    if (!Out.IsValid()) return false;
                    if (!P || !P->GetWorld())
                    {
                        Out->SetBoolField(TEXT("done"), true);
                        Out->SetBoolField(TEXT("ok"), false);
                        Out->SetStringField(TEXT("error"), TEXT("PIE ended or the pawn is gone"));
                        return false;
                    }
                    UWorld* World = P->GetWorld();
                    const double Elapsed = World->GetTimeSeconds() - Run->StartTime;
                    auto Finish = [&](bool bOk, const FString& Error)
                    {
                        const FVector Actual = P->GetActorLocation();
                        if (!Error.IsEmpty())
                        {
                            // Never leave the player parked over a hole: put it back where it was.
                            P->SetActorLocation(Origin, false, nullptr, ETeleportType::TeleportPhysics);
                            Out->SetBoolField(TEXT("returned"), true);
                        }
                        if (WeakMovement.IsValid() && WeakMovement->MovementMode == MOVE_None)
                        {
                            WeakMovement->SetMovementMode(PreviousMode == MOVE_None ? MOVE_Walking : PreviousMode.GetValue());
                        }
                        const double Distance = FVector::Dist2D(Actual, Target);
                        Out->SetBoolField(TEXT("done"), true);
                        Out->SetBoolField(TEXT("ok"), bOk && Distance <= Tolerance);
                        Out->SetArrayField(TEXT("location"), VectorJson(Actual));
                        Out->SetNumberField(TEXT("distance"), Distance);
                        Out->SetBoolField(TEXT("streaming_completed"), Run->bStreamingCompleted);
                        Out->SetBoolField(TEXT("ground_found"), Run->bGroundFound);
                        Out->SetNumberField(TEXT("seconds"), Elapsed);
                        if (!Error.IsEmpty())
                        {
                            Out->SetStringField(TEXT("error"), Error);
                        }
                        else if (Distance > Tolerance)
                        {
                            Out->SetStringField(TEXT("error"), FString::Printf(TEXT("ended %.0f cm from the target (tolerance %.0f)"), Distance, Tolerance));
                        }
                    };

                    if (Run->Phase == EPhase::Waiting)
                    {
                        const UWorldPartitionSubsystem* Partition = World->GetSubsystem<UWorldPartitionSubsystem>();
                        Run->bStreamingCompleted = !bWaitStreaming || !World->GetWorldPartition() || !Partition || Partition->IsStreamingCompleted();
                        FVector Ground;
                        Run->bGroundFound = TraceGround(World, Target, P, Ground);
                        if (Run->bStreamingCompleted && Run->bGroundFound)
                        {
                            float HalfHeight = 0.f;
                            if (const ACharacter* C = Cast<ACharacter>(P))
                            {
                                HalfHeight = C->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
                            }
                            P->SetActorLocation(Ground + FVector(0.0, 0.0, HalfHeight + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
                            if (WeakMovement.IsValid())
                            {
                                WeakMovement->SetMovementMode(PreviousMode == MOVE_None ? MOVE_Walking : PreviousMode.GetValue());
                            }
                            Run->Phase = EPhase::Settling;
                            Run->SettleUntil = World->GetTimeSeconds() + Settle;
                            return true;
                        }
                        if (Elapsed >= Timeout)
                        {
                            Finish(false, Run->bGroundFound ? TEXT("streaming did not complete in time") : TEXT("no ground under the target"));
                            return false;
                        }
                        return true;
                    }

                    if (World->GetTimeSeconds() < Run->SettleUntil)
                    {
                        return true;
                    }
                    Finish(true, FString());
                    return false;
                }));

            auto Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("job"), Job);
            return Result;
        });

    // view.is_visible — read-only. Is a point / actor on the PIE player's screen and not hidden?
    // Params: target ([x, y, z]) or actor (name / label — its bounds center), player.
    // Returns { on_screen, in_front, screen ([x, y] px), viewport ([w, h]), occluded, occluder
    // (actor / component that hides it, or null), distance }.
    Registry.Register(TEXT("view.is_visible"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            APlayerController* Controller = PiePlayer(Params, OutError);
            if (!Controller) return nullptr;
            UWorld* World = Controller->GetWorld();
            FVector Target;
            AActor* TargetActor = nullptr;
            FString TargetError;
            if (!ReadTarget(Params, World, Target, TargetActor, TargetError))
            {
                PlayerFail(OutError, TargetError);
                return nullptr;
            }

            FVector2D Screen = FVector2D::ZeroVector;
            const bool bInFront = Controller->ProjectWorldLocationToScreen(Target, Screen, /*bPlayerViewportRelative=*/true);
            int32 Width = 0, Height = 0;
            Controller->GetViewportSize(Width, Height);
            const bool bInsideViewport = bInFront && Screen.X >= 0 && Screen.Y >= 0 && Screen.X <= Width && Screen.Y <= Height;

            const FVector CameraLocation = Controller->PlayerCameraManager ? Controller->PlayerCameraManager->GetCameraLocation() : Controller->GetFocalLocation();
            FCollisionQueryParams Query(SCENE_QUERY_STAT(MCPIsVisible), true);
            Query.AddIgnoredActor(Controller->GetPawn());
            if (TargetActor)
            {
                Query.AddIgnoredActor(TargetActor);
            }
            FHitResult Hit;
            const bool bBlocked = World->LineTraceSingleByChannel(Hit, CameraLocation, Target, ECC_Visibility, Query)
                && FVector::Dist(Hit.ImpactPoint, Target) > 10.0;

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("on_screen"), bInsideViewport && !bBlocked);
            Result->SetBoolField(TEXT("in_front"), bInFront);
            Result->SetBoolField(TEXT("inside_viewport"), bInsideViewport);
            Result->SetArrayField(TEXT("screen"), { MakeShared<FJsonValueNumber>(Screen.X), MakeShared<FJsonValueNumber>(Screen.Y) });
            Result->SetArrayField(TEXT("viewport"), { MakeShared<FJsonValueNumber>(Width), MakeShared<FJsonValueNumber>(Height) });
            Result->SetBoolField(TEXT("occluded"), bBlocked);
            if (bBlocked)
            {
                auto Occluder = MakeShared<FJsonObject>();
                Occluder->SetStringField(TEXT("actor"), Hit.GetActor() ? Hit.GetActor()->GetActorNameOrLabel() : FString());
                Occluder->SetStringField(TEXT("component"), Hit.GetComponent() ? Hit.GetComponent()->GetName() : FString());
                Occluder->SetArrayField(TEXT("location"), VectorJson(Hit.ImpactPoint));
                Result->SetObjectField(TEXT("occluder"), Occluder);
            }
            else
            {
                Result->SetField(TEXT("occluder"), MakeShared<FJsonValueNull>());
            }
            Result->SetArrayField(TEXT("target"), VectorJson(Target));
            Result->SetNumberField(TEXT("distance"), FVector::Dist(CameraLocation, Target));
            return Result;
        });

    // player.job_status — the state of an aim_at / teleport job. Params: job (required).
    Registry.Register(TEXT("player.job_status"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            double JobNumber = 0.0;
            if (!Params.IsValid() || !Params->TryGetNumberField(TEXT("job"), JobNumber))
            {
                PlayerFail(OutError, TEXT("job is required"));
                return nullptr;
            }
            TSharedPtr<FJsonObject> State = JobState(static_cast<int32>(JobNumber));
            if (!State.IsValid())
            {
                PlayerFail(OutError, FString::Printf(TEXT("No job %d"), static_cast<int32>(JobNumber)));
                return nullptr;
            }
            return MakeShared<FJsonObject>(*State);
        });
}
