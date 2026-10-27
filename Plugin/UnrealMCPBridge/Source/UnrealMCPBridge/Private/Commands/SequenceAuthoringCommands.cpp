#include "Commands/SequenceAuthoringCommands.h"
#include "Commands/SequencerHelpers.h"
#include "Commands/SceneTools.h"
#include "Commands/CommandJsonHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Camera/CameraComponent.h"
#include "CineCameraComponent.h"
#include "Animation/AnimSequenceBase.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "MovieScene.h"
#include "MovieSceneSequencePlaybackSettings.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieSceneTimeHelpers.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace MCPSequenceAuthoring
{
    using UnrealMCPBridge::Json::RequireString;
    using UnrealMCPBridge::Json::ReadVector3;
    using UnrealMCPBridge::Json::ReadRotator;
    using UnrealMCPBridge::Json::MakeVector3JsonArray;
    using UnrealMCPBridge::Json::MakeRotatorJsonArray;

    void Fail(MCPProtocol::FMCPError& OutError, const FString& Message, int32 Code = MCPProtocol::FMCPError::InvalidParams)
    {
        OutError.Code = Code;
        OutError.Message = Message;
    }

    /** Transient sequences live in the transient package; this keeps them from being collected. */
    TArray<TStrongObjectPtr<ULevelSequence>>& TransientSequences()
    {
        static TArray<TStrongObjectPtr<ULevelSequence>> Sequences;
        return Sequences;
    }

    /**
     * A point in time from Params: <FrameField> (display frame, the timeline numbers) or <TimeField>
     * (seconds). Neither present -> Default.
     */
    FFrameNumber ReadTick(const UMovieScene* MovieScene, const TSharedPtr<FJsonObject>& Params, const TCHAR* FrameField, const TCHAR* TimeField, FFrameNumber Default)
    {
        double Value = 0.0;
        if (Params->TryGetNumberField(FrameField, Value))
        {
            return MCPSequencer::DisplayToTick(MovieScene, Value);
        }
        if (Params->TryGetNumberField(TimeField, Value))
        {
            return MovieScene->GetTickResolution().AsFrameTime(Value).RoundToFrame();
        }
        return Default;
    }

    FFrameNumber PlaybackStart(const UMovieScene* MovieScene)
    {
        return UE::MovieScene::DiscreteInclusiveLower(MovieScene->GetPlaybackRange());
    }

    /**
     * The binding a track goes on: binding (existing name/GUID), actor (a level actor, bound as a
     * possessable) or spawn (an asset or actor class, made a spawnable through sequencer.add_spawnable so
     * the skeletal-mesh template handling stays in one place). Prefix lets camera_cut use camera* fields.
     */
    bool ResolveOrCreateBinding(const FMCPCommandRegistry& Registry, ULevelSequence* Sequence, const TSharedPtr<FJsonObject>& Params,
        const FString& Prefix, FGuid& OutGuid, FString& OutHow, AActor*& OutPossessedActor, MCPProtocol::FMCPError& OutError)
    {
        OutPossessedActor = nullptr;
        FString Ref, ActorName, SpawnAsset, Name;
        Params->TryGetStringField(Prefix.IsEmpty() ? TEXT("binding") : *Prefix, Ref);
        Params->TryGetStringField(*(Prefix.IsEmpty() ? FString(TEXT("actor")) : Prefix + TEXT("_actor")), ActorName);
        Params->TryGetStringField(*(Prefix.IsEmpty() ? FString(TEXT("spawn")) : Prefix + TEXT("_spawn")), SpawnAsset);
        Params->TryGetStringField(*(Prefix.IsEmpty() ? FString(TEXT("name")) : Prefix + TEXT("_name")), Name);

        if (!Ref.IsEmpty())
        {
            OutHow = TEXT("existing");
            return MCPSequencer::ResolveBinding(Sequence, Ref, OutGuid, OutError);
        }

        if (!ActorName.IsEmpty())
        {
            UWorld* World = UnrealMCPBridge::Scene::GetEditorWorld(OutError);
            if (!World) return false;
            FString FindError;
            AActor* Actor = UnrealMCPBridge::Scene::FindActor(World, ActorName, FindError);
            if (!Actor)
            {
                Fail(OutError, FindError);
                return false;
            }
            Sequence->Modify();
            // ULevelSequence overrides it protected; call through the public base like CreateSpawnable.
            OutGuid = static_cast<UMovieSceneSequence*>(Sequence)->CreatePossessable(Actor);
            if (!OutGuid.IsValid())
            {
                Fail(OutError, FString::Printf(TEXT("Could not bind %s as a possessable"), *Actor->GetActorLabel()), MCPProtocol::FMCPError::InternalError);
                return false;
            }
            if (!Name.IsEmpty())
            {
                if (FMovieScenePossessable* Possessable = Sequence->GetMovieScene()->FindPossessable(OutGuid))
                {
                    Possessable->SetName(Name);
                }
            }
            OutHow = TEXT("possessable");
            OutPossessedActor = Actor;
            return true;
        }

        if (!SpawnAsset.IsEmpty())
        {
            auto SpawnParams = MakeShared<FJsonObject>();
            SpawnParams->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
            SpawnParams->SetStringField(TEXT("asset_path"), SpawnAsset);
            if (!Name.IsEmpty())
            {
                SpawnParams->SetStringField(TEXT("name"), Name);
            }
            TSharedPtr<FJsonObject> SpawnResult;
            if (!Registry.Dispatch(TEXT("sequencer.add_spawnable"), SpawnParams, SpawnResult, OutError) || !SpawnResult)
            {
                return false;
            }
            FGuid::Parse(SpawnResult->GetStringField(TEXT("guid")), OutGuid);
            OutHow = TEXT("spawnable");
            return OutGuid.IsValid();
        }

        Fail(OutError, Prefix.IsEmpty()
            ? TEXT("give binding (name/GUID), actor (level actor to possess) or spawn (asset / actor class to spawn)")
            : *FString::Printf(TEXT("give %s (binding), %s_actor or %s_spawn"), *Prefix, *Prefix, *Prefix));
        return false;
    }

    /** The binding's transform section, created (infinite range) with its track when missing. */
    UMovieScene3DTransformSection* FindOrAddTransformSection(UMovieScene* MovieScene, const FGuid& Guid, bool& bOutCreated)
    {
        bOutCreated = false;
        UMovieScene3DTransformTrack* Track = MovieScene->FindTrack<UMovieScene3DTransformTrack>(Guid);
        if (!Track)
        {
            Track = MovieScene->AddTrack<UMovieScene3DTransformTrack>(Guid);
            bOutCreated = true;
        }
        if (!Track)
        {
            return nullptr;
        }
        if (Track->GetAllSections().Num() > 0)
        {
            return Cast<UMovieScene3DTransformSection>(Track->GetAllSections()[0]);
        }
        UMovieScene3DTransformSection* Section = Cast<UMovieScene3DTransformSection>(Track->CreateNewSection());
        if (Section)
        {
            Section->SetRange(TRange<FFrameNumber>::All());
            Track->AddSection(*Section);
            bOutCreated = true;
        }
        return Section;
    }

    /** Location / rotation ([pitch, yaw, roll]) / scale into the nine transform channels. */
    enum class EKeyInterp : uint8 { Cubic, Linear, Constant };

    void AddTransformKey(TArrayView<FMovieSceneDoubleChannel*> Channels, FFrameNumber Tick, const FVector* Location, const FRotator* Rotation, const FVector* Scale, EKeyInterp Interp)
    {
        auto Add = [Tick, Interp](FMovieSceneDoubleChannel* Channel, double Value)
        {
            switch (Interp)
            {
            case EKeyInterp::Linear:   Channel->AddLinearKey(Tick, Value); break;
            case EKeyInterp::Constant: Channel->AddConstantKey(Tick, Value); break;
            default:                   Channel->AddCubicKey(Tick, Value); break;
            }
        };
        // Channel order: Location X/Y/Z, Rotation X(roll)/Y(pitch)/Z(yaw), Scale X/Y/Z.
        if (Location) { Add(Channels[0], Location->X); Add(Channels[1], Location->Y); Add(Channels[2], Location->Z); }
        if (Rotation) { Add(Channels[3], Rotation->Roll); Add(Channels[4], Rotation->Pitch); Add(Channels[5], Rotation->Yaw); }
        if (Scale)    { Add(Channels[6], Scale->X); Add(Channels[7], Scale->Y); Add(Channels[8], Scale->Z); }
    }

    /** Defaults = the actor's current transform, so a new track does not snap a possessable to the origin. */
    void SetTransformDefaults(TArrayView<FMovieSceneDoubleChannel*> Channels, const FTransform& Transform)
    {
        const FVector Location = Transform.GetLocation();
        const FRotator Rotation = Transform.Rotator();
        const FVector Scale = Transform.GetScale3D();
        const double Values[9] = { Location.X, Location.Y, Location.Z, Rotation.Roll, Rotation.Pitch, Rotation.Yaw, Scale.X, Scale.Y, Scale.Z };
        for (int32 Index = 0; Index < 9; ++Index)
        {
            Channels[Index]->SetDefault(Values[Index]);
        }
    }

    TArrayView<FMovieSceneDoubleChannel*> TransformChannels(UMovieScene3DTransformSection* Section)
    {
        return Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
    }

    /** Grows the playback range so a key at Tick is inside it; returns true when it changed. */
    bool ExtendPlaybackTo(UMovieScene* MovieScene, FFrameNumber Tick)
    {
        const FFrameNumber Start = PlaybackStart(MovieScene);
        const FFrameNumber End = UE::MovieScene::DiscreteExclusiveUpper(MovieScene->GetPlaybackRange());
        if (Tick < End)
        {
            return false;
        }
        MovieScene->SetPlaybackRange(Start, (Tick - Start).Value + 1);
        return true;
    }

    TSharedPtr<FJsonObject> MakeSequenceJson(ULevelSequence* Sequence)
    {
        UMovieScene* MovieScene = Sequence->GetMovieScene();
        auto Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
        Obj->SetBoolField(TEXT("transient"), Sequence->GetPackage() == GetTransientPackage());
        Obj->SetNumberField(TEXT("display_rate"), MovieScene->GetDisplayRate().AsDecimal());
        Obj->SetNumberField(TEXT("playback_start"), MCPSequencer::TickToDisplay(MovieScene, PlaybackStart(MovieScene)));
        Obj->SetNumberField(TEXT("playback_end"), MCPSequencer::TickToDisplay(MovieScene, UE::MovieScene::DiscreteExclusiveUpper(MovieScene->GetPlaybackRange())));
        return Obj;
    }

    /** The camera-cut section's camera at Tick, if any. */
    FGuid CameraAt(UMovieScene* MovieScene, FFrameNumber Tick)
    {
        if (const UMovieSceneCameraCutTrack* Track = Cast<UMovieSceneCameraCutTrack>(MovieScene->GetCameraCutTrack()))
        {
            for (UMovieSceneSection* Section : Track->GetAllSections())
            {
                if (const UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(Section))
                {
                    if (Cut->IsActive() && Cut->GetRange().Contains(Tick))
                    {
                        return Cut->GetCameraBindingID().GetGuid();
                    }
                }
            }
        }
        return FGuid();
    }

    /** Fields of capture_frame that pass straight through to scene.capture_view. */
    const TCHAR* const CapturePassThrough[] = {
        TEXT("width"), TEXT("height"), TEXT("path"), TEXT("marks"), TEXT("warmup"), TEXT("game_view"),
        TEXT("hide_actors"), TEXT("respect_editor_hidden"), TEXT("stream_textures"), TEXT("stream_seconds"),
    };
}

void FSequenceAuthoringCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    using namespace MCPSequenceAuthoring;

    // sequence.create — a transient Level Sequence (kept alive by the bridge, never saved, gone when the
    // editor closes). Params: name (default "MCPSequence"), fps (display rate, default 30), duration
    // (seconds, default 5). Returns { sequence_path, transient, display_rate, playback_start, playback_end }.
    // The path works with every sequencer.* / sequence.* command.
    Registry.Register(TEXT("sequence.create"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FString Name = TEXT("MCPSequence");
            double Fps = 30.0, Duration = 5.0;
            if (Params.IsValid())
            {
                Params->TryGetStringField(TEXT("name"), Name);
                Params->TryGetNumberField(TEXT("fps"), Fps);
                Params->TryGetNumberField(TEXT("duration"), Duration);
            }
            if (Fps <= 0.0 || Duration <= 0.0)
            {
                Fail(OutError, TEXT("fps and duration must be > 0"));
                return nullptr;
            }

            UPackage* Package = GetTransientPackage();
            ULevelSequence* Sequence = NewObject<ULevelSequence>(Package,
                MakeUniqueObjectName(Package, ULevelSequence::StaticClass(), FName(*Name)), RF_Transient | RF_Transactional);
            Sequence->Initialize();
            UMovieScene* MovieScene = Sequence->GetMovieScene();
            if (!MovieScene)
            {
                Fail(OutError, TEXT("ULevelSequence::Initialize created no movie scene"), MCPProtocol::FMCPError::InternalError);
                return nullptr;
            }
            MovieScene->SetDisplayRate(FFrameRate(FMath::Max(1, FMath::RoundToInt(Fps)), 1));
            MovieScene->SetPlaybackRange(FFrameNumber(0), MovieScene->GetTickResolution().AsFrameTime(Duration).RoundToFrame().Value);
            TransientSequences().Emplace(Sequence);
            return MakeSequenceJson(Sequence);
        });

    // sequence.discard — releases a transient sequence made by sequence.create. Params: sequence_path.
    Registry.Register(TEXT("sequence.discard"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            ULevelSequence* Sequence = MCPSequencer::LoadSequence(Params, OutError);
            if (!Sequence) return nullptr;
            const int32 Removed = TransientSequences().RemoveAll([Sequence](const TStrongObjectPtr<ULevelSequence>& Held) { return Held.Get() == Sequence; });
            if (Removed == 0)
            {
                Fail(OutError, TEXT("Not a transient sequence made by sequence.create — saved sequences are not discarded here"));
                return nullptr;
            }
            Sequence->MarkAsGarbage();
            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("discarded"), true);
            return Result;
        });

    // sequence.add_track — Params: sequence_path, kind ("transform" | "animation" | "camera_cut").
    // The binding: binding (existing name/GUID), or actor (level actor -> possessable), or spawn (asset /
    // actor class -> spawnable, e.g. a skeletal mesh or /Script/CinematicCamera.CineCameraActor); name
    // names a new one. camera_cut takes camera / camera_actor / camera_spawn / camera_name instead.
    // transform: location / rotation / scale add a first key at start (else a possessable keeps its
    // current transform as the channel default). animation: anim (+ row). Times: start_frame or start_time.
    // Returns { sequence_path, kind, binding, binding_guid, binding_how, track, section_start, warnings }.
    // A saved sequence becomes dirty (asset.save after approval); a transient one has nothing to save.
    Registry.Register(TEXT("sequence.add_track"),
        [&Registry](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            ULevelSequence* Sequence = MCPSequencer::LoadSequence(Params, OutError);
            if (!Sequence) return nullptr;
            UMovieScene* MovieScene = Sequence->GetMovieScene();
            FString Kind;
            if (!RequireString(Params, TEXT("kind"), Kind, OutError)) return nullptr;
            Kind = Kind.ToLower();
            if (Kind != TEXT("transform") && Kind != TEXT("animation") && Kind != TEXT("camera_cut"))
            {
                Fail(OutError, TEXT("kind is transform, animation or camera_cut"));
                return nullptr;
            }

            UAnimSequenceBase* Anim = nullptr;
            if (Kind == TEXT("animation"))
            {
                FString AnimPath;
                if (!RequireString(Params, TEXT("anim"), AnimPath, OutError)) return nullptr;
                Anim = LoadObject<UAnimSequenceBase>(nullptr, *AnimPath);
                if (!Anim)
                {
                    Fail(OutError, FString::Printf(TEXT("Could not load animation %s"), *AnimPath));
                    return nullptr;
                }
            }
            FVector Location = FVector::ZeroVector, Scale = FVector::OneVector;
            FRotator Rotation = FRotator::ZeroRotator;
            if (!ReadVector3(Params, TEXT("location"), Location, OutError) || !ReadRotator(Params, TEXT("rotation"), Rotation, OutError)
                || !ReadVector3(Params, TEXT("scale"), Scale, OutError))
            {
                return nullptr;
            }

            FGuid Guid;
            FString How;
            AActor* Possessed = nullptr;
            if (!ResolveOrCreateBinding(Registry, Sequence, Params, Kind == TEXT("camera_cut") ? TEXT("camera") : TEXT(""), Guid, How, Possessed, OutError))
            {
                return nullptr;
            }

            const FFrameNumber Start = ReadTick(MovieScene, Params, TEXT("start_frame"), TEXT("start_time"), PlaybackStart(MovieScene));
            Sequence->Modify();
            MovieScene->Modify();

            TArray<FString> Warnings;
            FString TrackClass;
            if (Kind == TEXT("transform"))
            {
                bool bCreated = false;
                UMovieScene3DTransformSection* Section = FindOrAddTransformSection(MovieScene, Guid, bCreated);
                if (!Section)
                {
                    Fail(OutError, TEXT("Could not add a transform track"), MCPProtocol::FMCPError::InternalError);
                    return nullptr;
                }
                Section->Modify();
                TArrayView<FMovieSceneDoubleChannel*> Channels = TransformChannels(Section);
                if (Channels.Num() < 9)
                {
                    Fail(OutError, TEXT("Transform section has fewer than 9 channels"), MCPProtocol::FMCPError::InternalError);
                    return nullptr;
                }
                if (bCreated && Possessed)
                {
                    SetTransformDefaults(Channels, Possessed->GetActorTransform());
                }
                const bool bHasLocation = Params->HasField(TEXT("location"));
                const bool bHasRotation = Params->HasField(TEXT("rotation"));
                const bool bHasScale = Params->HasField(TEXT("scale"));
                if (bHasLocation || bHasRotation || bHasScale)
                {
                    AddTransformKey(Channels, Start, bHasLocation ? &Location : nullptr, bHasRotation ? &Rotation : nullptr, bHasScale ? &Scale : nullptr, EKeyInterp::Cubic);
                }
                TrackClass = UMovieScene3DTransformTrack::StaticClass()->GetName();
            }
            else if (Kind == TEXT("animation"))
            {
                UMovieSceneSkeletalAnimationTrack* Track = MovieScene->FindTrack<UMovieSceneSkeletalAnimationTrack>(Guid);
                if (!Track)
                {
                    Track = MovieScene->AddTrack<UMovieSceneSkeletalAnimationTrack>(Guid);
                }
                double Row = 0.0;
                Params->TryGetNumberField(TEXT("row"), Row);
                if (!Track || !Track->AddNewAnimationOnRow(Start, Anim, static_cast<int32>(Row)))
                {
                    Fail(OutError, TEXT("Could not add the animation section"), MCPProtocol::FMCPError::InternalError);
                    return nullptr;
                }
                const FFrameNumber End = Start + MovieScene->GetTickResolution().AsFrameTime(Anim->GetPlayLength()).RoundToFrame();
                if (ExtendPlaybackTo(MovieScene, End - 1))
                {
                    Warnings.Add(TEXT("playback range extended to fit the clip"));
                }
                TrackClass = UMovieSceneSkeletalAnimationTrack::StaticClass()->GetName();
            }
            else
            {
                UMovieSceneCameraCutTrack* Track = Cast<UMovieSceneCameraCutTrack>(MovieScene->GetCameraCutTrack());
                if (!Track)
                {
                    Track = Cast<UMovieSceneCameraCutTrack>(MovieScene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass()));
                }
                if (!Track || !Track->AddNewCameraCut(UE::MovieScene::FRelativeObjectBindingID(Guid), Start))
                {
                    Fail(OutError, TEXT("Could not add the camera cut"), MCPProtocol::FMCPError::InternalError);
                    return nullptr;
                }
                TrackClass = UMovieSceneCameraCutTrack::StaticClass()->GetName();
            }

            Sequence->MarkPackageDirty();

            auto Result = MakeSequenceJson(Sequence);
            Result->SetStringField(TEXT("kind"), Kind);
            Result->SetStringField(TEXT("binding"), MCPSequencer::BindingName(MovieScene, Guid));
            Result->SetStringField(TEXT("binding_guid"), Guid.ToString());
            Result->SetStringField(TEXT("binding_how"), How);
            Result->SetStringField(TEXT("track"), TrackClass);
            Result->SetNumberField(TEXT("section_start"), MCPSequencer::TickToDisplay(MovieScene, Start));
            if (Warnings.Num() > 0)
            {
                TArray<TSharedPtr<FJsonValue>> Values;
                for (const FString& W : Warnings) Values.Add(MakeShared<FJsonValueString>(W));
                Result->SetArrayField(TEXT("warnings"), Values);
            }
            return Result;
        });

    // sequence.set_keys — transform keys on a binding (the track is created when missing).
    // Params: sequence_path, binding, keys ([{frame | time, location?, rotation? [pitch,yaw,roll],
    // scale?}]), interpolation ("cubic" default | "linear" | "constant"). Keys past the end extend the
    // playback range. Returns { binding, keys_added, key_count, playback_end, extended }.
    Registry.Register(TEXT("sequence.set_keys"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            ULevelSequence* Sequence = MCPSequencer::LoadSequence(Params, OutError);
            if (!Sequence) return nullptr;
            UMovieScene* MovieScene = Sequence->GetMovieScene();
            FString Ref;
            if (!RequireString(Params, TEXT("binding"), Ref, OutError)) return nullptr;
            FGuid Guid;
            if (!MCPSequencer::ResolveBinding(Sequence, Ref, Guid, OutError)) return nullptr;

            const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
            if (!Params->TryGetArrayField(TEXT("keys"), Keys) || !Keys || Keys->Num() == 0)
            {
                Fail(OutError, TEXT("keys ([{frame | time, location?, rotation?, scale?}]) is required"));
                return nullptr;
            }
            FString InterpName;
            Params->TryGetStringField(TEXT("interpolation"), InterpName);
            const EKeyInterp Interp = InterpName.Equals(TEXT("linear"), ESearchCase::IgnoreCase) ? EKeyInterp::Linear
                : InterpName.Equals(TEXT("constant"), ESearchCase::IgnoreCase) ? EKeyInterp::Constant : EKeyInterp::Cubic;

            // Parse every key before touching the section.
            struct FKey { FFrameNumber Tick; TOptional<FVector> Location; TOptional<FRotator> Rotation; TOptional<FVector> Scale; };
            TArray<FKey> Parsed;
            for (int32 Index = 0; Index < Keys->Num(); ++Index)
            {
                const TSharedPtr<FJsonObject>* KeyObj = nullptr;
                if (!(*Keys)[Index]->TryGetObject(KeyObj) || !KeyObj || (!(*KeyObj)->HasField(TEXT("frame")) && !(*KeyObj)->HasField(TEXT("time"))))
                {
                    Fail(OutError, FString::Printf(TEXT("keys[%d] needs frame or time"), Index));
                    return nullptr;
                }
                FKey Key;
                Key.Tick = ReadTick(MovieScene, *KeyObj, TEXT("frame"), TEXT("time"), FFrameNumber(0));
                FVector V; FRotator R;
                if ((*KeyObj)->HasField(TEXT("location"))) { if (!ReadVector3(*KeyObj, TEXT("location"), V, OutError)) return nullptr; Key.Location = V; }
                if ((*KeyObj)->HasField(TEXT("rotation"))) { R = FRotator::ZeroRotator; if (!ReadRotator(*KeyObj, TEXT("rotation"), R, OutError)) return nullptr; Key.Rotation = R; }
                if ((*KeyObj)->HasField(TEXT("scale")))    { if (!ReadVector3(*KeyObj, TEXT("scale"), V, OutError)) return nullptr; Key.Scale = V; }
                if (!Key.Location && !Key.Rotation && !Key.Scale)
                {
                    Fail(OutError, FString::Printf(TEXT("keys[%d] has no location, rotation or scale"), Index));
                    return nullptr;
                }
                Parsed.Add(Key);
            }

            Sequence->Modify();
            MovieScene->Modify();
            bool bCreated = false;
            UMovieScene3DTransformSection* Section = FindOrAddTransformSection(MovieScene, Guid, bCreated);
            if (!Section)
            {
                Fail(OutError, TEXT("Could not add a transform track"), MCPProtocol::FMCPError::InternalError);
                return nullptr;
            }
            Section->Modify();
            TArrayView<FMovieSceneDoubleChannel*> Channels = TransformChannels(Section);
            if (Channels.Num() < 9)
            {
                Fail(OutError, TEXT("Transform section has fewer than 9 channels"), MCPProtocol::FMCPError::InternalError);
                return nullptr;
            }

            bool bExtended = false;
            for (const FKey& Key : Parsed)
            {
                AddTransformKey(Channels, Key.Tick, Key.Location ? &Key.Location.GetValue() : nullptr,
                    Key.Rotation ? &Key.Rotation.GetValue() : nullptr, Key.Scale ? &Key.Scale.GetValue() : nullptr, Interp);
                if (!Section->GetRange().Contains(Key.Tick))
                {
                    Section->ExpandToFrame(Key.Tick);
                }
                bExtended |= ExtendPlaybackTo(MovieScene, Key.Tick);
            }
            Sequence->MarkPackageDirty();

            int32 MaxKeys = 0;
            for (FMovieSceneDoubleChannel* Channel : Channels)
            {
                MaxKeys = FMath::Max(MaxKeys, Channel->GetNumKeys());
            }
            auto Result = MakeSequenceJson(Sequence);
            Result->SetStringField(TEXT("binding"), MCPSequencer::BindingName(MovieScene, Guid));
            Result->SetNumberField(TEXT("keys_added"), Parsed.Num());
            Result->SetNumberField(TEXT("key_count"), MaxKeys);
            Result->SetBoolField(TEXT("extended"), bExtended);
            return Result;
        });

    // sequence.capture_frame — evaluates one frame in the editor world and renders it from the camera:
    // camera (binding), else an explicit location + look_at/rotation + fov, else the active camera cut
    // at that frame. Params: sequence_path, frame | time (default playback start), plus scene.capture_view's
    // width, height, path, marks, warmup, game_view, hide_actors, ... Every actor the sequence animated
    // is restored and spawnables are removed afterwards (FinishCompletionStateOverride = ForceRestoreState).
    // Returns scene.capture_view's result + { frame, camera: {binding, location, rotation, fov},
    // level_dirty, dirtied_packages, map_dirty }.
    Registry.Register(TEXT("sequence.capture_frame"),
        [&Registry](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            ULevelSequence* Sequence = MCPSequencer::LoadSequence(Params, OutError);
            if (!Sequence) return nullptr;
            UWorld* World = UnrealMCPBridge::Scene::GetEditorWorld(OutError);
            if (!World) return nullptr;
            UMovieScene* MovieScene = Sequence->GetMovieScene();
            const FFrameNumber Tick = ReadTick(MovieScene, Params, TEXT("frame"), TEXT("time"), PlaybackStart(MovieScene));
            const double DisplayFrame = MCPSequencer::TickToDisplay(MovieScene, Tick);

            FGuid CameraGuid;
            FString CameraRef;
            if (Params->TryGetStringField(TEXT("camera"), CameraRef) && !CameraRef.IsEmpty())
            {
                if (!MCPSequencer::ResolveBinding(Sequence, CameraRef, CameraGuid, OutError)) return nullptr;
            }
            else if (!Params->HasField(TEXT("location")))
            {
                // An explicit location is the caller's camera; only without one does the camera cut decide.
                CameraGuid = CameraAt(MovieScene, Tick);
            }
            if (!CameraGuid.IsValid() && !Params->HasField(TEXT("location")))
            {
                Fail(OutError, TEXT("No camera: add a camera_cut track, pass camera (binding), or pass location + look_at/rotation"));
                return nullptr;
            }

            const UnrealMCPBridge::Scene::FDirtySnapshot Before = UnrealMCPBridge::Scene::SnapshotDirtyPackages();

            // The player actor is never saved, like scene.spawn_temp's actors.
            FActorSpawnParameters SpawnParams;
            SpawnParams.ObjectFlags |= RF_Transient;
            SpawnParams.bCreateActorPackage = false;
            SpawnParams.OverrideLevel = World->PersistentLevel;
            SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            SpawnParams.bNoFail = true;
            ALevelSequenceActor* PlayerActor = World->SpawnActor<ALevelSequenceActor>(SpawnParams);
            if (!PlayerActor)
            {
                Fail(OutError, TEXT("Could not spawn a level sequence actor"), MCPProtocol::FMCPError::InternalError);
                return nullptr;
            }
            PlayerActor->Tags.AddUnique(UnrealMCPBridge::Scene::TempActorTag);

            FMovieSceneSequencePlaybackSettings Settings;
            Settings.bDisableCameraCuts = true;
            Settings.FinishCompletionStateOverride = EMovieSceneCompletionModeOverride::ForceRestoreState;
            PlayerActor->PlaybackSettings = Settings;
            ULevelSequencePlayer* Player = PlayerActor->GetSequencePlayer();
            if (!Player)
            {
                World->EditorDestroyActor(PlayerActor, /*bShouldModifyLevel=*/false);
                Fail(OutError, TEXT("The level sequence actor has no player"), MCPProtocol::FMCPError::InternalError);
                return nullptr;
            }
            Player->SetPlaybackSettings(Settings);
            // ALevelSequenceActor::InitializePlayer only runs in a game world; initialize the player directly.
            Player->Initialize(Sequence, World->PersistentLevel, FLevelSequenceCameraSettings());
            Player->SetPlaybackPosition(FMovieSceneSequencePlaybackParams(FFrameTime::FromDecimal(DisplayFrame), EUpdatePositionMethod::Jump));

            // Evaluate skeletal poses now; the anim track only set the clip position.
            const UMovieScene* ConstMovieScene = MovieScene;
            for (const FMovieSceneBinding& Binding : ConstMovieScene->GetBindings())
            {
                for (TWeakObjectPtr<> Bound : Player->FindBoundObjects(Binding.GetObjectGuid(), MovieSceneSequenceID::Root))
                {
                    if (AActor* Actor = Cast<AActor>(Bound.Get()))
                    {
                        TInlineComponentArray<USkeletalMeshComponent*> Meshes(Actor);
                        for (USkeletalMeshComponent* Mesh : Meshes)
                        {
                            Mesh->TickAnimation(0.f, /*bNeedsValidRootMotion=*/false);
                            Mesh->RefreshBoneTransforms();
                            Mesh->RefreshFollowerComponents();
                        }
                    }
                }
            }

            auto CaptureParams = MakeShared<FJsonObject>();
            for (const TCHAR* Field : CapturePassThrough)
            {
                if (TSharedPtr<FJsonValue> Value = Params->TryGetField(Field))
                {
                    CaptureParams->SetField(Field, Value);
                }
            }

            auto CameraJson = MakeShared<FJsonObject>();
            bool bHaveCamera = false;
            if (CameraGuid.IsValid())
            {
                for (TWeakObjectPtr<> Bound : Player->FindBoundObjects(CameraGuid, MovieSceneSequenceID::Root))
                {
                    UCameraComponent* Camera = Cast<UCameraComponent>(Bound.Get());
                    if (AActor* Actor = Cast<AActor>(Bound.Get()))
                    {
                        Camera = Actor->FindComponentByClass<UCameraComponent>();
                    }
                    if (!Camera)
                    {
                        continue;
                    }
                    const UCineCameraComponent* Cine = Cast<UCineCameraComponent>(Camera);
                    const float Fov = Cine ? Cine->GetHorizontalFieldOfView() : Camera->FieldOfView;
                    CaptureParams->SetField(TEXT("location"), MakeVector3JsonArray(Camera->GetComponentLocation()));
                    CaptureParams->SetField(TEXT("rotation"), MakeRotatorJsonArray(Camera->GetComponentRotation()));
                    CaptureParams->SetNumberField(TEXT("fov"), Fov);
                    CameraJson->SetStringField(TEXT("binding"), MCPSequencer::BindingName(MovieScene, CameraGuid));
                    bHaveCamera = true;
                    break;
                }
            }
            if (!bHaveCamera)
            {
                if (CameraGuid.IsValid() && !Params->HasField(TEXT("location")))
                {
                    Player->Stop();
                    World->EditorDestroyActor(PlayerActor, /*bShouldModifyLevel=*/false);
                    Fail(OutError, FString::Printf(TEXT("Camera binding '%s' resolved to nothing with a camera component at frame %.1f"),
                        *MCPSequencer::BindingName(MovieScene, CameraGuid), DisplayFrame));
                    return nullptr;
                }
                for (const TCHAR* Field : { TEXT("location"), TEXT("look_at"), TEXT("rotation"), TEXT("fov") })
                {
                    if (TSharedPtr<FJsonValue> Value = Params->TryGetField(Field))
                    {
                        CaptureParams->SetField(Field, Value);
                    }
                }
                CameraJson->SetStringField(TEXT("binding"), FString());
            }

            TSharedPtr<FJsonObject> Result;
            MCPProtocol::FMCPError CaptureError;
            const bool bCaptured = Registry.Dispatch(TEXT("scene.capture_view"), CaptureParams, Result, CaptureError) && Result.IsValid();

            // Restore everything the evaluation touched before reporting.
            Player->Stop();
            World->EditorDestroyActor(PlayerActor, /*bShouldModifyLevel=*/false);

            if (!bCaptured)
            {
                OutError = CaptureError;
                return nullptr;
            }
            CameraJson->SetField(TEXT("location"), CaptureParams->TryGetField(TEXT("location")));
            if (TSharedPtr<FJsonValue> Rotation = CaptureParams->TryGetField(TEXT("rotation")))
            {
                CameraJson->SetField(TEXT("rotation"), Rotation);
            }
            CameraJson->SetField(TEXT("fov"), Result->TryGetField(TEXT("fov")));
            Result->SetObjectField(TEXT("camera"), CameraJson);
            Result->SetNumberField(TEXT("frame"), DisplayFrame);
            UnrealMCPBridge::Scene::WriteDirtyReport(World, Before, Result);
            return Result;
        });
}
