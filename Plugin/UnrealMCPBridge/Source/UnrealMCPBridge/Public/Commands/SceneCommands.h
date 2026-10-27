#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Handles scene.* JSON-RPC methods — the photo tools: place things that are never saved
// (spawn_temp, pose_character), find the ground (get_ground_height), render a game-view image
// straight to a file (capture_view), borrow a level actor for a moment (temp_override), and do
// whole shot lists without a round trip per step (run_shots / shots_status).
//
// The "temp" promise is enforced here, not by the caller: temp actors are RF_Transient with no
// external package, nothing calls Modify on a real level actor, and nothing is saved. Every
// mutating command reports level_dirty / dirtied_packages so the caller can check it held.
class FSceneCommandHandler : public IMCPCommandHandler
{
public:
    virtual ~FSceneCommandHandler() override;
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
