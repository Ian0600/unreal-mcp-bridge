#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Handles datatable.* JSON-RPC methods: read rows (datatable.get_rows) and re-read a table from
// its JSON source (datatable.reimport, with a dry-run row diff). There is deliberately no row
// write: tables are produced by Excel -> DataGen -> JSON, and a hand-written row would be
// overwritten by the next DataGen run.
//
// The reimport fills the table with UDataTable::CreateTableFromJSONString directly. The
// scripting route (DataTableFunctionLibrary::FillDataTableFromJSON*) goes through
// UCSVImportFactory, which pops a modal "missing entry" list for every row without a default
// column and blocks the game thread — the bridge call never returns.
class FDataTableCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
