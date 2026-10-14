#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Handles behaviortree.* (Behavior Tree and Blackboard authoring) JSON-RPC methods.
// The commands run BlueprintInspector's implementation, vendored in Private/Inspector; no other plugin is needed.
class FBehaviorTreeCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
