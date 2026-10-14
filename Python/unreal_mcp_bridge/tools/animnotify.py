from __future__ import annotations

from fastmcp import FastMCP

from ..connection import UnrealConnection


def register_animnotify_tools(mcp: FastMCP, conn: UnrealConnection) -> None:
    @mcp.tool()
    def animnotify_list(animation_path: str, notify_class: str = "") -> dict:
        """Lists the notify events on an animation asset (sequence or montage).

        notify_class optionally filters by class name or path and matches subclasses, e.g.
        "AnimNotifyState_MotionWarping" also returns project subclasses of it.

        Returns {"notifies": [{"index", "class", "start_time", "duration", "track"}]}. The index is
        what statetree-style edits need — pass it to animnotify_set_window."""
        if not animation_path:
            raise ValueError("animation_path is required")
        params: dict = {"animation_path": animation_path}
        if notify_class:
            params["notify_class"] = notify_class
        return conn.call("animnotify.list", params)

    @mcp.tool()
    def animnotify_add(
        animation_path: str,
        notify_class: str,
        start_time: float,
        duration: float | None = None,
        track: int = 0,
        properties: dict | None = None,
    ) -> dict:
        """Places a NEW notify (or notify state) on an animation asset.

        Python cannot do this at all: UAnimSequenceBase::Notifies is script-protected, so a montage's
        hit window is unreachable from editor_python_exec.

        notify_class takes a full object path ("/Script/IFSR.AnimNotifyState_MonsterMeleeHit") or a bare
        class name. Path form is preferred — a bare-name lookup is a global scan that can hit a
        same-named class in another module.

        duration is required for notify states and rejected for plain notifies.

        properties optionally configures the new notify, as {name: "ImportText value"} — the same text
        form the editor writes, so gameplay tags go in as '(TagName="Anim.Reaction.Hit.L")', enums as
        their enumerator name, and so on. Set the sub-object config here rather than after the fact:
        re-creating a notify to change it rebuilds its Instanced sub-objects from class defaults.

        Returns {"ok", "index", "class", "start_time", "duration", "track"}. The index is the position
        AFTER the notify list is re-sorted by time — pass that one to animnotify_set_window, not the
        order you added things in."""
        if not animation_path:
            raise ValueError("animation_path is required")
        if not notify_class:
            raise ValueError("notify_class is required")
        params: dict = {
            "animation_path": animation_path,
            "notify_class": notify_class,
            "start_time": start_time,
            "track": track,
        }
        if duration is not None:
            params["duration"] = duration
        if properties:
            params["properties"] = properties
        return conn.call("animnotify.add", params)

    @mcp.tool()
    def animnotify_set_window(
        animation_path: str,
        index: int,
        start_time: float | None = None,
        duration: float | None = None,
    ) -> dict:
        """Retimes an existing notify event in place. index comes from animnotify_list. Pass start_time
        and/or duration (duration applies to notify states only).

        Edits the event in place rather than removing and re-adding it — a notify's Instanced sub-objects
        (a MotionWarping notify's RootMotionModifier, for instance) are rebuilt from class defaults on
        re-creation, which silently discards whatever the animator configured on them.

        Returns {"ok", "class", "start_time", "duration"} reflecting the stored values."""
        if not animation_path:
            raise ValueError("animation_path is required")
        if start_time is None and duration is None:
            raise ValueError("provide start_time and/or duration")
        params: dict = {"animation_path": animation_path, "index": index}
        if start_time is not None:
            params["start_time"] = start_time
        if duration is not None:
            params["duration"] = duration
        return conn.call("animnotify.set_window", params)

    @mcp.tool()
    def animnotify_set_properties(animation_path: str, index: int, properties: dict) -> dict:
        """Reconfigures a notify that is already placed. index comes from animnotify_list, properties
        is {name: "ImportText value"} — the same text form animnotify_add takes.

        Use this rather than remove-and-re-add: a notify's Instanced sub-objects are rebuilt from
        class defaults on creation, so re-adding one silently discards the rest of the animator's
        configuration on it.

        Returns {"ok", "class", "applied": [names]}."""
        if not animation_path:
            raise ValueError("animation_path is required")
        if not properties:
            raise ValueError("properties (non-empty) is required")
        return conn.call("animnotify.set_properties", {
            "animation_path": animation_path,
            "index": index,
            "properties": properties,
        })

    @mcp.tool()
    def animnotify_remove(animation_path: str, index: int) -> dict:
        """Removes a notify event. index comes from animnotify_list.

        Indices shift. They are positions in a list that gets re-sorted by time, so removing one
        invalidates every index read before it — re-list between removals rather than walking a
        saved list backwards, because the order you saved is not guaranteed to be the sorted one.

        Returns {"ok", "removed": {"class", "start_time", "duration"}, "remaining"}."""
        if not animation_path:
            raise ValueError("animation_path is required")
        return conn.call("animnotify.remove", {
            "animation_path": animation_path,
            "index": index,
        })

    @mcp.tool()
    def animnotify_dump_details(asset_path: str) -> dict:
        """Sequence/Montage 공용 노티파이 상세 덤프.
        엔진 Python 미노출 필드(TriggerTime/Duration/TrackIndex) + 인스턴스 노티파이의
        EditAnywhere 프로퍼티를 key=value 로 직렬화 (커스텀 노티파이 클래스 대응).

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::DumpNotifyDetails; no other plugin needed."""
        return conn.call("animnotify.dump_details", {"asset_path": asset_path})

    @mcp.tool()
    def animnotify_set_event_timing(asset_path: str, notify_index: int, new_trigger_time: float, new_duration: float) -> dict:
        """기존 노티파이의 시간/듀레이션 in-place 수정.
        (GetAnimationNotifyEvents 는 복사본을 반환해 Python 으로는 수정 불가한 갭)
        NewDuration < 0 = 듀레이션 유지. NotifyState 가 아니면 듀레이션 무시.

        Built-in port of BlueprintInspector's BlueprintInspectorLibrary::SetNotifyEventTiming; no other plugin needed."""
        return conn.call("animnotify.set_event_timing", {"asset_path": asset_path, "notify_index": notify_index, "new_trigger_time": new_trigger_time, "new_duration": new_duration})
