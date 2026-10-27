#include "Commands/SequencerCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "Commands/SequencerHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "MovieSceneToolHelpers.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneBindingProxy.h"
#include "MovieSceneBindingReferences.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "MovieSceneTimeHelpers.h"
#include "Bindings/MovieSceneCustomBinding.h"
#include "Bindings/MovieSceneSpawnableBinding.h"
#include "Bindings/MovieSceneSpawnableActorBinding.h"
#include "Channels/MovieSceneChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"

#include "Animation/AnimSequence.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "Exporters/AnimSeqExportOption.h"
#include "Factories/AnimSequenceFactory.h"
#include "AssetToolsModule.h"
#include "Misc/PackageName.h"
#include "SequencerTools.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

// 이름 공간을 따로 둔다 — 유니티 빌드에서 다른 명령 파일의 익명 이름 공간 헬퍼(LoadAnim 등)와 합쳐져
// 재정의 오류가 나지 않게.
namespace MCPSequencer
{
    ULevelSequence* LoadSequence(const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        FString Path;
        if (!UnrealMCPBridge::Json::RequireString(Params, TEXT("sequence_path"), Path, OutError))
        {
            return nullptr;
        }
        ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *Path);
        if (!Sequence || !Sequence->GetMovieScene())
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("Could not load level sequence: %s"), *Path);
            return nullptr;
        }
        return Sequence;
    }

    // 틱(내부 해상도) → 표시 프레임. 시퀀서 타임라인에 보이는 숫자가 표시 프레임이다.
    double TickToDisplay(const UMovieScene* MovieScene, FFrameNumber Tick)
    {
        return FFrameRate::TransformTime(FFrameTime(Tick), MovieScene->GetTickResolution(), MovieScene->GetDisplayRate()).AsDecimal();
    }

    FFrameNumber DisplayToTick(const UMovieScene* MovieScene, double DisplayFrame)
    {
        return FFrameRate::TransformTime(FFrameTime::FromDecimal(DisplayFrame), MovieScene->GetDisplayRate(), MovieScene->GetTickResolution()).RoundToFrame();
    }

    // 범위 끝은 열려 있을 수 있다(무한 섹션). 열린 쪽은 null. end_frame 은 엔진 규칙대로 배타적 끝이다.
    void SetRangeFields(const TSharedPtr<FJsonObject>& Obj, const UMovieScene* MovieScene, const TRange<FFrameNumber>& Range)
    {
        if (Range.GetLowerBound().IsClosed())
        {
            Obj->SetNumberField(TEXT("start_frame"), TickToDisplay(MovieScene, UE::MovieScene::DiscreteInclusiveLower(Range)));
        }
        else
        {
            Obj->SetField(TEXT("start_frame"), MakeShared<FJsonValueNull>());
        }
        if (Range.GetUpperBound().IsClosed())
        {
            Obj->SetNumberField(TEXT("end_frame"), TickToDisplay(MovieScene, UE::MovieScene::DiscreteExclusiveUpper(Range)));
        }
        else
        {
            Obj->SetField(TEXT("end_frame"), MakeShared<FJsonValueNull>());
        }
    }

    FString BindingName(UMovieScene* MovieScene, const FGuid& Guid)
    {
        if (const FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(Guid))
        {
            return Spawnable->GetName();
        }
        if (const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Guid))
        {
            return Possessable->GetName();
        }
        return MovieScene->GetObjectDisplayName(Guid).ToString();
    }

    // binding 은 GUID 문자열이나 이름(스폰어블/포제서블 이름 또는 표시 이름). 이름이 겹치면 실패하고
    // 후보 GUID 를 알려 준다 — 엉뚱한 바인딩을 굽는 것보다 낫다.
    bool ResolveBinding(ULevelSequence* Sequence, const FString& Ref, FGuid& OutGuid, MCPProtocol::FMCPError& OutError)
    {
        UMovieScene* MovieScene = Sequence->GetMovieScene();
        FGuid Parsed;
        if (FGuid::Parse(Ref, Parsed) && MovieScene->FindBinding(Parsed))
        {
            OutGuid = Parsed;
            return true;
        }

        TArray<FGuid> Matches;
        for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
        {
            const FGuid& Guid = Binding.GetObjectGuid();
            if (BindingName(MovieScene, Guid) == Ref || MovieScene->GetObjectDisplayName(Guid).ToString() == Ref)
            {
                Matches.AddUnique(Guid);
            }
        }
        if (Matches.Num() == 1)
        {
            OutGuid = Matches[0];
            return true;
        }

        OutError.Code = MCPProtocol::FMCPError::InvalidParams;
        if (Matches.Num() == 0)
        {
            OutError.Message = FString::Printf(TEXT("No binding named '%s' in %s (see sequencer.list_bindings)"), *Ref, *Sequence->GetName());
        }
        else
        {
            TArray<FString> Ids;
            for (const FGuid& Guid : Matches) { Ids.Add(Guid.ToString()); }
            OutError.Message = FString::Printf(TEXT("Binding name '%s' is ambiguous — pass one of these GUIDs: %s"), *Ref, *FString::Join(Ids, TEXT(", ")));
        }
        return false;
    }

    // 바인딩이 실제로 무엇을 붙잡는지 — 스폰어블이면 템플릿, 포제서블이면 클래스.
    void DescribeBindingKind(ULevelSequence* Sequence, const FGuid& Guid, const TSharedPtr<FJsonObject>& Obj)
    {
        UMovieScene* MovieScene = Sequence->GetMovieScene();
        FString Kind = TEXT("unknown");
        FString TemplateClass;
        FString CustomClass;

        if (const FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(Guid))
        {
            Kind = TEXT("spawnable");
            if (const UObject* Template = Spawnable->GetObjectTemplate())
            {
                TemplateClass = Template->GetClass()->GetName();
            }
        }
        else if (const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Guid))
        {
            Kind = TEXT("possessable");
            if (const UClass* Class = Possessable->GetPossessedObjectClass())
            {
                TemplateClass = Class->GetName();
            }
            if (const FMovieSceneBindingReferences* Refs = Sequence->GetBindingReferences())
            {
                if (const UMovieSceneCustomBinding* Custom = Refs->GetCustomBinding(Guid, 0))
                {
                    CustomClass = Custom->GetClass()->GetName();
                    if (Custom->IsA<UMovieSceneSpawnableBindingBase>())
                    {
                        Kind = TEXT("spawnable");
                    }
                    if (UMovieSceneSpawnableActorBindingBase* ActorBinding = Cast<UMovieSceneSpawnableActorBindingBase>(const_cast<UMovieSceneCustomBinding*>(Custom)))
                    {
                        if (const UObject* Template = ActorBinding->GetObjectTemplate())
                        {
                            TemplateClass = Template->GetClass()->GetName();
                        }
                    }
                    if (const UClass* Bound = Custom->GetBoundObjectClass())
                    {
                        if (TemplateClass.IsEmpty()) { TemplateClass = Bound->GetName(); }
                    }
                }
            }
        }

        Obj->SetStringField(TEXT("kind"), Kind);
        Obj->SetStringField(TEXT("bound_class"), TemplateClass);
        Obj->SetStringField(TEXT("custom_binding"), CustomClass);
    }

    // 바인딩 템플릿에서 스켈레탈 메시 컴포넌트를 찾는다(베이크 대상 스켈레톤 추정용).
    USkeletalMeshComponent* FindTemplateSkelMesh(ULevelSequence* Sequence, const FGuid& Guid)
    {
        UObject* Template = nullptr;
        UMovieScene* MovieScene = Sequence->GetMovieScene();
        if (FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(Guid))
        {
            Template = Spawnable->GetObjectTemplate();
        }
        // ULevelSequence 는 const 판만 오버라이드해서 비 const 판이 가려진다 — 기반 클래스로 부른다.
        else if (FMovieSceneBindingReferences* Refs = static_cast<UMovieSceneSequence*>(Sequence)->GetBindingReferences())
        {
            if (UMovieSceneSpawnableActorBindingBase* ActorBinding = Cast<UMovieSceneSpawnableActorBindingBase>(Refs->GetCustomBinding(Guid, 0)))
            {
                Template = ActorBinding->GetObjectTemplate();
            }
        }
        if (AActor* Actor = Cast<AActor>(Template))
        {
            return Actor->FindComponentByClass<USkeletalMeshComponent>();
        }
        return nullptr;
    }

    TSharedPtr<FJsonObject> MakeSectionJson(const UMovieScene* MovieScene, const UMovieSceneSection* Section, int32 Index)
    {
        auto Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("index"), Index);
        Obj->SetStringField(TEXT("class"), Section->GetClass()->GetName());
        Obj->SetStringField(TEXT("object_name"), Section->GetName());
        Obj->SetNumberField(TEXT("row_index"), Section->GetRowIndex());
        Obj->SetNumberField(TEXT("overlap_priority"), Section->GetOverlapPriority());
        const FOptionalMovieSceneBlendType Blend = Section->GetBlendType();
        Obj->SetStringField(TEXT("blend_type"), Blend.IsValid()
            ? StaticEnum<EMovieSceneBlendType>()->GetNameStringByValue(static_cast<int64>(Blend.Get()))
            : FString(TEXT("(none)")));
        Obj->SetBoolField(TEXT("active"), Section->IsActive());
        Obj->SetBoolField(TEXT("locked"), Section->IsLocked());
        SetRangeFields(Obj, MovieScene, Section->GetRange());

        // 채널·키 수 — 레이어 둘을 비교할 때 "비어 있는 Additive 레이어"를 바로 가려낸다.
        int32 NumChannels = 0;
        int32 NumKeys = 0;
        for (const FMovieSceneChannelEntry& Entry : Section->GetChannelProxy().GetAllEntries())
        {
            for (const FMovieSceneChannel* Channel : Entry.GetChannels())
            {
                if (Channel)
                {
                    ++NumChannels;
                    NumKeys += Channel->GetNumKeys();
                }
            }
        }
        Obj->SetNumberField(TEXT("num_channels"), NumChannels);
        Obj->SetNumberField(TEXT("num_keys"), NumKeys);
        return Obj;
    }

    bool TrackMatchesFilter(const UMovieSceneTrack* Track, const FString& Filter)
    {
        if (Filter.IsEmpty())
        {
            return true;
        }
        for (const UClass* It = Track->GetClass(); It; It = It->GetSuperClass())
        {
            if (It->GetName() == Filter || It->GetPathName() == Filter)
            {
                return true;
            }
        }
        // "ControlRig" 처럼 일부만 줘도 잡히게.
        return Track->GetClass()->GetName().Contains(Filter);
    }

    TSharedPtr<FJsonObject> MakeTrackJson(const UMovieScene* MovieScene, const UMovieSceneTrack* Track, bool bIncludeSections)
    {
        auto Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("class"), Track->GetClass()->GetName());
#if WITH_EDITORONLY_DATA
        Obj->SetStringField(TEXT("display_name"), Track->GetDisplayName().ToString());
#endif
        const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
        Obj->SetNumberField(TEXT("num_sections"), Sections.Num());
        if (bIncludeSections)
        {
            TArray<TSharedPtr<FJsonValue>> Out;
            for (int32 i = 0; i < Sections.Num(); ++i)
            {
                if (Sections[i])
                {
                    Out.Add(MakeShared<FJsonValueObject>(MakeSectionJson(MovieScene, Sections[i], i)));
                }
            }
            Obj->SetArrayField(TEXT("sections"), Out);
        }
        return Obj;
    }

    // { "PropName": "ImportText 값" } 을 대상 객체에 넣는다(animnotify.add 의 properties 와 같은 방식).
    bool ApplyImportTextProperties(UObject* Target, const TSharedPtr<FJsonObject>& Props, MCPProtocol::FMCPError& OutError)
    {
        if (!Props.IsValid())
        {
            return true;
        }
        for (const auto& Pair : Props->Values)
        {
            FProperty* Property = FindFProperty<FProperty>(Target->GetClass(), *Pair.Key);
            if (!Property)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("%s has no property '%s'"), *Target->GetClass()->GetName(), *Pair.Key);
                return false;
            }
            FString ValueText;
            bool bBool = false;
            double Number = 0.0;
            if (Pair.Value->TryGetString(ValueText)) {}
            else if (Pair.Value->TryGetBool(bBool)) { ValueText = bBool ? TEXT("True") : TEXT("False"); }
            else if (Pair.Value->TryGetNumber(Number)) { ValueText = FString::SanitizeFloat(Number); }
            else
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("property '%s' must be a string (ImportText form), bool or number"), *Pair.Key);
                return false;
            }
            void* Addr = Property->ContainerPtrToValuePtr<void>(Target);
            if (!Property->ImportText_Direct(*ValueText, Addr, Target, PPF_None))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("could not parse '%s' for property '%s'"), *ValueText, *Pair.Key);
                return false;
            }
        }
        return true;
    }

    TSharedPtr<FJsonObject> MakeMarkJson(const UMovieScene* MovieScene, const FMovieSceneMarkedFrame& Mark, int32 Index)
    {
        auto Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("index"), Index);
        Obj->SetNumberField(TEXT("frame"), TickToDisplay(MovieScene, Mark.FrameNumber));
        Obj->SetNumberField(TEXT("tick"), Mark.FrameNumber.Value);
        Obj->SetNumberField(TEXT("seconds"), MovieScene->GetTickResolution().AsSeconds(FFrameTime(Mark.FrameNumber)));
        Obj->SetStringField(TEXT("label"), Mark.Label);
#if WITH_EDITORONLY_DATA
        Obj->SetStringField(TEXT("comment"), Mark.Comment);
        Obj->SetBoolField(TEXT("use_custom_color"), Mark.bUseCustomColor);
        TArray<TSharedPtr<FJsonValue>> Color;
        Color.Add(MakeShared<FJsonValueNumber>(Mark.CustomColor.R));
        Color.Add(MakeShared<FJsonValueNumber>(Mark.CustomColor.G));
        Color.Add(MakeShared<FJsonValueNumber>(Mark.CustomColor.B));
        Color.Add(MakeShared<FJsonValueNumber>(Mark.CustomColor.A));
        Obj->SetArrayField(TEXT("color"), Color);
#endif
        return Obj;
    }

    TSharedPtr<FJsonObject> MakeMarksResult(const UMovieScene* MovieScene)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        const TArray<FMovieSceneMarkedFrame>& Marks = MovieScene->GetMarkedFrames();
        for (int32 i = 0; i < Marks.Num(); ++i)
        {
            Out.Add(MakeShared<FJsonValueObject>(MakeMarkJson(MovieScene, Marks[i], i)));
        }
        auto Result = MakeShared<FJsonObject>();
        Result->SetArrayField(TEXT("marks"), Out);
        Result->SetBoolField(TEXT("locked"), MovieScene->AreMarkedFramesLocked());
        Result->SetNumberField(TEXT("display_rate"), MovieScene->GetDisplayRate().AsDecimal());
        return Result;
    }
}

void FSequencerCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    using namespace MCPSequencer;

    // sequencer.list_bindings — every binding in a level sequence with its kind and tracks.
    // Params: sequence_path. Returns { display_rate, tick_resolution, playback_start, playback_end,
    // bindings: [ { name, display_name, guid, kind, bound_class, custom_binding, tracks: [ { class,
    // display_name, num_sections } ] } ], tracks: [ global tracks ] }.
    Registry.Register(TEXT("sequencer.list_bindings"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            ULevelSequence* Sequence = LoadSequence(Params, OutError); if (!Sequence) return nullptr;
            UMovieScene* MovieScene = Sequence->GetMovieScene();

            TArray<TSharedPtr<FJsonValue>> Bindings;
            for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
            {
                const FGuid& Guid = Binding.GetObjectGuid();
                auto Obj = MakeShared<FJsonObject>();
                Obj->SetStringField(TEXT("name"), BindingName(MovieScene, Guid));
                Obj->SetStringField(TEXT("display_name"), MovieScene->GetObjectDisplayName(Guid).ToString());
                Obj->SetStringField(TEXT("guid"), Guid.ToString());
                DescribeBindingKind(Sequence, Guid, Obj);
                TArray<TSharedPtr<FJsonValue>> Tracks;
                for (const UMovieSceneTrack* Track : Binding.GetTracks())
                {
                    if (Track) { Tracks.Add(MakeShared<FJsonValueObject>(MakeTrackJson(MovieScene, Track, false))); }
                }
                Obj->SetArrayField(TEXT("tracks"), Tracks);
                Bindings.Add(MakeShared<FJsonValueObject>(Obj));
            }

            TArray<TSharedPtr<FJsonValue>> GlobalTracks;
            for (const UMovieSceneTrack* Track : MovieScene->GetTracks())
            {
                if (Track) { GlobalTracks.Add(MakeShared<FJsonValueObject>(MakeTrackJson(MovieScene, Track, false))); }
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("display_rate"), MovieScene->GetDisplayRate().AsDecimal());
            Result->SetNumberField(TEXT("tick_resolution"), MovieScene->GetTickResolution().AsDecimal());
            auto Playback = MakeShared<FJsonObject>();
            SetRangeFields(Playback, MovieScene, MovieScene->GetPlaybackRange());
            Result->SetField(TEXT("playback_start"), Playback->TryGetField(TEXT("start_frame")));
            Result->SetField(TEXT("playback_end"), Playback->TryGetField(TEXT("end_frame")));
            Result->SetArrayField(TEXT("bindings"), Bindings);
            Result->SetArrayField(TEXT("tracks"), GlobalTracks);
            return Result;
        });

    // sequencer.list_sections — every section of every matching track, Additive layers included.
    // Params: sequence_path, binding (optional name or GUID; omitted = all bindings plus global tracks),
    // track_class (optional; exact class name/path, a superclass, or a substring such as "ControlRig").
    // Returns { tracks: [ { binding, binding_guid, class, display_name, num_sections, sections: [ { index,
    // class, row_index, overlap_priority, blend_type, active, locked, start_frame, end_frame (exclusive,
    // null = open), num_channels, num_keys } ] } ] }.
    Registry.Register(TEXT("sequencer.list_sections"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            ULevelSequence* Sequence = LoadSequence(Params, OutError); if (!Sequence) return nullptr;
            UMovieScene* MovieScene = Sequence->GetMovieScene();

            FString TrackFilter; Params->TryGetStringField(TEXT("track_class"), TrackFilter);
            FString BindingRef; Params->TryGetStringField(TEXT("binding"), BindingRef);
            FGuid OnlyGuid;
            if (!BindingRef.IsEmpty() && !ResolveBinding(Sequence, BindingRef, OnlyGuid, OutError))
            {
                return nullptr;
            }

            TArray<TSharedPtr<FJsonValue>> Out;
            for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
            {
                const FGuid& Guid = Binding.GetObjectGuid();
                if (OnlyGuid.IsValid() && Guid != OnlyGuid)
                {
                    continue;
                }
                for (const UMovieSceneTrack* Track : Binding.GetTracks())
                {
                    if (!Track || !TrackMatchesFilter(Track, TrackFilter))
                    {
                        continue;
                    }
                    TSharedPtr<FJsonObject> Obj = MakeTrackJson(MovieScene, Track, true);
                    Obj->SetStringField(TEXT("binding"), BindingName(MovieScene, Guid));
                    Obj->SetStringField(TEXT("binding_guid"), Guid.ToString());
                    Out.Add(MakeShared<FJsonValueObject>(Obj));
                }
            }
            if (!OnlyGuid.IsValid())
            {
                for (const UMovieSceneTrack* Track : MovieScene->GetTracks())
                {
                    if (!Track || !TrackMatchesFilter(Track, TrackFilter))
                    {
                        continue;
                    }
                    TSharedPtr<FJsonObject> Obj = MakeTrackJson(MovieScene, Track, true);
                    Obj->SetStringField(TEXT("binding"), TEXT(""));
                    Obj->SetStringField(TEXT("binding_guid"), TEXT(""));
                    Out.Add(MakeShared<FJsonValueObject>(Obj));
                }
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("tracks"), Out);
            return Result;
        });

    // sequencer.add_spawnable — adds a spawnable that actually spawns. Params: sequence_path, asset_path
    // (SkeletalMesh, actor Blueprint or actor class), name (optional binding name; default = asset name),
    // animation_path (optional AnimSequence placed on a new skeletal animation track), start_frame
    // (optional display frame for that animation, default = playback start).
    // Returns { ok, guid, name, kind, custom_binding, bound_class, animation_track_added }.
    // Does not touch the level and does not save — use asset.save.
    Registry.Register(TEXT("sequencer.add_spawnable"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            ULevelSequence* Sequence = LoadSequence(Params, OutError); if (!Sequence) return nullptr;
            UMovieScene* MovieScene = Sequence->GetMovieScene();

            FString AssetPath;
            if (!UnrealMCPBridge::Json::RequireString(Params, TEXT("asset_path"), AssetPath, OutError))
            {
                return nullptr;
            }
            UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
            if (!Asset)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("Could not load %s"), *AssetPath);
                return nullptr;
            }

            UAnimSequenceBase* Anim = nullptr;
            FString AnimPath;
            if (Params->TryGetStringField(TEXT("animation_path"), AnimPath) && !AnimPath.IsEmpty())
            {
                Anim = LoadObject<UAnimSequenceBase>(nullptr, *AnimPath);
                if (!Anim)
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = FString::Printf(TEXT("Could not load animation: %s"), *AnimPath);
                    return nullptr;
                }
            }

            const FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("MCP: Add Spawnable %s"), *Asset->GetName())));
            Sequence->Modify();
            MovieScene->Modify();

            // 스켈레탈 메시 애셋은 액터 스폰어블 바인딩이 받지 않는다 — 레벨 밖 임시 액터를 만들어 템플릿으로
            // 넘긴다. CreateSpawnable 이 MovieScene 안으로 복제하므로 이 임시 액터는 바로 버린다.
            ASkeletalMeshActor* TempActor = nullptr;
            UObject* SpawnSource = Asset;
            if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset))
            {
                // RF_Transient 는 주지 않는다 — 컴포넌트 서브오브젝트로 번져 복제된 템플릿 컴포넌트가 저장에서 빠질 수 있다.
                TempActor = NewObject<ASkeletalMeshActor>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), ASkeletalMeshActor::StaticClass(), Mesh->GetFName()));
                TempActor->GetSkeletalMeshComponent()->SetSkeletalMeshAsset(Mesh);
                SpawnSource = TempActor;
            }

            // ULevelSequence 에선 protected 오버라이드라 기반 클래스(public)로 부른다.
            const FGuid Guid = static_cast<UMovieSceneSequence*>(Sequence)->CreateSpawnable(SpawnSource);
            if (TempActor)
            {
                TempActor->MarkAsGarbage();
            }
            if (!Guid.IsValid() || !MovieScene->FindBinding(Guid))
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = FString::Printf(TEXT("CreateSpawnable failed for %s (%s)"), *AssetPath, *Asset->GetClass()->GetName());
                return nullptr;
            }

            FString Name;
            if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
            {
                Name = Asset->GetName();
            }
            if (FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Guid))
            {
                Possessable->SetName(Name);
            }
            else if (FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(Guid))
            {
                Spawnable->SetName(Name);
            }

            bool bAnimAdded = false;
            if (Anim)
            {
                if (UMovieSceneSkeletalAnimationTrack* AnimTrack = MovieScene->AddTrack<UMovieSceneSkeletalAnimationTrack>(Guid))
                {
                    double StartFrame = 0.0;
                    const FFrameNumber StartTick = Params->TryGetNumberField(TEXT("start_frame"), StartFrame)
                        ? DisplayToTick(MovieScene, StartFrame)
                        : UE::MovieScene::DiscreteInclusiveLower(MovieScene->GetPlaybackRange());
                    bAnimAdded = AnimTrack->AddNewAnimationOnRow(StartTick, Anim, 0) != nullptr;
                }
            }

            Sequence->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            Result->SetStringField(TEXT("guid"), Guid.ToString());
            Result->SetStringField(TEXT("name"), BindingName(MovieScene, Guid));
            DescribeBindingKind(Sequence, Guid, Result);
            Result->SetBoolField(TEXT("animation_track_added"), bAnimAdded);
            if (Result->GetStringField(TEXT("kind")) != TEXT("spawnable") || Result->GetStringField(TEXT("custom_binding")) == TEXT("MovieSceneSpawnableDirectorBlueprintBinding"))
            {
                Result->SetStringField(TEXT("warning"), TEXT("binding is not an actor spawnable — it may not spawn anything"));
            }
            return Result;
        });

    // sequencer.bake_anim_sequence — bakes one binding's skeletal mesh to an AnimSequence without opening
    // Sequencer (one call; no open/bake split). Params: sequence_path, binding (name or GUID), anim_path
    // (existing AnimSequence, or a new asset path to create), skeleton_path (optional; only needed when
    // creating and the binding has no skeletal mesh template, e.g. a possessable), export_options
    // (optional { UAnimSeqExportOption property: value }, e.g. { "bRecordInWorldSpace": true,
    // "bUseCustomTimeRange": true, "CustomStartFrame": "(Value=0)" }), create_link (optional, default false).
    // Returns { ok, anim_path, created, skeleton, play_length, num_frames }. Does not save — use asset.save.
    Registry.Register(TEXT("sequencer.bake_anim_sequence"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            ULevelSequence* Sequence = LoadSequence(Params, OutError); if (!Sequence) return nullptr;

            FString BindingRef;
            if (!UnrealMCPBridge::Json::RequireString(Params, TEXT("binding"), BindingRef, OutError))
            {
                return nullptr;
            }
            FGuid Guid;
            if (!ResolveBinding(Sequence, BindingRef, Guid, OutError))
            {
                return nullptr;
            }

            FString AnimPathIn;
            if (!UnrealMCPBridge::Json::RequireString(Params, TEXT("anim_path"), AnimPathIn, OutError))
            {
                return nullptr;
            }
            const FString PackageName = FPackageName::ObjectPathToPackageName(AnimPathIn);
            const FString AssetName = FPackageName::GetShortName(PackageName);
            const FString ObjectPath = PackageName + TEXT(".") + AssetName;

            UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
            if (!World)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("No editor world");
                return nullptr;
            }

            // 있는 애셋을 먼저 찾는다 — AssetTools::CreateAsset 은 같은 이름이 있으면 덮어쓰기 확인 창(모달)을 띄운다.
            UAnimSequence* AnimSequence = LoadObject<UAnimSequence>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
            bool bCreated = false;
            if (!AnimSequence)
            {
                if (FPackageName::DoesPackageExist(PackageName))
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = FString::Printf(TEXT("%s exists but is not an AnimSequence"), *PackageName);
                    return nullptr;
                }

                USkeleton* Skeleton = nullptr;
                FString SkeletonPath;
                if (Params->TryGetStringField(TEXT("skeleton_path"), SkeletonPath) && !SkeletonPath.IsEmpty())
                {
                    Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
                }
                else if (USkeletalMeshComponent* SkelComp = FindTemplateSkelMesh(Sequence, Guid))
                {
                    Skeleton = SkelComp->GetSkeletalMeshAsset() ? SkelComp->GetSkeletalMeshAsset()->GetSkeleton() : nullptr;
                }
                if (!Skeleton)
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = TEXT("Could not determine the skeleton for a new AnimSequence — pass skeleton_path");
                    return nullptr;
                }

                UAnimSequenceFactory* Factory = NewObject<UAnimSequenceFactory>();
                Factory->TargetSkeleton = Skeleton;
                IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
                AnimSequence = Cast<UAnimSequence>(AssetTools.CreateAsset(AssetName, FPackageName::GetLongPackagePath(PackageName), UAnimSequence::StaticClass(), Factory));
                if (!AnimSequence)
                {
                    OutError.Code = MCPProtocol::FMCPError::InternalError;
                    OutError.Message = FString::Printf(TEXT("Could not create AnimSequence %s"), *ObjectPath);
                    return nullptr;
                }
                bCreated = true;
            }

            UAnimSeqExportOption* ExportOptions = NewObject<UAnimSeqExportOption>(GetTransientPackage());
            const TSharedPtr<FJsonObject>* OptionsPtr = nullptr;
            Params->TryGetObjectField(TEXT("export_options"), OptionsPtr);
            if (!ApplyImportTextProperties(ExportOptions, OptionsPtr ? *OptionsPtr : nullptr, OutError))
            {
                return nullptr;
            }
            bool bCreateLink = false;
            Params->TryGetBoolField(TEXT("create_link"), bCreateLink);

            // USequencerToolsFunctionLibrary::ExportAnimSequence 를 그대로 쓰지 않는다 — 스폰 키가 일정한(늘 켜짐)
            // 스폰어블이면 플레이어를 한 번도 평가하지 않아 스폰어블이 생기지 않고 "No skel mesh found" 로 끝난다.
            // Python 에서 시퀀서를 먼저 열어야 했던 이유가 이것(열린 시퀀서가 대신 스폰해 둔다). 같은 순서로
            // 플레이어를 띄우되, 시작 프레임을 먼저 평가해 스폰시킨 뒤 굽는다.
            UMovieScene* MovieScene = Sequence->GetMovieScene();
            ALevelSequenceActor* PlayerActor = nullptr;
            FMovieSceneSequencePlaybackSettings Settings;
            ULevelSequencePlayer* Player = ULevelSequencePlayer::CreateLevelSequencePlayer(World, Sequence, Settings, PlayerActor);
            if (!Player)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = TEXT("Could not create a level sequence player");
                return nullptr;
            }
            const FFrameTime StartTime = FFrameRate::TransformTime(UE::MovieScene::DiscreteInclusiveLower(MovieScene->GetPlaybackRange()).Value,
                MovieScene->GetTickResolution(), MovieScene->GetDisplayRate());
            Player->SetPlaybackPosition(FMovieSceneSequencePlaybackParams(StartTime, EUpdatePositionMethod::Play));

            USkeletalMeshComponent* SkelComp = nullptr;
            for (TWeakObjectPtr<UObject> Bound : Player->FindBoundObjects(Guid, MovieSceneSequenceID::Root))
            {
                if (AActor* Actor = Cast<AActor>(Bound.Get()))
                {
                    SkelComp = Actor->FindComponentByClass<USkeletalMeshComponent>();
                }
                else
                {
                    SkelComp = Cast<USkeletalMeshComponent>(Bound.Get());
                }
                if (SkelComp && SkelComp->GetSkeletalMeshAsset() && SkelComp->GetSkeletalMeshAsset()->GetSkeleton())
                {
                    break;
                }
                SkelComp = nullptr;
            }

            bool bOk = false;
            if (SkelComp)
            {
                AnimSequence->SetSkeleton(SkelComp->GetSkeletalMeshAsset()->GetSkeleton());
                FAnimExportSequenceParameters ExportParams;
                ExportParams.Player = Player;
                ExportParams.MovieSceneSequence = Sequence;
                ExportParams.RootMovieSceneSequence = Sequence;
                bOk = MovieSceneToolHelpers::ExportToAnimSequence(AnimSequence, ExportOptions, ExportParams, SkelComp);
            }
            Player->Stop();
            if (PlayerActor)
            {
                World->DestroyActor(PlayerActor);
            }

            if (!SkelComp || !bOk)
            {
                OutError.Code = MCPProtocol::FMCPError::InternalError;
                OutError.Message = !SkelComp
                    ? FString::Printf(TEXT("Binding '%s' resolved no SkeletalMeshComponent with a mesh at the playback start (not spawned there, or a possessable missing from the open level)"),
                        *BindingName(MovieScene, Guid))
                    : FString::Printf(TEXT("ExportToAnimSequence failed for binding '%s' — see the editor log"), *BindingName(MovieScene, Guid));
                return nullptr;
            }
            if (bCreateLink)
            {
                USequencerToolsFunctionLibrary::LinkAnimSequence(Sequence, AnimSequence, ExportOptions, FMovieSceneBindingProxy(Guid, Sequence));
            }
            AnimSequence->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            Result->SetStringField(TEXT("anim_path"), AnimSequence->GetPathName());
            Result->SetBoolField(TEXT("created"), bCreated);
            Result->SetStringField(TEXT("binding"), BindingName(Sequence->GetMovieScene(), Guid));
            Result->SetStringField(TEXT("skeleton"), AnimSequence->GetSkeleton() ? AnimSequence->GetSkeleton()->GetPathName() : FString());
            Result->SetNumberField(TEXT("play_length"), AnimSequence->GetPlayLength());
            Result->SetNumberField(TEXT("num_frames"), AnimSequence->GetNumberOfSampledKeys());
            return Result;
        });

    // sequencer.get_marked_frames — Params: sequence_path. Returns { marks: [ { index, frame (display),
    // tick, seconds, label, comment, use_custom_color, color [r,g,b,a] } ], locked, display_rate }.
    Registry.Register(TEXT("sequencer.get_marked_frames"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            ULevelSequence* Sequence = LoadSequence(Params, OutError); if (!Sequence) return nullptr;
            return MakeMarksResult(Sequence->GetMovieScene());
        });

    // sequencer.set_marked_frames — adds or updates marks. Params: sequence_path, marks: [ { frame
    // (display frame), label (optional), comment (optional), color (optional [r,g,b] or [r,g,b,a]) } ],
    // replace (optional, default false: clear all marks first). A mark whose non-empty label already
    // exists is moved/updated instead of duplicated. Returns the full mark list like get_marked_frames.
    Registry.Register(TEXT("sequencer.set_marked_frames"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            ULevelSequence* Sequence = LoadSequence(Params, OutError); if (!Sequence) return nullptr;
            UMovieScene* MovieScene = Sequence->GetMovieScene();

            const TArray<TSharedPtr<FJsonValue>>* MarksIn = nullptr;
            if (!Params->TryGetArrayField(TEXT("marks"), MarksIn))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("marks is required (array of { frame, label, comment, color })");
                return nullptr;
            }
            if (MovieScene->AreMarkedFramesLocked())
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("marked frames are locked on this sequence");
                return nullptr;
            }

            // 먼저 전부 검사 — 중간에 실패해서 절반만 들어가는 일이 없게.
            TArray<FMovieSceneMarkedFrame> NewMarks;
            for (const TSharedPtr<FJsonValue>& Value : *MarksIn)
            {
                const TSharedPtr<FJsonObject>* MarkObj = nullptr;
                double Frame = 0.0;
                if (!Value->TryGetObject(MarkObj) || !(*MarkObj)->TryGetNumberField(TEXT("frame"), Frame))
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = TEXT("each mark needs a numeric frame (display frame)");
                    return nullptr;
                }
                FMovieSceneMarkedFrame Mark(DisplayToTick(MovieScene, Frame));
                (*MarkObj)->TryGetStringField(TEXT("label"), Mark.Label);
#if WITH_EDITORONLY_DATA
                (*MarkObj)->TryGetStringField(TEXT("comment"), Mark.Comment);
                const TArray<TSharedPtr<FJsonValue>>* Color = nullptr;
                if ((*MarkObj)->TryGetArrayField(TEXT("color"), Color) && (Color->Num() == 3 || Color->Num() == 4))
                {
                    Mark.CustomColor = FLinearColor((*Color)[0]->AsNumber(), (*Color)[1]->AsNumber(), (*Color)[2]->AsNumber(),
                        Color->Num() == 4 ? (*Color)[3]->AsNumber() : 1.0);
                    Mark.bUseCustomColor = true;
                }
#endif
                NewMarks.Add(Mark);
            }

            bool bReplace = false;
            Params->TryGetBoolField(TEXT("replace"), bReplace);

            const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Set Marked Frames")));
            MovieScene->Modify();
            if (bReplace)
            {
                MovieScene->DeleteMarkedFrames();
            }
            for (const FMovieSceneMarkedFrame& Mark : NewMarks)
            {
                const int32 Existing = Mark.Label.IsEmpty() ? INDEX_NONE : MovieScene->FindMarkedFrameByLabel(Mark.Label);
                if (Existing != INDEX_NONE)
                {
                    MovieScene->DeleteMarkedFrame(Existing);
                }
                MovieScene->AddMarkedFrame(Mark);
            }
            MovieScene->SortMarkedFrames();
            Sequence->MarkPackageDirty();
            return MakeMarksResult(MovieScene);
        });

    // sequencer.remove_marked_frames — Params: sequence_path, and one of: all (true), labels [..],
    // indices [..] (from get_marked_frames). Returns { removed } plus the remaining list.
    Registry.Register(TEXT("sequencer.remove_marked_frames"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            ULevelSequence* Sequence = LoadSequence(Params, OutError); if (!Sequence) return nullptr;
            UMovieScene* MovieScene = Sequence->GetMovieScene();

            bool bAll = false;
            Params->TryGetBoolField(TEXT("all"), bAll);
            const TArray<TSharedPtr<FJsonValue>>* Labels = nullptr;
            const TArray<TSharedPtr<FJsonValue>>* Indices = nullptr;
            Params->TryGetArrayField(TEXT("labels"), Labels);
            Params->TryGetArrayField(TEXT("indices"), Indices);
            if (!bAll && !Labels && !Indices)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("give all: true, labels or indices");
                return nullptr;
            }
            if (MovieScene->AreMarkedFramesLocked())
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("marked frames are locked on this sequence");
                return nullptr;
            }

            const TArray<FMovieSceneMarkedFrame>& Marks = MovieScene->GetMarkedFrames();
            TSet<int32> ToRemove;
            if (bAll)
            {
                for (int32 i = 0; i < Marks.Num(); ++i) { ToRemove.Add(i); }
            }
            if (Labels)
            {
                for (const TSharedPtr<FJsonValue>& V : *Labels)
                {
                    const FString Label = V->AsString();
                    for (int32 i = 0; i < Marks.Num(); ++i)
                    {
                        if (Marks[i].Label == Label) { ToRemove.Add(i); }
                    }
                }
            }
            if (Indices)
            {
                for (const TSharedPtr<FJsonValue>& V : *Indices)
                {
                    const int32 i = static_cast<int32>(V->AsNumber());
                    if (Marks.IsValidIndex(i)) { ToRemove.Add(i); }
                }
            }

            TArray<int32> Sorted = ToRemove.Array();
            Sorted.Sort(TGreater<int32>());   // 뒤에서부터 지워야 앞 인덱스가 안 밀린다
            if (Sorted.Num() > 0)
            {
                const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Remove Marked Frames")));
                MovieScene->Modify();
                for (const int32 i : Sorted)
                {
                    MovieScene->DeleteMarkedFrame(i);
                }
                Sequence->MarkPackageDirty();
            }

            TSharedPtr<FJsonObject> Result = MakeMarksResult(MovieScene);
            Result->SetNumberField(TEXT("removed"), Sorted.Num());
            return Result;
        });
}
