#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Handles the Maya <-> Unreal FBX exchange: level.export_selected, asset.export_fbx,
// asset.import_fbx, level.spawn_asset. Grouped by workflow rather than by namespace —
// asset.import_fbx is the return leg of the same round trip the export commands start.
class FLevelIOCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
