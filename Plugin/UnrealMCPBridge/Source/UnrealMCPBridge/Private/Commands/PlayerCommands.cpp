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
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/GameViewportClient.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/Paths.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "UnrealClient.h"

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

    // view.capture — saves what the PIE player sees, debug lines included (it reads the game
    // viewport, unlike scene.capture_view which renders the editor world through a scene capture).
    // Params: path (png/jpg under <Project>/Saved or CaptureOutputRoots; frames > 1 append _0001 ...),
    // frames (default 1, max 300), every (frames between shots, default 1), player.
    // Returns { job }; player.job_status gives { done, files, width, height, frames, error }.
    // The editor window must be visible — a background PIE viewport does not render new frames.
    Registry.Register(TEXT("view.capture"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            APlayerController* Controller = PiePlayer(Params, OutError);
            if (!Controller) return nullptr;
            FString RequestedPath;
            Params->TryGetStringField(TEXT("path"), RequestedPath);
            FString BasePath, PathError;
            if (!UnrealMCPBridge::Scene::ResolveImageOutputPath(RequestedPath, BasePath, PathError))
            {
                PlayerFail(OutError, PathError);
                return nullptr;
            }
            double FramesValue = 1.0, EveryValue = 1.0;
            Params->TryGetNumberField(TEXT("frames"), FramesValue);
            Params->TryGetNumberField(TEXT("every"), EveryValue);
            const int32 Frames = FMath::Clamp(static_cast<int32>(FramesValue), 1, 300);
            const int32 Every = FMath::Max(1, static_cast<int32>(EveryValue));

            const int32 Job = NewJob(TEXT("view_capture"));
            TWeakObjectPtr<UWorld> WeakWorld = Controller->GetWorld();
            TSharedRef<int32> Tick = MakeShared<int32>(0);
            TSharedRef<TArray<TSharedPtr<FJsonValue>>> Files = MakeShared<TArray<TSharedPtr<FJsonValue>>>();
            FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
                [Job, WeakWorld, BasePath, Frames, Every, Tick, Files](float)
                {
                    TSharedPtr<FJsonObject> State = JobState(Job);
                    if (!State.IsValid()) return false;
                    UGameViewportClient* ViewportClient = WeakWorld.IsValid() ? WeakWorld->GetGameViewport() : nullptr;
                    FViewport* Viewport = ViewportClient ? ViewportClient->Viewport : nullptr;
                    if (!Viewport)
                    {
                        State->SetBoolField(TEXT("done"), true);
                        State->SetStringField(TEXT("error"), TEXT("PIE ended or has no viewport"));
                        return false;
                    }
                    if ((*Tick)++ % Every != 0)
                    {
                        return true;
                    }
                    TArray<FColor> Pixels;
                    const FIntPoint Size = Viewport->GetSizeXY();
                    if (Size.X <= 0 || !Viewport->ReadPixels(Pixels) || Pixels.Num() != Size.X * Size.Y)
                    {
                        State->SetBoolField(TEXT("done"), true);
                        State->SetStringField(TEXT("error"), TEXT("Reading the PIE viewport failed (is the editor window minimized?)"));
                        return false;
                    }
                    for (FColor& Pixel : Pixels)
                    {
                        Pixel.A = 255;
                    }
                    const FString File = Frames == 1 ? BasePath
                        : FPaths::GetPath(BasePath) / FString::Printf(TEXT("%s_%04d.%s"), *FPaths::GetBaseFilename(BasePath), Files->Num() + 1, *FPaths::GetExtension(BasePath));
                    IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), /*Tree=*/true);
                    if (!FImageUtils::SaveImageByExtension(*File, FImageView(Pixels.GetData(), Size.X, Size.Y)))
                    {
                        State->SetBoolField(TEXT("done"), true);
                        State->SetStringField(TEXT("error"), FString::Printf(TEXT("Writing %s failed"), *File));
                        return false;
                    }
                    Files->Add(MakeShared<FJsonValueString>(File));
                    State->SetNumberField(TEXT("width"), Size.X);
                    State->SetNumberField(TEXT("height"), Size.Y);
                    State->SetArrayField(TEXT("files"), *Files);
                    State->SetNumberField(TEXT("frames"), Files->Num());
                    if (Files->Num() >= Frames)
                    {
                        State->SetBoolField(TEXT("done"), true);
                        return false;
                    }
                    return true;
                }));

            auto Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("job"), Job);
            return Result;
        });

    // anim.record — what an actor's skeletal meshes play, sampled over time (montages, the sequences
    // inside dynamic montages from PlaySlotAnimationAsDynamicMontage, slot, section, position, weight).
    // Params: actor (name / label in the PIE world, required), seconds (game seconds, default 5),
    // interval (game seconds between samples, default 0.1), mesh (component name filter, optional),
    // changes_only (default true: a sample is kept only when something but position moved).
    // Returns { job }; player.job_status gives { done, actor, samples: [{t, mesh, montages: [{montage,
    // anim, slot, section, position, weight, playing}]}] }.
    Registry.Register(TEXT("anim.record"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            APlayerController* Controller = PiePlayer(Params, OutError);
            if (!Controller) return nullptr;
            FVector Ignored;
            AActor* Actor = nullptr;
            FString TargetError;
            if (!Params->HasField(TEXT("actor")) || !ReadTarget(Params, Controller->GetWorld(), Ignored, Actor, TargetError) || !Actor)
            {
                PlayerFail(OutError, TargetError.IsEmpty() ? TEXT("actor (name / label) is required") : TargetError);
                return nullptr;
            }
            double Seconds = 5.0, Interval = 0.1;
            Params->TryGetNumberField(TEXT("seconds"), Seconds);
            Params->TryGetNumberField(TEXT("interval"), Interval);
            FString MeshFilter;
            Params->TryGetStringField(TEXT("mesh"), MeshFilter);
            bool bChangesOnly = true;
            Params->TryGetBoolField(TEXT("changes_only"), bChangesOnly);

            const int32 Job = NewJob(TEXT("anim_record"));
            TSharedPtr<FJsonObject> Start = JobState(Job);
            Start->SetStringField(TEXT("actor"), Actor->GetActorNameOrLabel());
            TWeakObjectPtr<AActor> WeakActor = Actor;
            const double StartTime = Actor->GetWorld()->GetTimeSeconds();
            TSharedRef<double> NextSample = MakeShared<double>(StartTime);
            TSharedRef<TArray<TSharedPtr<FJsonValue>>> Samples = MakeShared<TArray<TSharedPtr<FJsonValue>>>();
            TSharedRef<TMap<FString, FString>> LastKey = MakeShared<TMap<FString, FString>>();
            FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
                [Job, WeakActor, StartTime, Seconds, Interval, MeshFilter, bChangesOnly, NextSample, Samples, LastKey](float)
                {
                    TSharedPtr<FJsonObject> State = JobState(Job);
                    AActor* A = WeakActor.Get();
                    if (!State.IsValid()) return false;
                    if (!A || !A->GetWorld())
                    {
                        State->SetBoolField(TEXT("done"), true);
                        State->SetStringField(TEXT("error"), TEXT("PIE ended or the actor is gone"));
                        return false;
                    }
                    const double Now = A->GetWorld()->GetTimeSeconds();
                    if (Now < *NextSample)
                    {
                        return true;
                    }
                    *NextSample = Now + Interval;
                    TArray<USkeletalMeshComponent*> Meshes;
                    A->GetComponents<USkeletalMeshComponent>(Meshes);
                    for (USkeletalMeshComponent* Mesh : Meshes)
                    {
                        if (!MeshFilter.IsEmpty() && !Mesh->GetName().Contains(MeshFilter))
                        {
                            continue;
                        }
                        UAnimInstance* Anim = Mesh->GetAnimInstance();
                        TArray<TSharedPtr<FJsonValue>> Montages;
                        FString Key;
                        for (const FAnimMontageInstance* Instance : Anim ? Anim->MontageInstances : TArray<FAnimMontageInstance*>())
                        {
                            const UAnimMontage* Montage = Instance ? Instance->Montage.Get() : nullptr;
                            if (!Montage)
                            {
                                continue;
                            }
                            // Dynamic montages (PlaySlotAnimationAsDynamicMontage) are transient wrappers; the sequence is the first segment.
                            FString Inner, Slot;
                            if (Montage->SlotAnimTracks.Num() > 0)
                            {
                                Slot = Montage->SlotAnimTracks[0].SlotName.ToString();
                                const TArray<FAnimSegment>& Segments = Montage->SlotAnimTracks[0].AnimTrack.AnimSegments;
                                if (Segments.Num() > 0 && Segments[0].GetAnimReference())
                                {
                                    Inner = Segments[0].GetAnimReference()->GetName();
                                }
                            }
                            auto Entry = MakeShared<FJsonObject>();
                            Entry->SetStringField(TEXT("montage"), Montage->GetName());
                            Entry->SetStringField(TEXT("anim"), Inner);
                            Entry->SetStringField(TEXT("slot"), Slot);
                            Entry->SetStringField(TEXT("section"), Instance->GetCurrentSection().ToString());
                            Entry->SetNumberField(TEXT("position"), Instance->GetPosition());
                            Entry->SetNumberField(TEXT("weight"), Instance->GetWeight());
                            Entry->SetBoolField(TEXT("playing"), Instance->IsPlaying());
                            Montages.Add(MakeShared<FJsonValueObject>(Entry));
                            Key += FString::Printf(TEXT("%s|%s|%s|%d|%.1f;"), *Montage->GetName(), *Inner,
                                *Instance->GetCurrentSection().ToString(), Instance->IsPlaying() ? 1 : 0, Instance->GetWeight());
                        }
                        // The first sample of each mesh is always kept, even when nothing plays yet.
                        const FString* Last = LastKey->Find(Mesh->GetName());
                        if (bChangesOnly && Last && *Last == Key)
                        {
                            continue;
                        }
                        LastKey->Add(Mesh->GetName(), Key);
                        auto Sample = MakeShared<FJsonObject>();
                        Sample->SetNumberField(TEXT("t"), Now - StartTime);
                        Sample->SetStringField(TEXT("mesh"), Mesh->GetName());
                        Sample->SetArrayField(TEXT("montages"), Montages);
                        Samples->Add(MakeShared<FJsonValueObject>(Sample));
                    }
                    State->SetArrayField(TEXT("samples"), *Samples);
                    if (Now - StartTime >= Seconds)
                    {
                        State->SetBoolField(TEXT("done"), true);
                        return false;
                    }
                    return true;
                }));

            auto Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("job"), Job);
            return Result;
        });

    // player.move_to — walks the PIE player to a point along the navmesh with move input, the way a
    // person moves (it cannot shoot meanwhile if the game says so). Params: location ([x, y, z],
    // required), sprint (bool), move_action (default "IA_Move", Axis2D: x = right, y = forward),
    // sprint_action (default "IA_Sprint"), acceptance (cm, default 100), timeout (game seconds,
    // default 30), player. Returns { job }; player.job_status gives { done, result ("arrived" /
    // "blocked" / "timeout" / "no_path"), location, distance, seconds, path_points }.
    Registry.Register(TEXT("player.move_to"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            APlayerController* Controller = PiePlayer(Params, OutError);
            if (!Controller) return nullptr;
            APawn* Pawn = Controller->GetPawn();
            FVector Target;
            if (!Pawn || !ReadPlayerPoint(Params, TEXT("location"), Target))
            {
                PlayerFail(OutError, Pawn ? TEXT("location ([x, y, z]) is required") : TEXT("The player has no pawn"));
                return nullptr;
            }
            FString MoveName = TEXT("IA_Move"), SprintName = TEXT("IA_Sprint");
            Params->TryGetStringField(TEXT("move_action"), MoveName);
            Params->TryGetStringField(TEXT("sprint_action"), SprintName);
            bool bSprint = false;
            Params->TryGetBoolField(TEXT("sprint"), bSprint);
            double Acceptance = 100.0, Timeout = 30.0;
            Params->TryGetNumberField(TEXT("acceptance"), Acceptance);
            Params->TryGetNumberField(TEXT("timeout"), Timeout);

            UEnhancedInputLocalPlayerSubsystem* Input = Controller->GetLocalPlayer()
                ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Controller->GetLocalPlayer()) : nullptr;
            UInputAction* MoveAction = FindInputAction(MoveName);
            UInputAction* SprintAction = bSprint ? FindInputAction(SprintName) : nullptr;
            if (!Input || !MoveAction || (bSprint && !SprintAction))
            {
                PlayerFail(OutError, FString::Printf(TEXT("Missing Enhanced Input subsystem or action (%s%s)"), *MoveName, bSprint ? *(TEXT(", ") + SprintName) : TEXT("")));
                return nullptr;
            }

            UWorld* World = Controller->GetWorld();
            UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
            UNavigationPath* Path = Nav ? Nav->FindPathToLocationSynchronously(World, Pawn->GetActorLocation(), Target, Pawn) : nullptr;
            const int32 Job = NewJob(TEXT("move_to"));
            TSharedPtr<FJsonObject> State = JobState(Job);
            if (!Path || !Path->IsValid() || Path->PathPoints.Num() == 0)
            {
                State->SetBoolField(TEXT("done"), true);
                State->SetStringField(TEXT("result"), TEXT("no_path"));
                auto Result = MakeShared<FJsonObject>();
                Result->SetNumberField(TEXT("job"), Job);
                return Result;
            }
            TSharedRef<TArray<FVector>> Points = MakeShared<TArray<FVector>>(Path->PathPoints);
            State->SetNumberField(TEXT("path_points"), Points->Num());
            if (SprintAction)
            {
                Input->StartContinuousInputInjectionForAction(SprintAction, FInputActionValue(true), {}, {});
            }

            struct FWalk
            {
                int32 Next = 1;
                double StartTime = 0.0;
                double LastProgressTime = 0.0;
                double BestDistance = TNumericLimits<double>::Max();
            };
            TSharedRef<FWalk> Walk = MakeShared<FWalk>();
            Walk->StartTime = Walk->LastProgressTime = World->GetTimeSeconds();
            TWeakObjectPtr<APlayerController> WeakController = Controller;
            TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> WeakInput = Input;
            TWeakObjectPtr<UInputAction> WeakMove = MoveAction;
            TWeakObjectPtr<UInputAction> WeakSprint = SprintAction;
            FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
                [Job, WeakController, WeakInput, WeakMove, WeakSprint, Points, Target, Acceptance, Timeout, Walk](float)
                {
                    TSharedPtr<FJsonObject> Out = JobState(Job);
                    APlayerController* PC = WeakController.Get();
                    APawn* P = PC ? PC->GetPawn() : nullptr;
                    if (!Out.IsValid()) return false;
                    auto Stop = [&](const TCHAR* Result)
                    {
                        if (WeakInput.IsValid() && WeakMove.IsValid()) WeakInput->StopContinuousInputInjectionForAction(WeakMove.Get());
                        if (WeakInput.IsValid() && WeakSprint.IsValid()) WeakInput->StopContinuousInputInjectionForAction(WeakSprint.Get());
                        Out->SetBoolField(TEXT("done"), true);
                        Out->SetStringField(TEXT("result"), Result);
                        if (P)
                        {
                            Out->SetArrayField(TEXT("location"), VectorJson(P->GetActorLocation()));
                            Out->SetNumberField(TEXT("distance"), FVector::Dist2D(P->GetActorLocation(), Target));
                            Out->SetNumberField(TEXT("seconds"), P->GetWorld()->GetTimeSeconds() - Walk->StartTime);
                        }
                        return false;
                    };
                    if (!P || !P->GetWorld())
                    {
                        return Stop(TEXT("pie_ended"));
                    }
                    const double Now = P->GetWorld()->GetTimeSeconds();
                    const FVector Here = P->GetActorLocation();
                    const double Remaining = FVector::Dist2D(Here, Target);
                    if (Remaining <= Acceptance)
                    {
                        return Stop(TEXT("arrived"));
                    }
                    if (Remaining < Walk->BestDistance - 10.0)
                    {
                        Walk->BestDistance = Remaining;
                        Walk->LastProgressTime = Now;
                    }
                    if (Now - Walk->LastProgressTime > 2.0)
                    {
                        return Stop(TEXT("blocked"));
                    }
                    if (Now - Walk->StartTime > Timeout)
                    {
                        return Stop(TEXT("timeout"));
                    }
                    // Next path point we are not yet at; steer toward it relative to the control yaw.
                    while (Walk->Next < Points->Num() - 1 && FVector::Dist2D(Here, (*Points)[Walk->Next]) < 80.0)
                    {
                        ++Walk->Next;
                    }
                    const FVector Goal = (*Points)[FMath::Min(Walk->Next, Points->Num() - 1)];
                    const FVector Dir = (Goal - Here).GetSafeNormal2D();
                    const FRotator Yaw(0.0, PC->GetControlRotation().Yaw, 0.0);
                    const FVector Forward = Yaw.Vector();
                    const FVector Right = FRotationMatrix(Yaw).GetScaledAxis(EAxis::Y);
                    const FVector2D Axis(FVector::DotProduct(Dir, Right), FVector::DotProduct(Dir, Forward));
                    WeakInput->StartContinuousInputInjectionForAction(WeakMove.Get(), FInputActionValue(Axis.GetSafeNormal()), {}, {});
                    return true;
                }));

            auto Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("job"), Job);
            Result->SetNumberField(TEXT("path_points"), Points->Num());
            return Result;
        });

    // player.job_status — the state of an aim_at / teleport / move_to / view.capture / anim.record job. Params: job (required).
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
