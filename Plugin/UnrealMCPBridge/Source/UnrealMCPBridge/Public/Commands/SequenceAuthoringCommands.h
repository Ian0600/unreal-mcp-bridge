#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Handles sequence.* JSON-RPC methods — building a Level Sequence for a shot and photographing one
// frame of it, without opening Sequencer:
//   sequence.create         a transient sequence (never in Content, so no file outside Perforce)
//   sequence.add_track      transform / skeletal animation / camera cut tracks, binding level
//                           actors (possessables) or spawning assets (via sequencer.add_spawnable)
//   sequence.set_keys       transform keys on a binding
//   sequence.capture_frame  evaluate one frame and render it from the camera cut's camera
//                           (via scene.capture_view), then put every animated actor back
//   sequence.discard        drop a transient sequence
//
// The sequencer.* family (SequencerCommands.cpp) inspects and bakes existing sequences; the two
// share its loading, binding-name and frame helpers through SequencerHelpers.h.
class FSequenceAuthoringCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
