#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MCPProtocol.h"

class ULevelSequence;
class UMovieScene;

// The sequencer.* helpers (defined in SequencerCommands.cpp) that sequence.* reuses, so both families
// load sequences, name bindings and convert frames one way.
namespace MCPSequencer
{
    /** Params.sequence_path -> a level sequence with a movie scene (transient ones resolve too). */
    ULevelSequence* LoadSequence(const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError);

    /** Tick (internal resolution) <-> display frame (the numbers on the Sequencer timeline). */
    double TickToDisplay(const UMovieScene* MovieScene, FFrameNumber Tick);
    FFrameNumber DisplayToTick(const UMovieScene* MovieScene, double DisplayFrame);

    FString BindingName(UMovieScene* MovieScene, const FGuid& Guid);

    /** A binding GUID or name; ambiguous names fail and list the candidate GUIDs. */
    bool ResolveBinding(ULevelSequence* Sequence, const FString& Ref, FGuid& OutGuid, MCPProtocol::FMCPError& OutError);
}
