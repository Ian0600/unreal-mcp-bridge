#include "Commands/AnimNotifyCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimTypes.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

namespace
{
    UAnimSequenceBase* LoadAnim(const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        FString Path;
        if (!Params.IsValid() || !Params->TryGetStringField(TEXT("animation_path"), Path) || Path.IsEmpty())
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = TEXT("animation_path is required and must be non-empty");
            return nullptr;
        }
        UAnimSequenceBase* Anim = LoadObject<UAnimSequenceBase>(nullptr, *Path);
        if (!Anim)
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("Could not load animation: %s"), *Path);
        }
        return Anim;
    }

    // skeleton_path(스켈레톤 · 스켈레탈 메시) 또는 animation_path(그 애니의 스켈레톤) 중 하나.
    USkeleton* LoadNotifySkeleton(const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        FString Path;
        if (Params.IsValid() && Params->TryGetStringField(TEXT("skeleton_path"), Path) && !Path.IsEmpty())
        {
            UObject* Asset = LoadObject<UObject>(nullptr, *Path);
            if (USkeleton* Skeleton = Cast<USkeleton>(Asset))
            {
                return Skeleton;
            }
            if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset))
            {
                if (Mesh->GetSkeleton()) { return Mesh->GetSkeleton(); }
            }
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("Could not load a Skeleton (or SkeletalMesh with one) from %s"), *Path);
            return nullptr;
        }
        if (Params.IsValid() && Params->HasField(TEXT("animation_path")))
        {
            UAnimSequenceBase* Anim = LoadAnim(Params, OutError);
            if (!Anim) { return nullptr; }
            if (USkeleton* Skeleton = Anim->GetSkeleton()) { return Skeleton; }
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("%s has no skeleton"), *Anim->GetName());
            return nullptr;
        }
        OutError.Code = MCPProtocol::FMCPError::InvalidParams;
        OutError.Message = TEXT("skeleton_path or animation_path is required");
        return nullptr;
    }

    TSharedPtr<FJsonObject> MakeSkeletonNotifyNamesResult(const USkeleton* Skeleton)
    {
        TArray<TSharedPtr<FJsonValue>> Names;
        for (const FName& Name : Skeleton->AnimationNotifies)
        {
            Names.Add(MakeShared<FJsonValueString>(Name.ToString()));
        }
        auto Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
        Result->SetArrayField(TEXT("names"), Names);
        return Result;
    }

    // Name of whatever sits on this event, notify or notify-state, for reporting/filtering.
    FString EventClassName(const FAnimNotifyEvent& Event)
    {
        if (Event.NotifyStateClass) { return Event.NotifyStateClass->GetClass()->GetName(); }
        if (Event.Notify)           { return Event.Notify->GetClass()->GetName(); }
        return TEXT("(none)");
    }

    bool EventMatchesFilter(const FAnimNotifyEvent& Event, const FString& ClassFilter)
    {
        if (ClassFilter.IsEmpty())
        {
            return true;
        }
        const UClass* Class = Event.NotifyStateClass ? Event.NotifyStateClass->GetClass()
                            : (Event.Notify ? Event.Notify->GetClass() : nullptr);
        for (const UClass* It = Class; It; It = It->GetSuperClass())
        {
            if (It->GetName() == ClassFilter || It->GetPathName() == ClassFilter)
            {
                return true;
            }
        }
        return false;
    }

    // Resolves a notify class from either a full object path (/Script/IFSR.AnimNotifyState_X) or a bare
    // class name. Path first: a bare name search is a global scan that can hit a same-named class in
    // another module, and silently placing the wrong notify is worse than failing.
    UClass* ResolveNotifyClass(const FString& ClassName, MCPProtocol::FMCPError& OutError)
    {
        UClass* Class = FindObject<UClass>(nullptr, *ClassName);
        if (!Class)
        {
            Class = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::ExactClass);
        }
        if (!Class)
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("Could not find notify class: %s"), *ClassName);
            return nullptr;
        }
        if (!Class->IsChildOf(UAnimNotify::StaticClass()) && !Class->IsChildOf(UAnimNotifyState::StaticClass()))
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("%s is neither a UAnimNotify nor a UAnimNotifyState"), *Class->GetName());
            return nullptr;
        }
        if (Class->HasAnyClassFlags(CLASS_Abstract))
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("%s is abstract"), *Class->GetName());
            return nullptr;
        }
        return Class;
    }

    // Applies { "PropName": "text value" } onto the freshly created notify object via ImportText —
    // the same route camerarig.set_node_property takes, so enums/structs/gameplay tags all go in as the
    // text the editor itself would write. Without this a MonsterMeleeHit lands with an empty ReactionTag.
    bool ApplyProperties(UObject* Target, const TSharedPtr<FJsonObject>& Props, MCPProtocol::FMCPError& OutError)
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
            if (!Pair.Value->TryGetString(ValueText))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("property '%s' must be given as a string (ImportText form)"), *Pair.Key);
                return false;
            }
            void* Addr = Property->ContainerPtrToValuePtr<void>(Target);
            const TCHAR* Result = Property->ImportText_Direct(*ValueText, Addr, Target, PPF_None);
            if (!Result)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("could not parse '%s' for property '%s'"), *ValueText, *Pair.Key);
                return false;
            }
        }
        return true;
    }
}

void FAnimNotifyCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    // animnotify.list — lists notify events on an animation. Params: animation_path,
    // notify_class (optional filter, class name or path; matches subclasses too).
    // Returns { notifies: [ { index, class, start_time, duration, track } ] }.
    Registry.Register(TEXT("animnotify.list"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimSequenceBase* Anim = LoadAnim(Params, OutError); if (!Anim) return nullptr;
            FString ClassFilter; Params->TryGetStringField(TEXT("notify_class"), ClassFilter);

            TArray<TSharedPtr<FJsonValue>> Out;
            for (int32 Index = 0; Index < Anim->Notifies.Num(); ++Index)
            {
                const FAnimNotifyEvent& Event = Anim->Notifies[Index];
                if (!EventMatchesFilter(Event, ClassFilter))
                {
                    continue;
                }
                auto Entry = MakeShared<FJsonObject>();
                Entry->SetNumberField(TEXT("index"), Index);
                Entry->SetStringField(TEXT("class"), EventClassName(Event));
                Entry->SetStringField(TEXT("name"), Event.NotifyName.ToString());   // 이름 노티(class "(none)")를 가려 보려고
                Entry->SetNumberField(TEXT("start_time"), Event.GetTriggerTime());
                Entry->SetNumberField(TEXT("duration"), Event.GetDuration());
                Entry->SetNumberField(TEXT("track"), Event.TrackIndex);
                Out.Add(MakeShared<FJsonValueObject>(Entry));
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("notifies"), Out);
            return Result;
        });

    // animnotify.add — places a new notify (or notify state) on an animation. Params: animation_path,
    // notify_class (path or bare class name), start_time, duration (notify states only), track
    // (optional, default 0), properties (optional { name: "ImportText value" } applied to the new notify).
    // Returns { ok, index, class, start_time, duration, track }.
    //
    // The index comes back **after** the sort: RefreshCacheData reorders Notifies by time, so the slot
    // the array append returned is not where the event ends up. Callers that immediately retime it with
    // animnotify.set_window would otherwise edit whatever event happened to land in that slot.
    Registry.Register(TEXT("animnotify.add"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimSequenceBase* Anim = LoadAnim(Params, OutError); if (!Anim) return nullptr;

            FString ClassName;
            if (!UnrealMCPBridge::Json::RequireString(Params, TEXT("notify_class"), ClassName, OutError))
            {
                return nullptr;
            }
            UClass* NotifyClass = ResolveNotifyClass(ClassName, OutError);
            if (!NotifyClass)
            {
                return nullptr;
            }
            const bool bIsState = NotifyClass->IsChildOf(UAnimNotifyState::StaticClass());

            double StartTime = 0.0;
            if (!Params->TryGetNumberField(TEXT("start_time"), StartTime))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("start_time is required");
                return nullptr;
            }
            double Duration = 0.0;
            const bool bHasDuration = Params->TryGetNumberField(TEXT("duration"), Duration);
            if (bIsState && (!bHasDuration || Duration <= 0.0))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("%s is a notify state — duration is required and must be > 0"), *NotifyClass->GetName());
                return nullptr;
            }
            if (!bIsState && bHasDuration)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("duration only applies to notify states; this class is a plain notify");
                return nullptr;
            }

            const float PlayLength = Anim->GetPlayLength();
            if (StartTime < 0.0 || StartTime > PlayLength || (StartTime + Duration) > PlayLength + KINDA_SMALL_NUMBER)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("window %.3f..%.3f falls outside the animation (0..%.3f)"),
                    StartTime, StartTime + Duration, PlayLength);
                return nullptr;
            }

            double TrackValue = 0.0;
            Params->TryGetNumberField(TEXT("track"), TrackValue);
            int32 Track = FMath::Max(0, static_cast<int32>(TrackValue));
#if WITH_EDITORONLY_DATA
            // A track index past the end leaves the event invisible in the montage editor even though it
            // fires at runtime, which reads as "the command did nothing". Clamp into what exists.
            if (Anim->AnimNotifyTracks.Num() > 0)
            {
                Track = FMath::Min(Track, Anim->AnimNotifyTracks.Num() - 1);
            }
            else
            {
                Track = 0;
            }
#endif

            Anim->Modify();

            // Outer is the animation, matching the editor's own placement (SAnimNotifyTrack) — a notify
            // outered anywhere else is not saved with the asset.
            UObject* NotifyObject = NewObject<UObject>(Anim, NotifyClass, NAME_None, RF_Transactional);
            // TryGet, not GetObjectField: the latter logs an error and hands back an invalid pointer when
            // the field is absent, and "properties" is optional.
            const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
            Params->TryGetObjectField(TEXT("properties"), PropsPtr);
            if (!ApplyProperties(NotifyObject, PropsPtr ? *PropsPtr : nullptr, OutError))
            {
                return nullptr;
            }

            FAnimNotifyEvent NewEvent;
            if (bIsState)
            {
                NewEvent.NotifyStateClass = Cast<UAnimNotifyState>(NotifyObject);
            }
            else
            {
                NewEvent.Notify = Cast<UAnimNotify>(NotifyObject);
            }
            NewEvent.NotifyName = FName(*NotifyClass->GetName()
                .Replace(TEXT("AnimNotifyState_"), TEXT(""))
                .Replace(TEXT("AnimNotify_"), TEXT("")));
            NewEvent.TrackIndex = Track;
#if WITH_EDITORONLY_DATA
            const FGuid NewGuid = FGuid::NewGuid();
            NewEvent.Guid = NewGuid;
#endif
            // Same link sequence as set_window, so a placed notify and a retimed one end up identical.
            NewEvent.Link(Anim, StartTime);
            NewEvent.RefreshTriggerOffset(Anim->CalculateOffsetForNotify(StartTime));
            if (bIsState)
            {
                NewEvent.SetDuration(Duration);
                NewEvent.EndLink.Link(Anim, StartTime + Duration);
                NewEvent.RefreshEndTriggerOffset(Anim->CalculateOffsetForNotify(StartTime + Duration));
            }

            Anim->Notifies.Add(NewEvent);
            Anim->RefreshCacheData();
            Anim->MarkPackageDirty();

            int32 FinalIndex = INDEX_NONE;
#if WITH_EDITORONLY_DATA
            for (int32 i = 0; i < Anim->Notifies.Num(); ++i)
            {
                if (Anim->Notifies[i].Guid == NewGuid) { FinalIndex = i; break; }
            }
#endif
            if (FinalIndex == INDEX_NONE)
            {
                FinalIndex = Anim->Notifies.Num() - 1;
            }

            const FAnimNotifyEvent& Stored = Anim->Notifies[FinalIndex];
            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            Result->SetNumberField(TEXT("index"), FinalIndex);
            Result->SetStringField(TEXT("class"), EventClassName(Stored));
            Result->SetNumberField(TEXT("start_time"), Stored.GetTriggerTime());
            Result->SetNumberField(TEXT("duration"), Stored.GetDuration());
            Result->SetNumberField(TEXT("track"), Stored.TrackIndex);
            return Result;
        });

    // animnotify.add_named — places a class-less named notify (a "skeleton notify", what the editor's
    // Add Notify → New Notify… makes) on a sequence or montage. Params: animation_path, notify_name,
    // time (seconds) or frame (at the animation's sampling rate), track_name (optional — created when
    // missing) or track (index, optional, default 0). Returns { ok, index, notify_name, start_time,
    // track, track_name, track_created, skeleton_notify_added }. Does not save — use asset.save.
    //
    // Python cannot do this: Notifies is protected and add_animation_notify_event_from_source adds
    // nothing without a notify object. The name is also registered on the skeleton
    // (USkeleton::AddNewAnimationNotify) so it shows in the notify picker and in AnimBP "AnimNotify_<name>" events.
    Registry.Register(TEXT("animnotify.add_named"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimSequenceBase* Anim = LoadAnim(Params, OutError); if (!Anim) return nullptr;

            FString NotifyNameStr;
            if (!UnrealMCPBridge::Json::RequireString(Params, TEXT("notify_name"), NotifyNameStr, OutError))
            {
                return nullptr;
            }
            const FName NotifyName(*NotifyNameStr);

            double StartTime = 0.0;
            double FrameValue = 0.0;
            const bool bHasTime = Params->TryGetNumberField(TEXT("time"), StartTime);
            const bool bHasFrame = Params->TryGetNumberField(TEXT("frame"), FrameValue);
            if (bHasTime == bHasFrame)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("give exactly one of time (seconds) or frame");
                return nullptr;
            }
            if (bHasFrame)
            {
                StartTime = Anim->GetSamplingFrameRate().AsSeconds(FFrameTime::FromDecimal(FrameValue));
            }
            const float PlayLength = Anim->GetPlayLength();
            if (StartTime < 0.0 || StartTime > PlayLength + KINDA_SMALL_NUMBER)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("time %.3f falls outside the animation (0..%.3f)"), StartTime, PlayLength);
                return nullptr;
            }
            StartTime = FMath::Min(StartTime, static_cast<double>(PlayLength));

            const FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("MCP: Add Notify %s"), *NotifyNameStr)));
            Anim->Modify();

            int32 Track = 0;
            bool bTrackCreated = false;
            FString TrackName;
#if WITH_EDITORONLY_DATA
            if (Params->TryGetStringField(TEXT("track_name"), TrackName) && !TrackName.IsEmpty())
            {
                Track = Anim->AnimNotifyTracks.IndexOfByPredicate(
                    [&TrackName](const FAnimNotifyTrack& T) { return T.TrackName == FName(*TrackName); });
                if (Track == INDEX_NONE)
                {
                    Track = Anim->AnimNotifyTracks.Add(FAnimNotifyTrack(FName(*TrackName), FLinearColor::White));
                    bTrackCreated = true;
                }
            }
            else
            {
                double TrackValue = 0.0;
                Params->TryGetNumberField(TEXT("track"), TrackValue);
                Track = FMath::Max(0, static_cast<int32>(TrackValue));
                if (Anim->AnimNotifyTracks.Num() == 0)
                {
                    // 트랙이 하나도 없는 애니 — 에디터가 열 때 만드는 기본 트랙 "1" 과 같이 만든다.
                    Anim->AnimNotifyTracks.Add(FAnimNotifyTrack(TEXT("1"), FLinearColor::White));
                    bTrackCreated = true;
                }
                Track = FMath::Min(Track, Anim->AnimNotifyTracks.Num() - 1);
            }
            TrackName = Anim->AnimNotifyTracks[Track].TrackName.ToString();
#endif

            // 스켈레톤 노티 목록에 등록 — 이미 있으면 그대로 둔다.
            bool bSkeletonNotifyAdded = false;
            if (USkeleton* Skeleton = Anim->GetSkeleton())
            {
                if (!Skeleton->AnimationNotifies.Contains(NotifyName))
                {
                    Skeleton->Modify();
                    Skeleton->AddNewAnimationNotify(NotifyName);
                    Skeleton->MarkPackageDirty();
                    bSkeletonNotifyAdded = true;
                }
            }

            FAnimNotifyEvent NewEvent;
            NewEvent.NotifyName = NotifyName;   // Notify · NotifyStateClass 둘 다 비움 = 이름 노티
            NewEvent.TrackIndex = Track;
#if WITH_EDITORONLY_DATA
            const FGuid NewGuid = FGuid::NewGuid();
            NewEvent.Guid = NewGuid;
#endif
            NewEvent.Link(Anim, StartTime);
            NewEvent.RefreshTriggerOffset(Anim->CalculateOffsetForNotify(StartTime));

            Anim->Notifies.Add(NewEvent);
            Anim->RefreshCacheData();
            Anim->MarkPackageDirty();

            int32 FinalIndex = INDEX_NONE;
#if WITH_EDITORONLY_DATA
            for (int32 i = 0; i < Anim->Notifies.Num(); ++i)
            {
                if (Anim->Notifies[i].Guid == NewGuid) { FinalIndex = i; break; }
            }
#endif
            if (FinalIndex == INDEX_NONE)
            {
                FinalIndex = Anim->Notifies.Num() - 1;
            }

            const FAnimNotifyEvent& Stored = Anim->Notifies[FinalIndex];
            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            Result->SetNumberField(TEXT("index"), FinalIndex);
            Result->SetStringField(TEXT("notify_name"), Stored.NotifyName.ToString());
            Result->SetNumberField(TEXT("start_time"), Stored.GetTriggerTime());
            Result->SetNumberField(TEXT("track"), Stored.TrackIndex);
            Result->SetStringField(TEXT("track_name"), TrackName);
            Result->SetBoolField(TEXT("track_created"), bTrackCreated);
            Result->SetBoolField(TEXT("skeleton_notify_added"), bSkeletonNotifyAdded);
            return Result;
        });

    // animnotify.list_skeleton_names — the skeleton's named-notify list (what the notify picker offers and
    // what AnimBP "AnimNotify_<name>" events come from). Python cannot read it: Skeleton.AnimationNotifies
    // is protected. Params: skeleton_path (Skeleton or SkeletalMesh) or animation_path.
    // Returns { skeleton, names: [..] }.
    Registry.Register(TEXT("animnotify.list_skeleton_names"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            USkeleton* Skeleton = LoadNotifySkeleton(Params, OutError); if (!Skeleton) return nullptr;
            return MakeSkeletonNotifyNamesResult(Skeleton);
        });

    // animnotify.add_skeleton_names — registers names on the skeleton without placing any notify, so they
    // are in the picker before anyone keys them. Params: skeleton_path or animation_path, names: [..].
    // Returns { skeleton, added: [..], existing: [..], names: [full list] }. Does not save — use asset.save.
    Registry.Register(TEXT("animnotify.add_skeleton_names"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            USkeleton* Skeleton = LoadNotifySkeleton(Params, OutError); if (!Skeleton) return nullptr;

            const TArray<TSharedPtr<FJsonValue>>* NamesIn = nullptr;
            if (!Params->TryGetArrayField(TEXT("names"), NamesIn) || NamesIn->Num() == 0)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("names is required (non-empty array of strings)");
                return nullptr;
            }

            TArray<FName> ToAdd;
            TArray<TSharedPtr<FJsonValue>> Existing;
            for (const TSharedPtr<FJsonValue>& Value : *NamesIn)
            {
                FString NameStr;
                if (!Value->TryGetString(NameStr) || NameStr.IsEmpty())
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = TEXT("names must be non-empty strings");
                    return nullptr;
                }
                const FName Name(*NameStr);
                if (Skeleton->AnimationNotifies.Contains(Name))
                {
                    Existing.Add(MakeShared<FJsonValueString>(NameStr));
                }
                else
                {
                    ToAdd.AddUnique(Name);
                }
            }

            TArray<TSharedPtr<FJsonValue>> Added;
            if (ToAdd.Num() > 0)
            {
                const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Add Skeleton Notify Names")));
                Skeleton->Modify();
                for (const FName& Name : ToAdd)
                {
                    Skeleton->AddNewAnimationNotify(Name);
                    Added.Add(MakeShared<FJsonValueString>(Name.ToString()));
                }
                Skeleton->MarkPackageDirty();
            }

            TSharedPtr<FJsonObject> Result = MakeSkeletonNotifyNamesResult(Skeleton);
            Result->SetArrayField(TEXT("added"), Added);
            Result->SetArrayField(TEXT("existing"), Existing);
            return Result;
        });

    // animnotify.set_window — retimes an existing notify event in place. Params: animation_path, index
    // (from animnotify.list), start_time (optional), duration (optional; notify states only).
    // Returns { ok, class, start_time, duration }.
    // In place on purpose: removing and re-adding the event would rebuild its Instanced sub-objects from
    // class defaults, quietly dropping whatever the animator configured on them.
    Registry.Register(TEXT("animnotify.set_window"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UAnimSequenceBase* Anim = LoadAnim(Params, OutError); if (!Anim) return nullptr;

            double IndexValue = -1.0;
            if (!Params->TryGetNumberField(TEXT("index"), IndexValue))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("index is required (see animnotify.list)");
                return nullptr;
            }
            const int32 Index = static_cast<int32>(IndexValue);
            if (!Anim->Notifies.IsValidIndex(Index))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("index %d out of range (%d notifies)"), Index, Anim->Notifies.Num());
                return nullptr;
            }

            FAnimNotifyEvent& Event = Anim->Notifies[Index];

            double StartTime = Event.GetTriggerTime();
            double Duration = Event.GetDuration();
            const bool bHasStart = Params->TryGetNumberField(TEXT("start_time"), StartTime);
            const bool bHasDuration = Params->TryGetNumberField(TEXT("duration"), Duration);
            if (!bHasStart && !bHasDuration)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("provide start_time and/or duration");
                return nullptr;
            }

            const float PlayLength = Anim->GetPlayLength();
            if (StartTime < 0.0 || StartTime > PlayLength || (StartTime + Duration) > PlayLength + KINDA_SMALL_NUMBER)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("window %.3f..%.3f falls outside the animation (0..%.3f)"),
                    StartTime, StartTime + Duration, PlayLength);
                return nullptr;
            }
            if (bHasDuration && !Event.NotifyStateClass)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("duration only applies to notify states; this event is a plain notify");
                return nullptr;
            }

            Anim->Modify();

            // Same sequence the editor uses when placing a notify: re-link to the new time, refresh the
            // trigger offset, then re-link the end so the state's window follows.
            Event.Link(Anim, StartTime);
            Event.RefreshTriggerOffset(Anim->CalculateOffsetForNotify(StartTime));
            if (Event.NotifyStateClass)
            {
                Event.SetDuration(Duration);
                Event.EndLink.Link(Anim, StartTime + Duration);
                Event.RefreshEndTriggerOffset(Anim->CalculateOffsetForNotify(StartTime + Duration));
            }

            Anim->RefreshCacheData();
            Anim->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            Result->SetStringField(TEXT("class"), EventClassName(Event));
            Result->SetNumberField(TEXT("start_time"), Event.GetTriggerTime());
            Result->SetNumberField(TEXT("duration"), Event.GetDuration());
            return Result;
        });
}
