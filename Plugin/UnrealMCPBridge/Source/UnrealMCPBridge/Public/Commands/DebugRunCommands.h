#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Handles the run-and-check commands that pie.start / pie.stop alone could not cover:
//   pie.exec_console     a console command in the PIE player's context (cheats, ifsr.* CVars) or the editor's
//   pie.read_log         the editor log since a cursor, filtered — captured by the bridge from startup
//   automation.run_test  run registered automation tests (IFSR.Quest.*, ...) and return their results
class FDebugRunCommandHandler : public IMCPCommandHandler
{
public:
    virtual ~FDebugRunCommandHandler() override;
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
