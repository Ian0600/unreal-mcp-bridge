#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MCPProtocol.h"
#include "UObject/WeakObjectPtr.h"

class AActor;
class UPrimitiveComponent;
class UWorld;

/**
 * Pieces of scene.* that the level data commands reuse — the editor world lookup, the dirty
 * package report both families return, and the ground trace (scene.get_ground_height and the
 * snap_to_ground option of scene.spawn_temp / level.actor_spawn are one implementation).
 * Defined in SceneCommands.cpp.
 */
namespace UnrealMCPBridge::Scene
{
    /** Every actor scene.spawn_temp / scene.pose_character places carries this tag, whatever tag the caller adds. */
    extern const FName TempActorTag;

    /** The editor world, or null with OutError set. PIE does not change which world this is. */
    UWorld* GetEditorWorld(MCPProtocol::FMCPError& OutError);

    /** Names of the packages that were dirty when the snapshot was taken. */
    struct FDirtySnapshot
    {
        TSet<FName> DirtyPackages;
    };

    FDirtySnapshot SnapshotDirtyPackages();

    /**
     * Writes the "did this call dirty anything" report into Result:
     *   level_dirty      — true when any package became dirty during the call (the check that
     *                      a temp-only tool kept its promise)
     *   dirtied_packages — those packages
     *   map_dirty        — whether World's map package is dirty now, for whatever reason
     */
    void WriteDirtyReport(UWorld* World, const FDirtySnapshot& Before, const TSharedPtr<FJsonObject>& Result);

    /** One surface the ground trace passed through, top first. */
    struct FGroundLayer
    {
        FVector Point = FVector::ZeroVector;
        FVector Normal = FVector::UpVector;
        TWeakObjectPtr<AActor> Actor;
        TWeakObjectPtr<UPrimitiveComponent> Component;
    };

    struct FGroundQuery
    {
        bool bIgnoreVolumes = true;   // AVolume actors, brush and shape (box/sphere/capsule) components
        bool bIgnoreTemp = true;      // actors carrying TempActorTag
        double TopZ = 200000.0;
        double BottomZ = -200000.0;
        int32 MaxLayers = 16;
        TArray<TWeakObjectPtr<AActor>> IgnoreActors;
    };

    /**
     * Traces straight down at (X, Y) on the Visibility channel and collects the top surface of each
     * component the ray passes, skipping the ignored kinds. Each hit component is ignored for the
     * next trace, so a building gives roof, then floor, then terrain — one surface per component.
     */
    void TraceGroundLayers(UWorld* World, double X, double Y, const FGroundQuery& Query, TArray<FGroundLayer>& OutLayers);

    /**
     * Picks one layer: Layer >= 0 counts from the top, -1 is the lowest. Returns false when there
     * is no such layer.
     */
    bool PickGroundLayer(const TArray<FGroundLayer>& Layers, int32 Layer, FGroundLayer& Out);

    /**
     * Parses the snap/ground options shared by scene.get_ground_height, scene.spawn_temp and
     * level.actor_spawn: ignore ([volumes, temp, roofs] — roofs means "use the lowest layer"),
     * layer, top_z, bottom_z.
     */
    void ReadGroundQuery(const TSharedPtr<FJsonObject>& Params, FGroundQuery& OutQuery, int32& OutLayer);

    /**
     * Moves Actor so that the bottom of its bounds rests on the chosen ground layer below its
     * current X/Y, plus ZOffset. Actors without primitive bounds (lights) put their pivot there.
     * Returns false with OutMessage when no ground was found; the actor is not moved then.
     */
    bool SnapActorToGround(AActor* Actor, const FGroundQuery& Query, int32 Layer, double ZOffset, FString& OutMessage);

    /** Bounds report for an actor: {center, extent, size_m} from its primitive components. */
    TSharedPtr<FJsonObject> MakeBoundsJson(AActor* Actor);

    /**
     * Finds a level actor in World by full object path, actor name or label (label match is
     * exact, case-insensitive). Ambiguous labels fail with every candidate listed.
     */
    AActor* FindActor(UWorld* World, const FString& NameOrPath, FString& OutError);
}
