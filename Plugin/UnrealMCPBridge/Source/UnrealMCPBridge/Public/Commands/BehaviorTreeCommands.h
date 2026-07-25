#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Handles behaviortree.* JSON-RPC methods — reading and authoring UBehaviorTree assets and their
// UBlackboardData: walk the tree, list/add blackboard keys, set node-instance properties, add
// composite/task nodes and decorator/service subnodes, and compile (regenerate the runtime tree from
// the editor graph). Uses the AIModule runtime nodes + the BehaviorTreeEditor/AIGraph editor graph.
class FBehaviorTreeCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
