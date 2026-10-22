#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Handles niagara.* JSON-RPC methods — programmatic authoring of Niagara system module
// stacks (list/add/remove/enable modules, read and set module inputs, compile).
//
// Works through the Niagara stack view models (UNiagaraStackScriptItemGroup /
// UNiagaraStackModuleItem / UNiagaraStackFunctionInput) rather than raw graph edits:
// those classes are DLL-exported in full, whereas most of FNiagaraStackGraphUtilities is
// not, and they handle override-pin bookkeeping, rapid-iteration parameters and graph
// relayout that hand-rolled graph edits would have to reproduce.
//
// The view model is built with bIsForDataProcessingOnly=true (the same mode Epic's own
// audit commandlet and version-upgrade path use), so no preview world, sequencer or undo
// registration is spun up. Mirrors the StateTree command handler pattern.
class FNiagaraCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
