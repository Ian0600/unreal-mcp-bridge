#include "Commands/AnimNotifyCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimTypes.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "UObject/UnrealType.h"
#include "Commands/InspectorInvoke.h"

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
    // ---- BEGIN commands forwarded to BlueprintInspector (generated from its function headers) ----
    Registry.Register(TEXT("animnotify.dump_details"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BlueprintInspectorLibrary"), TEXT("DumpNotifyDetails"), { TEXT("asset_path") }));
    Registry.Register(TEXT("animnotify.set_event_timing"), MakeInspectorCommand(TEXT("/Script/BlueprintInspector.BlueprintInspectorLibrary"), TEXT("SetNotifyEventTiming"), { TEXT("asset_path"), TEXT("notify_index"), TEXT("new_trigger_time"), TEXT("new_duration") }));
    // ---- END forwarded commands ----

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

    // animnotify.set_properties — reconfigures an event that is already placed. Params: animation_path,
    // index (from animnotify.list), properties { name: "ImportText value" }.
    // Returns { ok, class, applied: [names] }.
    //
    // The alternative — remove and re-add — is not equivalent: a notify's Instanced sub-objects are
    // rebuilt from class defaults on creation, so re-adding silently drops the rest of the animator's
    // config. Same reason animnotify.set_window edits in place.
    Registry.Register(TEXT("animnotify.set_properties"),
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

            const TSharedPtr<FJsonObject>* Props = nullptr;
            if (!Params->TryGetObjectField(TEXT("properties"), Props) || !Props || !Props->IsValid())
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("properties is required and must be an object");
                return nullptr;
            }

            FAnimNotifyEvent& Event = Anim->Notifies[Index];
            UObject* Target = Event.NotifyStateClass ? static_cast<UObject*>(Event.NotifyStateClass)
                                                     : static_cast<UObject*>(Event.Notify);
            if (!Target)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("notify %d has no notify object to configure"), Index);
                return nullptr;
            }

            Anim->Modify();
            Target->Modify();
            if (!ApplyProperties(Target, *Props, OutError))
            {
                return nullptr;   // 어느 속성에서 걸렸는지는 ApplyProperties 가 메시지에 담았다.
            }

            Anim->RefreshCacheData();
            Anim->MarkPackageDirty();

            TArray<TSharedPtr<FJsonValue>> Applied;
            for (const auto& Pair : (*Props)->Values)
            {
                // 5.8 의 FJsonObject 키는 FString 이 아니라 공유 문자열 타입이다 - 명시 변환이 필요하다.
                Applied.Add(MakeShared<FJsonValueString>(FString(*Pair.Key)));
            }

            auto PropResult = MakeShared<FJsonObject>();
            PropResult->SetBoolField(TEXT("ok"), true);
            PropResult->SetStringField(TEXT("class"), EventClassName(Event));
            PropResult->SetArrayField(TEXT("applied"), Applied);
            return PropResult;
        });

    // animnotify.remove — Params: animation_path, index (from animnotify.list).
    // Returns { ok, removed: { class, start_time, duration }, remaining }.
    //
    // **Indices shift.** They are positions in a list that gets re-sorted by time, so removing one
    // invalidates every index read before it. Re-list between removals rather than walking a saved
    // list backwards — the order you saved is not guaranteed to be the sorted one.
    Registry.Register(TEXT("animnotify.remove"),
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

            const FAnimNotifyEvent& Doomed = Anim->Notifies[Index];
            auto Removed = MakeShared<FJsonObject>();
            Removed->SetStringField(TEXT("class"), EventClassName(Doomed));
            Removed->SetNumberField(TEXT("start_time"), Doomed.GetTriggerTime());
            Removed->SetNumberField(TEXT("duration"), Doomed.GetDuration());

            Anim->Modify();
            Anim->Notifies.RemoveAt(Index);

            /* 남은 이벤트의 링크를 다시 잡는다. 몽타주는 노티를 세그먼트에 걸어 두므로(상태 노티는
               EndLink 까지), 배열만 줄이고 두면 다음 편집에서 엉뚱한 시각으로 되살아난다. */
            Anim->RefreshCacheData();
            Anim->MarkPackageDirty();

            auto RemoveResult = MakeShared<FJsonObject>();
            RemoveResult->SetBoolField(TEXT("ok"), true);
            RemoveResult->SetField(TEXT("removed"), MakeShared<FJsonValueObject>(Removed));
            RemoveResult->SetNumberField(TEXT("remaining"), Anim->Notifies.Num());
            return RemoveResult;
        });
}
