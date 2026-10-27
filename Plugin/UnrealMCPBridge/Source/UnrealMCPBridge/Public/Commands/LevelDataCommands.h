#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Handles the level data authoring commands — the saved counterpart of scene.*:
//   level.query_actors        filter by class / folder / label / tag / sphere / box, read chosen properties
//   level.actor_spawn         place saved actors with label, folder, scale and initial properties (dry_run)
//   level.actors_set_properties  many actors x many properties in one undo step (dry_run = diff only)
//   level.actor_add_instanced add an instanced subobject to an array property (TriggerActions, ...)
//   level.dirty_files         dirty map / external actor / external object packages and their files
//   level.spline_set_points   rewrite a spline component's points (patrol routes)
//   level.volume_set_extent   resize box / sphere / capsule shapes and brush volumes
//   nav.project_point / nav.test_path   is a point on the navmesh, does a path connect
//
// Everything that writes goes through one FScopedTransaction per call (one Ctrl+Z) and the
// shared reflection path in CommandReflectionHelpers.h, and nothing here saves — saving and
// Perforce stay with the caller's approval flow (asset.save, level.dirty_files for the list).
class FLevelDataCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
