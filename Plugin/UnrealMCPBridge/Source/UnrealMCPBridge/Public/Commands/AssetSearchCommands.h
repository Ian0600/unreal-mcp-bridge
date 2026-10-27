#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Read-only asset lookups for building scenes without a hand-made index: asset.find (name
// fragments + class), asset.bounds (mesh dimensions) and asset.anims_for_skeleton (clips a mesh can
// play, compatible skeletons included). All of it comes from the asset registry except bounds,
// which loads the meshes it is asked about.
class FAssetSearchCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
