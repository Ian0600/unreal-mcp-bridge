#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Handles widget.anim_* JSON-RPC methods — the animation half of Widget Blueprint authoring:
// animation assets, the widgets they possess, property tracks, sections and keyframes.
//
// A UWidgetAnimation is a UMovieSceneSequence, so everything below the animation itself is
// plain Sequencer data: UMovieScene owns the possessables and tracks, tracks own sections,
// and sections expose their keyframe channels through FMovieSceneChannelProxy. All of that
// is fully DLL-exported, so these commands edit the movie scene directly instead of going
// through the Sequencer UI.
//
// The one UMG-specific piece is binding: a movie scene possessable GUID means nothing to a
// widget on its own, so UWidgetAnimation keeps a parallel FWidgetAnimationBinding array that
// maps each GUID to a widget name (and, for slot animation, the slot's widget name). Both
// halves have to be written together or the animation silently animates nothing.
//
// Split from FWidgetCommandHandler only for file size; the method names share the widget.*
// namespace because they operate on the same asset.
class FWidgetAnimCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
