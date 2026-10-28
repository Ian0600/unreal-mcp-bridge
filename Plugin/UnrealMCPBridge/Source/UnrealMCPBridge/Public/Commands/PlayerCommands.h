#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Drives the PIE player the way a person would, for automated play tests:
//   input.inject       inject an Enhanced Input action (press, hold for seconds, hold until released)
//   player.aim_at      turn the camera toward a point at a set speed, with an aim error
//   player.teleport    move the player, wait for World Partition cells and the ground, verify the result
//   view.is_visible    is a point / actor on the player's screen and not hidden behind something
//   player.job_status  result of an aim_at / teleport job (they take several frames)
class FPlayerCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
