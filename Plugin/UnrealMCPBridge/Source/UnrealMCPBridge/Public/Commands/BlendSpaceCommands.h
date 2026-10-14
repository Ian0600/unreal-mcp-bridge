#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Handles blendspace.* (BlendSpace samples and axes) JSON-RPC methods.
// The commands run BlueprintInspector's implementation, vendored in Private/Inspector; no other plugin is needed.
class FBlendSpaceCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
