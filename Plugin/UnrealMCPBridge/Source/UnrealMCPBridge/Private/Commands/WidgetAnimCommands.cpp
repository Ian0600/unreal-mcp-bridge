#include "Commands/WidgetAnimCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/MovieSceneMarginTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Blueprint/WidgetTree.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "Channels/MovieSceneByteChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneIntegerChannel.h"
#include "Components/PanelSlot.h"
#include "Components/Widget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Tracks/MovieSceneByteTrack.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneEnumTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneIntegerTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Tracks/MovieSceneVectorTrack.h"
#include "WidgetBlueprint.h"

// Named rather than anonymous namespace: see the note in CommandJsonHelpers.h about unity
// builds merging command translation units.
namespace UnrealMCPBridge::WidgetAnimCmd
{
    using MCPProtocol::FMCPError;
    using UnrealMCPBridge::Json::RequireString;

    void Fail(FMCPError& OutError, int32 Code, const FString& Message)
    {
        OutError.Code = Code;
        OutError.Message = Message;
    }

    TSharedPtr<FJsonObject> Ok()
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField(TEXT("ok"), true);
        return Out;
    }

    // ---------------------------------------------------------------------------------
    // Resolution
    // ---------------------------------------------------------------------------------

    UWidgetBlueprint* LoadWidgetBlueprint(const TSharedPtr<FJsonObject>& Params, FMCPError& OutError)
    {
        FString Path;
        if (!RequireString(Params, TEXT("widget_path"), Path, OutError))
        {
            return nullptr;
        }
        UWidgetBlueprint* Blueprint = LoadObject<UWidgetBlueprint>(nullptr, *Path);
        if (!Blueprint)
        {
            Fail(OutError, FMCPError::InvalidParams,
                FString::Printf(TEXT("Could not load WidgetBlueprint: %s"), *Path));
            return nullptr;
        }
        return Blueprint;
    }

    UWidgetAnimation* FindAnimation(UWidgetBlueprint& Blueprint, const FString& AnimationName)
    {
        for (UWidgetAnimation* Animation : Blueprint.Animations)
        {
            if (Animation && (Animation->GetName().Equals(AnimationName, ESearchCase::IgnoreCase) ||
                              Animation->GetDisplayLabel().Equals(AnimationName, ESearchCase::IgnoreCase)))
            {
                return Animation;
            }
        }
        return nullptr;
    }

    UWidgetAnimation* ResolveAnimationParam(const TSharedPtr<FJsonObject>& Params, UWidgetBlueprint& Blueprint,
        FMCPError& OutError)
    {
        FString AnimationName;
        if (!RequireString(Params, TEXT("animation_name"), AnimationName, OutError))
        {
            return nullptr;
        }
        UWidgetAnimation* Animation = FindAnimation(Blueprint, AnimationName);
        if (!Animation)
        {
            Fail(OutError, FMCPError::InvalidParams,
                FString::Printf(TEXT("animation not found: %s"), *AnimationName));
            return nullptr;
        }
        if (!Animation->MovieScene)
        {
            Fail(OutError, FMCPError::InternalError,
                FString::Printf(TEXT("animation '%s' has no movie scene"), *AnimationName));
            return nullptr;
        }
        return Animation;
    }

    // The possessable GUID a track hangs off is meaningless to UMG on its own — the widget
    // binding array is what maps it back to a widget name at runtime. Look both up together.
    const FWidgetAnimationBinding* FindWidgetBinding(const UWidgetAnimation& Animation, const FString& WidgetName)
    {
        for (const FWidgetAnimationBinding& Binding : Animation.AnimationBindings)
        {
            if (Binding.WidgetName.ToString().Equals(WidgetName, ESearchCase::IgnoreCase))
            {
                return &Binding;
            }
        }
        return nullptr;
    }

    // UMovieScene::GetBindings has const and non-const overloads and the non-const one is
    // deprecated in 5.8, so every read goes through a const reference.
    const TArray<FMovieSceneBinding>& ReadBindings(const UMovieScene& MovieScene)
    {
        return MovieScene.GetBindings();
    }

    // Locates the track animating one property of one possessable. A binding can carry
    // several tracks, so the property name is what picks between them.
    UMovieSceneTrack* FindPropertyTrack(const UMovieScene& MovieScene, const FGuid& ObjectGuid, FName PropertyName)
    {
        for (const FMovieSceneBinding& SceneBinding : ReadBindings(MovieScene))
        {
            if (SceneBinding.GetObjectGuid() != ObjectGuid)
            {
                continue;
            }
            for (UMovieSceneTrack* Track : SceneBinding.GetTracks())
            {
                const UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track);
                if (PropertyTrack && PropertyTrack->GetPropertyName() == PropertyName)
                {
                    return Track;
                }
            }
            break;
        }
        return nullptr;
    }

    // ---------------------------------------------------------------------------------
    // Time conversion
    // ---------------------------------------------------------------------------------

    // Times cross the wire in seconds because that is what the animation panel shows;
    // everything inside the movie scene is FFrameNumber at tick resolution.
    FFrameNumber SecondsToFrame(const UMovieScene& MovieScene, double Seconds)
    {
        return (Seconds * MovieScene.GetTickResolution()).RoundToFrame();
    }

    double FrameToSeconds(const UMovieScene& MovieScene, FFrameNumber Frame)
    {
        return MovieScene.GetTickResolution().AsSeconds(FFrameTime(Frame));
    }

    // ---------------------------------------------------------------------------------
    // Track selection
    // ---------------------------------------------------------------------------------

    /**
     * Picks the movie scene track class that can animate a given widget property.
     *
     * Sequencer normally does this through registered track editors, which live in the
     * MovieSceneTools editor module behind an ISequencer instance. Reproducing the mapping
     * here keeps the command headless; it covers every property type UMG animation actually
     * exposes, and callers can override it with an explicit track_class when a project adds
     * its own.
     */
    UClass* TrackClassForProperty(const FProperty& Property)
    {
        if (Property.IsA<FBoolProperty>())          return UMovieSceneBoolTrack::StaticClass();
        if (Property.IsA<FIntProperty>())           return UMovieSceneIntegerTrack::StaticClass();
        if (Property.IsA<FFloatProperty>())         return UMovieSceneFloatTrack::StaticClass();
        if (Property.IsA<FDoubleProperty>())        return UMovieSceneFloatTrack::StaticClass();
        if (Property.IsA<FByteProperty>())          return UMovieSceneByteTrack::StaticClass();
        if (Property.IsA<FEnumProperty>())          return UMovieSceneEnumTrack::StaticClass();

        if (const FStructProperty* StructProperty = CastField<FStructProperty>(&Property))
        {
            const FName StructName = StructProperty->Struct->GetFName();
            if (StructName == TEXT("LinearColor") || StructName == TEXT("Color") ||
                StructName == TEXT("SlateColor"))
            {
                return UMovieSceneColorTrack::StaticClass();
            }
            if (StructName == TEXT("Margin"))
            {
                return UMovieSceneMarginTrack::StaticClass();
            }
            if (StructName == TEXT("WidgetTransform"))
            {
                return UMovieScene2DTransformTrack::StaticClass();
            }
            if (StructName == TEXT("Vector2D") || StructName == TEXT("Vector2f"))
            {
                return UMovieSceneFloatVectorTrack::StaticClass();
            }
        }
        return nullptr;
    }

    // ---------------------------------------------------------------------------------
    // Channels
    // ---------------------------------------------------------------------------------

    // Reports one section's keyframe channels. Channel names come from the editor-only
    // metadata Sequencer displays ("R", "G", "Translation.X", ...), which is also how
    // anim_key_add addresses them.
    TArray<TSharedPtr<FJsonValue>> DescribeChannels(UMovieSceneSection& Section, const UMovieScene& MovieScene)
    {
        TArray<TSharedPtr<FJsonValue>> Entries;
        const FMovieSceneChannelProxy& Proxy = Section.GetChannelProxy();

        for (const FMovieSceneChannelEntry& ChannelEntry : Proxy.GetAllEntries())
        {
            TArrayView<FMovieSceneChannel* const> Channels = ChannelEntry.GetChannels();
#if WITH_EDITOR
            TArrayView<const FMovieSceneChannelMetaData> MetaData = ChannelEntry.GetMetaData();
#endif
            for (int32 Index = 0; Index < Channels.Num(); ++Index)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("channel_type"), ChannelEntry.GetChannelTypeName().ToString());
                Entry->SetNumberField(TEXT("index"), Index);
                Entry->SetNumberField(TEXT("key_count"), Channels[Index] ? Channels[Index]->GetNumKeys() : 0);
#if WITH_EDITOR
                if (MetaData.IsValidIndex(Index))
                {
                    Entry->SetStringField(TEXT("name"), MetaData[Index].Name.ToString());
                    Entry->SetStringField(TEXT("display_name"), MetaData[Index].DisplayText.ToString());
                }
#endif
                Entries.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
        (void)MovieScene;
        return Entries;
    }

    /**
     * Finds a channel by the name reported above, falling back to positional lookup within a
     * channel type so single-channel sections can be addressed without knowing the name.
     *
     * Channel names are dotted and grouped: a colour track has "Color.R".."Color.A", a 2D
     * transform has "Translation.X", "Scale.X", "Shear.X" and "Angle". The leaf on its own
     * is accepted when it is unambiguous, so "R" resolves but "X" does not — on a transform
     * that would be three different channels. Display names are deliberately not matched:
     * they are localised ("Angle" reads as "각도" on a Korean editor).
     */
    FMovieSceneChannel* FindChannel(UMovieSceneSection& Section, const FString& ChannelName, int32 ChannelIndex)
    {
        const FMovieSceneChannelProxy& Proxy = Section.GetChannelProxy();

        if (!ChannelName.IsEmpty())
        {
#if WITH_EDITOR
            FMovieSceneChannel* LeafMatch = nullptr;
            int32 LeafMatchCount = 0;

            for (const FMovieSceneChannelEntry& ChannelEntry : Proxy.GetAllEntries())
            {
                TArrayView<FMovieSceneChannel* const> Channels = ChannelEntry.GetChannels();
                TArrayView<const FMovieSceneChannelMetaData> MetaData = ChannelEntry.GetMetaData();
                for (int32 Index = 0; Index < Channels.Num(); ++Index)
                {
                    if (!MetaData.IsValidIndex(Index))
                    {
                        continue;
                    }
                    const FString FullName = MetaData[Index].Name.ToString();
                    if (FullName.Equals(ChannelName, ESearchCase::IgnoreCase))
                    {
                        return Channels[Index];
                    }

                    FString Leaf;
                    if (FullName.Split(TEXT("."), nullptr, &Leaf, ESearchCase::CaseSensitive, ESearchDir::FromEnd) &&
                        Leaf.Equals(ChannelName, ESearchCase::IgnoreCase))
                    {
                        LeafMatch = Channels[Index];
                        ++LeafMatchCount;
                    }
                }
            }
            return LeafMatchCount == 1 ? LeafMatch : nullptr;
#else
            return nullptr;
#endif
        }

        int32 Remaining = ChannelIndex;
        for (const FMovieSceneChannelEntry& ChannelEntry : Proxy.GetAllEntries())
        {
            TArrayView<FMovieSceneChannel* const> Channels = ChannelEntry.GetChannels();
            if (Remaining < Channels.Num())
            {
                return Channels[Remaining];
            }
            Remaining -= Channels.Num();
        }
        return nullptr;
    }

    /**
     * Writes one key into whichever channel type this is.
     *
     * Each channel type owns its own key struct and interpolation model, so there is no
     * common typed entry point — the dispatch below is the same shape Sequencer's own key
     * helpers use. Float and double channels accept an interpolation mode; the discrete
     * types (bool, int, byte/enum) are constant by nature and ignore it.
     */
    bool AddKeyToChannel(FMovieSceneChannel* Channel, FName ChannelTypeName, FFrameNumber Frame,
        double Value, const FString& Interpolation, FMCPError& OutError)
    {
        const bool bLinear = Interpolation.Equals(TEXT("linear"), ESearchCase::IgnoreCase);
        const bool bConstant = Interpolation.Equals(TEXT("constant"), ESearchCase::IgnoreCase);

        if (ChannelTypeName == FMovieSceneFloatChannel::StaticStruct()->GetFName())
        {
            FMovieSceneFloatChannel* Typed = static_cast<FMovieSceneFloatChannel*>(Channel);
            const float FloatValue = static_cast<float>(Value);
            if (bConstant)      Typed->AddConstantKey(Frame, FloatValue);
            else if (bLinear)   Typed->AddLinearKey(Frame, FloatValue);
            else                Typed->AddCubicKey(Frame, FloatValue);
            return true;
        }
        if (ChannelTypeName == FMovieSceneDoubleChannel::StaticStruct()->GetFName())
        {
            FMovieSceneDoubleChannel* Typed = static_cast<FMovieSceneDoubleChannel*>(Channel);
            if (bConstant)      Typed->AddConstantKey(Frame, Value);
            else if (bLinear)   Typed->AddLinearKey(Frame, Value);
            else                Typed->AddCubicKey(Frame, Value);
            return true;
        }
        if (ChannelTypeName == FMovieSceneBoolChannel::StaticStruct()->GetFName())
        {
            FMovieSceneBoolChannel* Typed = static_cast<FMovieSceneBoolChannel*>(Channel);
            Typed->GetData().UpdateOrAddKey(Frame, Value != 0.0);
            return true;
        }
        if (ChannelTypeName == FMovieSceneIntegerChannel::StaticStruct()->GetFName())
        {
            FMovieSceneIntegerChannel* Typed = static_cast<FMovieSceneIntegerChannel*>(Channel);
            Typed->GetData().UpdateOrAddKey(Frame, static_cast<int32>(Value));
            return true;
        }
        if (ChannelTypeName == FMovieSceneByteChannel::StaticStruct()->GetFName())
        {
            FMovieSceneByteChannel* Typed = static_cast<FMovieSceneByteChannel*>(Channel);
            Typed->GetData().UpdateOrAddKey(Frame, static_cast<uint8>(Value));
            return true;
        }

        Fail(OutError, FMCPError::InvalidParams,
            FString::Printf(TEXT("unsupported channel type for numeric keying: %s"), *ChannelTypeName.ToString()));
        return false;
    }

    void MarkStructurallyModified(UWidgetBlueprint& Blueprint)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
    }
}

void FWidgetAnimCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    using namespace UnrealMCPBridge::WidgetAnimCmd;
    using MCPProtocol::FMCPError;

    // =====================================================================================
    // ANIMATION ASSETS
    // =====================================================================================

    // widget.anim_list — animations on a widget blueprint. Params: widget_path.
    Registry.Register(TEXT("widget.anim_list"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            TArray<TSharedPtr<FJsonValue>> Entries;
            for (const UWidgetAnimation* Animation : Blueprint->Animations)
            {
                if (!Animation)
                {
                    continue;
                }
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"), Animation->GetName());
                Entry->SetStringField(TEXT("display_label"), Animation->GetDisplayLabel());
                Entry->SetNumberField(TEXT("start_time"), Animation->GetStartTime());
                Entry->SetNumberField(TEXT("end_time"), Animation->GetEndTime());
                Entry->SetNumberField(TEXT("binding_count"), Animation->GetBindings().Num());
                if (const UMovieScene* MovieScene = Animation->GetMovieScene())
                {
                    Entry->SetNumberField(TEXT("display_rate"), MovieScene->GetDisplayRate().AsDecimal());
                    Entry->SetNumberField(TEXT("track_count"), MovieScene->GetBindings().Num());
                }
                Entries.Add(MakeShared<FJsonValueObject>(Entry));
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("count"), Entries.Num());
            Result->SetArrayField(TEXT("animations"), Entries);
            return Result;
        });

    // widget.anim_create — new animation. Params: widget_path, animation_name,
    // start_time (default 0), end_time (default 5), frame_rate (default 60).
    Registry.Register(TEXT("widget.anim_create"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            FString AnimationName;
            if (!RequireString(Params, TEXT("animation_name"), AnimationName, OutError)) return nullptr;

            if (FindAnimation(*Blueprint, AnimationName))
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("animation '%s' already exists"), *AnimationName));
                return nullptr;
            }
            // Animations become blueprint variables, so a name already taken by one would
            // produce a duplicate member at compile time rather than failing here.
            if (Blueprint->WidgetTree && Blueprint->WidgetTree->FindWidget(FName(*AnimationName)))
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("'%s' is already the name of a widget in this blueprint"), *AnimationName));
                return nullptr;
            }

            double StartTime = 0.0;
            double EndTime = 5.0;
            double FrameRate = 60.0;
            Params->TryGetNumberField(TEXT("start_time"), StartTime);
            Params->TryGetNumberField(TEXT("end_time"), EndTime);
            Params->TryGetNumberField(TEXT("frame_rate"), FrameRate);

            if (EndTime <= StartTime)
            {
                Fail(OutError, FMCPError::InvalidParams, TEXT("end_time must be greater than start_time"));
                return nullptr;
            }

            Blueprint->Modify();

            const FName AnimationFName(*AnimationName);
            UWidgetAnimation* NewAnimation = NewObject<UWidgetAnimation>(Blueprint, AnimationFName, RF_Transactional);
            NewAnimation->SetDisplayLabel(AnimationName);
            NewAnimation->MovieScene = NewObject<UMovieScene>(NewAnimation, AnimationFName, RF_Transactional);
            NewAnimation->MovieScene->SetDisplayRate(FFrameRate(static_cast<int32>(FrameRate), 1));

            const FFrameTime StartFrame = StartTime * NewAnimation->MovieScene->GetTickResolution();
            const FFrameTime EndFrame = EndTime * NewAnimation->MovieScene->GetTickResolution();
            NewAnimation->MovieScene->SetPlaybackRange(
                TRange<FFrameNumber>(StartFrame.FrameNumber, EndFrame.FrameNumber + 1));
            NewAnimation->MovieScene->GetEditorData().WorkStart = StartTime;
            NewAnimation->MovieScene->GetEditorData().WorkEnd = EndTime;

            Blueprint->Animations.Add(NewAnimation);
            // Registers the GUID that keeps graph references alive across a later rename.
            Blueprint->OnVariableAdded(AnimationFName);
            MarkStructurallyModified(*Blueprint);

            TSharedPtr<FJsonObject> Result = Ok();
            Result->SetStringField(TEXT("name"), NewAnimation->GetName());
            return Result;
        });

    // widget.anim_remove — delete an animation. Params: widget_path, animation_name.
    Registry.Register(TEXT("widget.anim_remove"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidgetAnimation* Animation = ResolveAnimationParam(Params, *Blueprint, OutError);
            if (!Animation) return nullptr;

            const FName RemovedName = Animation->GetFName();

            Blueprint->Modify();
            Blueprint->Animations.Remove(Animation);
            Blueprint->OnVariableRemoved(RemovedName);
            MarkStructurallyModified(*Blueprint);
            return Ok();
        });

    // widget.anim_rename — rename an animation and its blueprint variable.
    // Params: widget_path, animation_name, new_name.
    Registry.Register(TEXT("widget.anim_rename"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidgetAnimation* Animation = ResolveAnimationParam(Params, *Blueprint, OutError);
            if (!Animation) return nullptr;

            FString NewName;
            if (!RequireString(Params, TEXT("new_name"), NewName, OutError)) return nullptr;

            if (FindAnimation(*Blueprint, NewName))
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("animation '%s' already exists"), *NewName));
                return nullptr;
            }
            if (Blueprint->WidgetTree && Blueprint->WidgetTree->FindWidget(FName(*NewName)))
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("'%s' is already the name of a widget in this blueprint"), *NewName));
                return nullptr;
            }

            const FName OldFName = Animation->GetFName();
            const FName NewFName(*NewName);

            Blueprint->Modify();
            Animation->Modify();
            Animation->SetDisplayLabel(NewName);
            Animation->Rename(*NewName, Animation->GetOuter(), REN_DontCreateRedirectors);
            // Retargets graph nodes that referenced the old animation variable.
            Blueprint->OnVariableRenamed(OldFName, NewFName);
            MarkStructurallyModified(*Blueprint);

            TSharedPtr<FJsonObject> Result = Ok();
            Result->SetStringField(TEXT("name"), Animation->GetName());
            return Result;
        });

    // widget.anim_set_range — playback range and display rate. Params: widget_path,
    // animation_name, start_time, end_time, frame_rate (all optional).
    Registry.Register(TEXT("widget.anim_set_range"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidgetAnimation* Animation = ResolveAnimationParam(Params, *Blueprint, OutError);
            if (!Animation) return nullptr;

            UMovieScene* MovieScene = Animation->MovieScene;
            MovieScene->Modify();

            double FrameRate = 0.0;
            if (Params->TryGetNumberField(TEXT("frame_rate"), FrameRate) && FrameRate > 0.0)
            {
                MovieScene->SetDisplayRate(FFrameRate(static_cast<int32>(FrameRate), 1));
            }

            double StartTime = 0.0, EndTime = 0.0;
            const bool bHasStart = Params->TryGetNumberField(TEXT("start_time"), StartTime);
            const bool bHasEnd = Params->TryGetNumberField(TEXT("end_time"), EndTime);
            if (bHasStart || bHasEnd)
            {
                const TRange<FFrameNumber> Current = MovieScene->GetPlaybackRange();
                const FFrameNumber NewStart = bHasStart
                    ? SecondsToFrame(*MovieScene, StartTime)
                    : Current.GetLowerBoundValue();
                const FFrameNumber NewEnd = bHasEnd
                    ? SecondsToFrame(*MovieScene, EndTime) + 1
                    : Current.GetUpperBoundValue();

                if (NewEnd <= NewStart)
                {
                    Fail(OutError, FMCPError::InvalidParams, TEXT("end_time must be greater than start_time"));
                    return nullptr;
                }
                MovieScene->SetPlaybackRange(TRange<FFrameNumber>(NewStart, NewEnd));
                MovieScene->GetEditorData().WorkStart = FrameToSeconds(*MovieScene, NewStart);
                MovieScene->GetEditorData().WorkEnd = FrameToSeconds(*MovieScene, NewEnd);
            }

            MarkStructurallyModified(*Blueprint);

            TSharedPtr<FJsonObject> Result = Ok();
            Result->SetNumberField(TEXT("start_time"), Animation->GetStartTime());
            Result->SetNumberField(TEXT("end_time"), Animation->GetEndTime());
            Result->SetNumberField(TEXT("display_rate"), MovieScene->GetDisplayRate().AsDecimal());
            return Result;
        });

    // =====================================================================================
    // WIDGET BINDINGS
    // =====================================================================================

    // widget.anim_binding_list — widgets possessed by an animation. Params: widget_path,
    // animation_name.
    Registry.Register(TEXT("widget.anim_binding_list"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidgetAnimation* Animation = ResolveAnimationParam(Params, *Blueprint, OutError);
            if (!Animation) return nullptr;

            TArray<TSharedPtr<FJsonValue>> Entries;
            for (const FWidgetAnimationBinding& Binding : Animation->GetBindings())
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("widget_name"), Binding.WidgetName.ToString());
                Entry->SetStringField(TEXT("slot_widget_name"), Binding.SlotWidgetName.ToString());
                Entry->SetStringField(TEXT("guid"), Binding.AnimationGuid.ToString());
                Entry->SetBoolField(TEXT("is_root_widget"), Binding.bIsRootWidget);

                int32 TrackCount = 0;
                for (const FMovieSceneBinding& SceneBinding : ReadBindings(*Animation->MovieScene))
                {
                    if (SceneBinding.GetObjectGuid() == Binding.AnimationGuid)
                    {
                        TrackCount = SceneBinding.GetTracks().Num();
                        break;
                    }
                }
                Entry->SetNumberField(TEXT("track_count"), TrackCount);
                Entries.Add(MakeShared<FJsonValueObject>(Entry));
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("count"), Entries.Num());
            Result->SetArrayField(TEXT("bindings"), Entries);
            return Result;
        });

    // widget.anim_bind_widget — make a widget (or its slot) animatable by this animation.
    // Params: widget_path, animation_name, widget_name, bind_slot (default false — bind the
    // widget's slot instead of the widget itself, which is how layout is animated).
    Registry.Register(TEXT("widget.anim_bind_widget"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;
            if (!Blueprint->WidgetTree)
            {
                Fail(OutError, FMCPError::InternalError, TEXT("blueprint has no widget tree"));
                return nullptr;
            }

            UWidgetAnimation* Animation = ResolveAnimationParam(Params, *Blueprint, OutError);
            if (!Animation) return nullptr;

            FString WidgetName;
            if (!RequireString(Params, TEXT("widget_name"), WidgetName, OutError)) return nullptr;

            UWidget* Widget = Blueprint->WidgetTree->FindWidget(FName(*WidgetName));
            if (!Widget)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("widget not found in tree: %s"), *WidgetName));
                return nullptr;
            }

            bool bBindSlot = false;
            Params->TryGetBoolField(TEXT("bind_slot"), bBindSlot);

            UObject* ObjectToPossess = Widget;
            if (bBindSlot)
            {
                if (!Widget->Slot)
                {
                    Fail(OutError, FMCPError::InvalidParams,
                        FString::Printf(TEXT("widget '%s' has no slot to bind"), *WidgetName));
                    return nullptr;
                }
                ObjectToPossess = Widget->Slot;
            }

            if (const FWidgetAnimationBinding* Existing = FindWidgetBinding(*Animation, WidgetName))
            {
                const bool bExistingIsSlot = !Existing->SlotWidgetName.IsNone();
                if (bExistingIsSlot == bBindSlot)
                {
                    TSharedPtr<FJsonObject> Result = Ok();
                    Result->SetStringField(TEXT("guid"), Existing->AnimationGuid.ToString());
                    Result->SetBoolField(TEXT("already_bound"), true);
                    return Result;
                }
            }

            Animation->Modify();
            UMovieScene* MovieScene = Animation->MovieScene;
            MovieScene->Modify();

            const FGuid PossessableGuid = MovieScene->AddPossessable(
                ObjectToPossess->GetName(), ObjectToPossess->GetClass());

            FWidgetAnimationBinding NewBinding;
            NewBinding.AnimationGuid = PossessableGuid;
            NewBinding.bIsRootWidget = (Blueprint->WidgetTree->RootWidget == Widget);
            if (bBindSlot)
            {
                // Slot bindings record the slot object's name plus the widget it holds — the
                // runtime resolves the slot by walking to the content widget first.
                NewBinding.SlotWidgetName = Widget->Slot->GetFName();
                NewBinding.WidgetName = Widget->GetFName();
            }
            else
            {
                NewBinding.WidgetName = Widget->GetFName();
            }
            Animation->AnimationBindings.Add(NewBinding);

            MarkStructurallyModified(*Blueprint);

            TSharedPtr<FJsonObject> Result = Ok();
            Result->SetStringField(TEXT("guid"), PossessableGuid.ToString());
            return Result;
        });

    // widget.anim_unbind_widget — remove a widget binding and its tracks.
    // Params: widget_path, animation_name, widget_name.
    Registry.Register(TEXT("widget.anim_unbind_widget"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidgetAnimation* Animation = ResolveAnimationParam(Params, *Blueprint, OutError);
            if (!Animation) return nullptr;

            FString WidgetName;
            if (!RequireString(Params, TEXT("widget_name"), WidgetName, OutError)) return nullptr;

            const FWidgetAnimationBinding* Binding = FindWidgetBinding(*Animation, WidgetName);
            if (!Binding)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("animation has no binding for widget '%s'"), *WidgetName));
                return nullptr;
            }
            const FGuid Guid = Binding->AnimationGuid;

            Animation->Modify();
            Animation->MovieScene->Modify();
            // Drops the possessable and every track hanging off it; the parallel widget
            // binding has to go too or the animation keeps a dangling GUID.
            Animation->MovieScene->RemovePossessable(Guid);
            Animation->AnimationBindings.RemoveAll([Guid](const FWidgetAnimationBinding& Candidate)
            {
                return Candidate.AnimationGuid == Guid;
            });

            MarkStructurallyModified(*Blueprint);
            return Ok();
        });

    // =====================================================================================
    // TRACKS AND SECTIONS
    // =====================================================================================

    // widget.anim_track_list — tracks and sections per bound widget. Params: widget_path,
    // animation_name, widget_name (optional filter).
    Registry.Register(TEXT("widget.anim_track_list"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidgetAnimation* Animation = ResolveAnimationParam(Params, *Blueprint, OutError);
            if (!Animation) return nullptr;

            FString WidgetFilter;
            Params->TryGetStringField(TEXT("widget_name"), WidgetFilter);

            UMovieScene* MovieScene = Animation->MovieScene;
            TArray<TSharedPtr<FJsonValue>> Entries;

            for (const FWidgetAnimationBinding& WidgetBinding : Animation->GetBindings())
            {
                if (!WidgetFilter.IsEmpty() &&
                    !WidgetBinding.WidgetName.ToString().Equals(WidgetFilter, ESearchCase::IgnoreCase))
                {
                    continue;
                }

                for (const FMovieSceneBinding& SceneBinding : ReadBindings(*MovieScene))
                {
                    if (SceneBinding.GetObjectGuid() != WidgetBinding.AnimationGuid)
                    {
                        continue;
                    }
                    for (const UMovieSceneTrack* Track : SceneBinding.GetTracks())
                    {
                        if (!Track)
                        {
                            continue;
                        }
                        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                        Entry->SetStringField(TEXT("widget_name"), WidgetBinding.WidgetName.ToString());
                        Entry->SetStringField(TEXT("guid"), WidgetBinding.AnimationGuid.ToString());
                        Entry->SetStringField(TEXT("track_class"), Track->GetClass()->GetPathName());
                        Entry->SetStringField(TEXT("track_name"), Track->GetTrackName().ToString());
                        if (const UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track))
                        {
                            Entry->SetStringField(TEXT("property"), PropertyTrack->GetPropertyName().ToString());
                            Entry->SetStringField(TEXT("property_path"), PropertyTrack->GetPropertyPath().ToString());
                        }

                        TArray<TSharedPtr<FJsonValue>> Sections;
                        const TArray<UMovieSceneSection*>& TrackSections = Track->GetAllSections();
                        for (int32 Index = 0; Index < TrackSections.Num(); ++Index)
                        {
                            UMovieSceneSection* Section = TrackSections[Index];
                            if (!Section)
                            {
                                continue;
                            }
                            TSharedPtr<FJsonObject> SectionJson = MakeShared<FJsonObject>();
                            SectionJson->SetNumberField(TEXT("index"), Index);
                            SectionJson->SetNumberField(TEXT("row_index"), Section->GetRowIndex());
                            const TRange<FFrameNumber> Range = Section->GetRange();
                            if (!Range.GetLowerBound().IsOpen())
                            {
                                SectionJson->SetNumberField(TEXT("start_time"),
                                    FrameToSeconds(*MovieScene, Range.GetLowerBoundValue()));
                            }
                            if (!Range.GetUpperBound().IsOpen())
                            {
                                SectionJson->SetNumberField(TEXT("end_time"),
                                    FrameToSeconds(*MovieScene, Range.GetUpperBoundValue()));
                            }
                            SectionJson->SetArrayField(TEXT("channels"), DescribeChannels(*Section, *MovieScene));
                            Sections.Add(MakeShared<FJsonValueObject>(SectionJson));
                        }
                        Entry->SetArrayField(TEXT("sections"), Sections);
                        Entries.Add(MakeShared<FJsonValueObject>(Entry));
                    }
                }
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("count"), Entries.Num());
            Result->SetArrayField(TEXT("tracks"), Entries);
            return Result;
        });

    // widget.anim_track_add — add a property track for a bound widget, with an initial
    // section spanning the playback range. Params: widget_path, animation_name, widget_name,
    // property, track_class (optional override), create_section (default true).
    Registry.Register(TEXT("widget.anim_track_add"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidgetAnimation* Animation = ResolveAnimationParam(Params, *Blueprint, OutError);
            if (!Animation) return nullptr;

            FString WidgetName, PropertyName;
            if (!RequireString(Params, TEXT("widget_name"), WidgetName, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("property"), PropertyName, OutError)) return nullptr;

            const FWidgetAnimationBinding* Binding = FindWidgetBinding(*Animation, WidgetName);
            if (!Binding)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("widget '%s' is not bound to this animation — call widget.anim_bind_widget first"),
                        *WidgetName));
                return nullptr;
            }

            // The property lives on whichever object the binding possesses: the widget for a
            // plain binding, the slot object for a slot binding.
            UWidget* Widget = Blueprint->WidgetTree ? Blueprint->WidgetTree->FindWidget(FName(*WidgetName)) : nullptr;
            if (!Widget)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("widget not found in tree: %s"), *WidgetName));
                return nullptr;
            }
            UObject* PossessedObject = Binding->SlotWidgetName.IsNone()
                ? static_cast<UObject*>(Widget)
                : static_cast<UObject*>(Widget->Slot);
            if (!PossessedObject)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("slot binding for '%s' has no live slot object"), *WidgetName));
                return nullptr;
            }

            FProperty* Property = PossessedObject->GetClass()->FindPropertyByName(FName(*PropertyName));
            if (!Property)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("no property '%s' on %s"),
                        *PropertyName, *PossessedObject->GetClass()->GetName()));
                return nullptr;
            }

            UClass* TrackClass = nullptr;
            FString TrackClassPath;
            if (Params->TryGetStringField(TEXT("track_class"), TrackClassPath) && !TrackClassPath.IsEmpty())
            {
                TrackClass = LoadObject<UClass>(nullptr, *TrackClassPath);
                if (!TrackClass || !TrackClass->IsChildOf(UMovieSceneTrack::StaticClass()))
                {
                    Fail(OutError, FMCPError::InvalidParams,
                        FString::Printf(TEXT("track_class is not a MovieSceneTrack: %s"), *TrackClassPath));
                    return nullptr;
                }
            }
            else
            {
                TrackClass = TrackClassForProperty(*Property);
                if (!TrackClass)
                {
                    Fail(OutError, FMCPError::InvalidParams,
                        FString::Printf(TEXT("no track type known for property '%s' of type %s — pass track_class explicitly"),
                            *PropertyName, *Property->GetCPPType()));
                    return nullptr;
                }
            }

            UMovieScene* MovieScene = Animation->MovieScene;
            Animation->Modify();
            MovieScene->Modify();

            if (UMovieSceneTrack* Existing = MovieScene->FindTrack(TrackClass, Binding->AnimationGuid, FName(*PropertyName)))
            {
                TSharedPtr<FJsonObject> Result = Ok();
                Result->SetStringField(TEXT("track_class"), Existing->GetClass()->GetPathName());
                Result->SetBoolField(TEXT("already_existed"), true);
                return Result;
            }

            UMovieSceneTrack* Track = MovieScene->AddTrack(TrackClass, Binding->AnimationGuid);
            if (!Track)
            {
                Fail(OutError, FMCPError::InternalError, TEXT("AddTrack returned null"));
                return nullptr;
            }

            if (UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track))
            {
                // The property path is what the runtime resolves at evaluation time; without
                // it the track exists but animates nothing.
                PropertyTrack->SetPropertyNameAndPath(FName(*PropertyName), PropertyName);
            }

            bool bCreateSection = true;
            Params->TryGetBoolField(TEXT("create_section"), bCreateSection);

            TSharedPtr<FJsonObject> Result = Ok();
            Result->SetStringField(TEXT("track_class"), Track->GetClass()->GetPathName());
            Result->SetStringField(TEXT("property"), PropertyName);

            if (bCreateSection)
            {
                UMovieSceneSection* Section = Track->CreateNewSection();
                if (Section)
                {
                    Section->SetFlags(RF_Transactional);
                    Section->SetRange(MovieScene->GetPlaybackRange());
                    Track->AddSection(*Section);
                    Result->SetNumberField(TEXT("section_index"), Track->GetAllSections().Num() - 1);
                    Result->SetArrayField(TEXT("channels"), DescribeChannels(*Section, *MovieScene));
                }
            }

            MarkStructurallyModified(*Blueprint);
            return Result;
        });

    // widget.anim_track_remove — delete a property track. Params: widget_path,
    // animation_name, widget_name, property.
    Registry.Register(TEXT("widget.anim_track_remove"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidgetAnimation* Animation = ResolveAnimationParam(Params, *Blueprint, OutError);
            if (!Animation) return nullptr;

            FString WidgetName, PropertyName;
            if (!RequireString(Params, TEXT("widget_name"), WidgetName, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("property"), PropertyName, OutError)) return nullptr;

            const FWidgetAnimationBinding* Binding = FindWidgetBinding(*Animation, WidgetName);
            if (!Binding)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("widget '%s' is not bound to this animation"), *WidgetName));
                return nullptr;
            }

            UMovieScene* MovieScene = Animation->MovieScene;
            UMovieSceneTrack* Target = FindPropertyTrack(*MovieScene, Binding->AnimationGuid, FName(*PropertyName));

            if (!Target)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("no track for property '%s' on widget '%s'"), *PropertyName, *WidgetName));
                return nullptr;
            }

            MovieScene->Modify();
            const bool bRemoved = MovieScene->RemoveTrack(*Target);
            MarkStructurallyModified(*Blueprint);

            TSharedPtr<FJsonObject> Result = Ok();
            Result->SetBoolField(TEXT("removed"), bRemoved);
            return Result;
        });

    // widget.anim_section_add — extra section on an existing track. Params: widget_path,
    // animation_name, widget_name, property, start_time, end_time, row_index (default 0).
    Registry.Register(TEXT("widget.anim_section_add"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidgetAnimation* Animation = ResolveAnimationParam(Params, *Blueprint, OutError);
            if (!Animation) return nullptr;

            FString WidgetName, PropertyName;
            if (!RequireString(Params, TEXT("widget_name"), WidgetName, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("property"), PropertyName, OutError)) return nullptr;

            const FWidgetAnimationBinding* Binding = FindWidgetBinding(*Animation, WidgetName);
            if (!Binding)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("widget '%s' is not bound to this animation"), *WidgetName));
                return nullptr;
            }

            UMovieScene* MovieScene = Animation->MovieScene;
            UMovieSceneTrack* Target = FindPropertyTrack(*MovieScene, Binding->AnimationGuid, FName(*PropertyName));

            if (!Target)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("no track for property '%s' on widget '%s'"), *PropertyName, *WidgetName));
                return nullptr;
            }

            double StartTime = 0.0, EndTime = 0.0;
            if (!Params->TryGetNumberField(TEXT("start_time"), StartTime) ||
                !Params->TryGetNumberField(TEXT("end_time"), EndTime))
            {
                Fail(OutError, FMCPError::InvalidParams, TEXT("start_time and end_time are required"));
                return nullptr;
            }
            if (EndTime <= StartTime)
            {
                Fail(OutError, FMCPError::InvalidParams, TEXT("end_time must be greater than start_time"));
                return nullptr;
            }

            int32 RowIndex = 0;
            if (double RowValue; Params->TryGetNumberField(TEXT("row_index"), RowValue))
            {
                RowIndex = static_cast<int32>(RowValue);
            }

            Target->Modify();
            UMovieSceneSection* Section = Target->CreateNewSection();
            if (!Section)
            {
                Fail(OutError, FMCPError::InternalError, TEXT("CreateNewSection returned null"));
                return nullptr;
            }
            Section->SetFlags(RF_Transactional);
            Section->SetRange(TRange<FFrameNumber>(
                SecondsToFrame(*MovieScene, StartTime), SecondsToFrame(*MovieScene, EndTime) + 1));
            Section->SetRowIndex(RowIndex);
            Target->AddSection(*Section);

            MarkStructurallyModified(*Blueprint);

            TSharedPtr<FJsonObject> Result = Ok();
            Result->SetNumberField(TEXT("section_index"), Target->GetAllSections().Num() - 1);
            Result->SetArrayField(TEXT("channels"), DescribeChannels(*Section, *MovieScene));
            return Result;
        });

    // widget.anim_key_add — write a keyframe. Params: widget_path, animation_name,
    // widget_name, property, time, value, channel (name from anim_track_list; defaults to
    // channel_index), channel_index (default 0), section_index (default 0),
    // interpolation ("cubic" | "linear" | "constant", default cubic).
    Registry.Register(TEXT("widget.anim_key_add"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidgetAnimation* Animation = ResolveAnimationParam(Params, *Blueprint, OutError);
            if (!Animation) return nullptr;

            FString WidgetName, PropertyName;
            if (!RequireString(Params, TEXT("widget_name"), WidgetName, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("property"), PropertyName, OutError)) return nullptr;

            const FWidgetAnimationBinding* Binding = FindWidgetBinding(*Animation, WidgetName);
            if (!Binding)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("widget '%s' is not bound to this animation"), *WidgetName));
                return nullptr;
            }

            UMovieScene* MovieScene = Animation->MovieScene;
            UMovieSceneTrack* Target = FindPropertyTrack(*MovieScene, Binding->AnimationGuid, FName(*PropertyName));

            if (!Target)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("no track for property '%s' on widget '%s' — call widget.anim_track_add first"),
                        *PropertyName, *WidgetName));
                return nullptr;
            }

            int32 SectionIndex = 0;
            if (double SectionValue; Params->TryGetNumberField(TEXT("section_index"), SectionValue))
            {
                SectionIndex = static_cast<int32>(SectionValue);
            }
            const TArray<UMovieSceneSection*>& Sections = Target->GetAllSections();
            if (!Sections.IsValidIndex(SectionIndex) || !Sections[SectionIndex])
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("section_index %d out of range (track has %d sections)"),
                        SectionIndex, Sections.Num()));
                return nullptr;
            }
            UMovieSceneSection* Section = Sections[SectionIndex];

            FString ChannelName;
            Params->TryGetStringField(TEXT("channel"), ChannelName);
            int32 ChannelIndex = 0;
            if (double ChannelValue; Params->TryGetNumberField(TEXT("channel_index"), ChannelValue))
            {
                ChannelIndex = static_cast<int32>(ChannelValue);
            }

            FMovieSceneChannel* Channel = FindChannel(*Section, ChannelName, ChannelIndex);
            if (!Channel)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    ChannelName.IsEmpty()
                        ? FString::Printf(TEXT("channel_index %d out of range"), ChannelIndex)
                        : FString::Printf(
                            TEXT("no channel matches '%s' on this section — the name is either unknown or ambiguous ")
                            TEXT("across channel groups. Call widget.anim_track_list and use the reported 'name'."),
                            *ChannelName));
                return nullptr;
            }

            // Recover the channel's type name so the key write can be dispatched — the proxy
            // groups channels by type, so this is a lookup rather than a virtual call.
            FName ChannelTypeName;
            {
                const FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
                for (const FMovieSceneChannelEntry& ChannelEntry : Proxy.GetAllEntries())
                {
                    if (ChannelEntry.GetChannels().Contains(Channel))
                    {
                        ChannelTypeName = ChannelEntry.GetChannelTypeName();
                        break;
                    }
                }
            }

            double Time = 0.0, Value = 0.0;
            if (!Params->TryGetNumberField(TEXT("time"), Time))
            {
                Fail(OutError, FMCPError::InvalidParams, TEXT("time is required"));
                return nullptr;
            }
            if (!Params->TryGetNumberField(TEXT("value"), Value))
            {
                bool bBoolValue = false;
                if (!Params->TryGetBoolField(TEXT("value"), bBoolValue))
                {
                    Fail(OutError, FMCPError::InvalidParams, TEXT("value is required (number or bool)"));
                    return nullptr;
                }
                Value = bBoolValue ? 1.0 : 0.0;
            }

            FString Interpolation = TEXT("cubic");
            Params->TryGetStringField(TEXT("interpolation"), Interpolation);

            Section->Modify();
            const FFrameNumber Frame = SecondsToFrame(*MovieScene, Time);
            if (!AddKeyToChannel(Channel, ChannelTypeName, Frame, Value, Interpolation, OutError))
            {
                return nullptr;
            }

            // Sections do not grow to fit new keys on their own; a key outside the range is
            // authored but never evaluated.
            const TRange<FFrameNumber> Range = Section->GetRange();
            if (!Range.Contains(Frame))
            {
                Section->SetRange(TRange<FFrameNumber>::Hull(Range, TRange<FFrameNumber>::Inclusive(Frame, Frame)));
            }

            MarkStructurallyModified(*Blueprint);

            TSharedPtr<FJsonObject> Result = Ok();
            Result->SetNumberField(TEXT("frame"), Frame.Value);
            Result->SetNumberField(TEXT("key_count"), Channel->GetNumKeys());
            return Result;
        });

    // widget.anim_key_list — keys on one channel. Params: widget_path, animation_name,
    // widget_name, property, channel / channel_index, section_index.
    Registry.Register(TEXT("widget.anim_key_list"),
        [](const TSharedPtr<FJsonObject>& Params, FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Params, OutError);
            if (!Blueprint) return nullptr;

            UWidgetAnimation* Animation = ResolveAnimationParam(Params, *Blueprint, OutError);
            if (!Animation) return nullptr;

            FString WidgetName, PropertyName;
            if (!RequireString(Params, TEXT("widget_name"), WidgetName, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("property"), PropertyName, OutError)) return nullptr;

            const FWidgetAnimationBinding* Binding = FindWidgetBinding(*Animation, WidgetName);
            if (!Binding)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("widget '%s' is not bound to this animation"), *WidgetName));
                return nullptr;
            }

            UMovieScene* MovieScene = Animation->MovieScene;
            UMovieSceneTrack* Target = FindPropertyTrack(*MovieScene, Binding->AnimationGuid, FName(*PropertyName));

            if (!Target)
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("no track for property '%s' on widget '%s'"), *PropertyName, *WidgetName));
                return nullptr;
            }

            int32 SectionIndex = 0;
            if (double SectionValue; Params->TryGetNumberField(TEXT("section_index"), SectionValue))
            {
                SectionIndex = static_cast<int32>(SectionValue);
            }
            const TArray<UMovieSceneSection*>& Sections = Target->GetAllSections();
            if (!Sections.IsValidIndex(SectionIndex) || !Sections[SectionIndex])
            {
                Fail(OutError, FMCPError::InvalidParams,
                    FString::Printf(TEXT("section_index %d out of range (track has %d sections)"),
                        SectionIndex, Sections.Num()));
                return nullptr;
            }

            FString ChannelName;
            Params->TryGetStringField(TEXT("channel"), ChannelName);
            int32 ChannelIndex = 0;
            if (double ChannelValue; Params->TryGetNumberField(TEXT("channel_index"), ChannelValue))
            {
                ChannelIndex = static_cast<int32>(ChannelValue);
            }

            FMovieSceneChannel* Channel = FindChannel(*Sections[SectionIndex], ChannelName, ChannelIndex);
            if (!Channel)
            {
                Fail(OutError, FMCPError::InvalidParams, TEXT("channel not found on this section"));
                return nullptr;
            }

            TArray<FFrameNumber> Times;
            TArray<FKeyHandle> Handles;
            Channel->GetKeys(TRange<FFrameNumber>::All(), &Times, &Handles);

            TArray<TSharedPtr<FJsonValue>> Entries;
            for (const FFrameNumber& Frame : Times)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetNumberField(TEXT("frame"), Frame.Value);
                Entry->SetNumberField(TEXT("time"), FrameToSeconds(*MovieScene, Frame));
                Entries.Add(MakeShared<FJsonValueObject>(Entry));
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("count"), Entries.Num());
            Result->SetArrayField(TEXT("keys"), Entries);
            return Result;
        });
}
