#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Handles blendspace.* (BlendSpace samples and axes) JSON-RPC methods.
// The commands forward to BlueprintInspector, which must be loaded for them to answer.
class FBlendSpaceCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
