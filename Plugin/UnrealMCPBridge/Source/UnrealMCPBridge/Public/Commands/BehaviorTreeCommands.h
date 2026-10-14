#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Handles behaviortree.* (Behavior Tree and Blackboard authoring) JSON-RPC methods.
// The commands forward to BlueprintInspector, which must be loaded for them to answer.
class FBehaviorTreeCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
