#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Handles skeletalmesh.* JSON-RPC methods — reading and creating attachment sockets.
//
// Python cannot do this at all: USkeletalMeshSocket exposes SocketName/BoneName/RelativeLocation as
// read-only to the scripting layer, and UObject has no import_text, so a socket created from
// editor_python_exec can never be given a name or a bone. USkeletalMesh::AddSocket also takes a
// socket object rather than a name, which leaves Python with nothing it can build.
//
// Sockets are how props attach to a character (a flashlight on the chest, a syringe in the hand):
// a bone name alone gives you the bone's own transform, and the offset that makes the prop sit
// right has to live somewhere the artist can nudge. That somewhere is the socket.
class FSkeletalMeshCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
