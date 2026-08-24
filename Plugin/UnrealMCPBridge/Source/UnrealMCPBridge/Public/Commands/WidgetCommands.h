#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Handles widget.* JSON-RPC methods — programmatic authoring of Widget Blueprints (UMG):
// the widget tree, widget and slot properties, named slots, property bindings, event graph
// bindings, UI components and clipboard transfer.
//
// Built on FWidgetBlueprintOperationUtils, which UE 5.6 introduced precisely for headless
// widget blueprint manipulation: every entry point takes a UWidgetBlueprint* rather than a
// live FWidgetBlueprintEditor toolkit, and each one handles the bookkeeping that hand-rolled
// tree edits would have to reproduce (variable GUID maps, desired-focus fixups, orphaned
// graph references, named slot host wiring, transaction scoping).
//
// Where that class has no equivalent, the fallbacks are the UWidgetBlueprint*-taking
// overloads on FWidgetBlueprintEditorUtils (delete, clipboard text, named slot hosts) and
// finally the runtime UMG API (UWidgetTree, UPanelWidget, UPanelSlot).
//
// Widget and slot classes are addressed by class path and edited through reflection rather
// than getting one command per type: UMG ships 46 widget classes and 18 slot classes, and a
// per-class command surface would be both enormous and stale the moment a project adds its
// own UUserWidget subclass.
class FWidgetCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
