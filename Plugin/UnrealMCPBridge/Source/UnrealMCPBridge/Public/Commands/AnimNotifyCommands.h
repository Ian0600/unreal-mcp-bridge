#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Handles animnotify.* JSON-RPC methods — inspecting, placing and retiming notifies on an animation
// asset. Python cannot do any of this: UAnimSequenceBase::Notifies is script-protected, so a montage's
// hit window is unreachable from editor_python_exec.
//
// Editing an existing event in place (rather than remove + re-add) matters: a notify's Instanced
// sub-objects (e.g. a MotionWarping notify's RootMotionModifier) do not survive re-creation, so
// re-adding silently resets the animator's config. animnotify.add is for placing a *new* event, and
// takes the sub-object config up front via its `properties` param for the same reason.
class FAnimNotifyCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
