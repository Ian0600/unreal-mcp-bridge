#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Handles object.* JSON-RPC methods — reads and writes ANY UObject property by reflection,
// on any resolvable object path: assets, their instanced subobjects, PIE actors, components.
//
// Why this exists: the editor's Python API only wraps properties the engine chose to expose
// (BlueprintType classes, EditAnywhere/BlueprintReadWrite members). Plenty of real authoring
// data sits outside that set — private UPROPERTY()s, protected members, and whole classes
// declared UCLASS(MinimalAPI). Those come back from Python as "Failed to find property" or
// "is protected and cannot be read", and the console `set` command is refused outright in the
// editor ("Set commands not allowed in the editor"). Reflection from C++ has no such limits:
// FindFProperty plus ImportText_Direct/ExportText_Direct ignore C++ access specifiers, which
// leaves this handler as the general escape hatch for headless asset editing.
//
// The per-domain handlers (camerarig.*, material.*, widget.*, ...) stay the better choice when
// one exists — they know the domain's invariants (rebuild the rig, recompile the blueprint,
// keep an editor graph in step). Use object.* for everything they do not cover.
class FObjectCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
