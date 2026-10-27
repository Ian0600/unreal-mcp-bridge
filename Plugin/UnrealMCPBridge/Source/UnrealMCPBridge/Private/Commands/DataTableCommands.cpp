#include "Commands/DataTableCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "Engine/DataTable.h"
#include "DataTableUtils.h"
#include "EditorFramework/AssetImportData.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace MCPDataTable
{
    using UnrealMCPBridge::Json::RequireString;

    void Fail(MCPProtocol::FMCPError& OutError, const FString& Message)
    {
        OutError.Code = MCPProtocol::FMCPError::InvalidParams;
        OutError.Message = Message;
    }

    UDataTable* LoadTable(const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        FString Path;
        if (!RequireString(Params, TEXT("table"), Path, OutError))
        {
            return nullptr;
        }
        // Accept "/Game/04_Data/DT/DT_Item" as well as the full object path.
        if (!Path.Contains(TEXT(".")))
        {
            Path += TEXT(".") + FPackageName::GetShortName(Path);
        }
        UDataTable* Table = LoadObject<UDataTable>(nullptr, *Path);
        if (!Table)
        {
            Fail(OutError, FString::Printf(TEXT("Could not load data table %s"), *Path));
            return nullptr;
        }
        if (!Table->GetRowStruct())
        {
            Fail(OutError, FString::Printf(TEXT("%s has no row struct (its C++ type may not be built)"), *Path));
            return nullptr;
        }
        return Table;
    }

    /** The JSON file the table was imported from, as recorded in its import data. */
    FString SourceFile(const UDataTable* Table)
    {
        return Table->AssetImportData ? Table->AssetImportData->GetFirstFilename() : FString();
    }

    /** Columns to report: all properties of the row struct, or the named subset (export names or property names). */
    TArray<const FProperty*> SelectColumns(const UScriptStruct* RowStruct, const TArray<FString>& Wanted, TArray<FString>& OutUnknown)
    {
        TArray<const FProperty*> Columns;
        for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
        {
            const FString ExportName = DataTableUtils::GetPropertyExportName(*It);
            if (Wanted.Num() == 0 || Wanted.ContainsByPredicate([&](const FString& W) { return W.Equals(ExportName, ESearchCase::IgnoreCase) || W.Equals(It->GetName(), ESearchCase::IgnoreCase); }))
            {
                Columns.Add(*It);
            }
        }
        for (const FString& W : Wanted)
        {
            const bool bFound = Columns.ContainsByPredicate([&](const FProperty* P) { return W.Equals(DataTableUtils::GetPropertyExportName(P), ESearchCase::IgnoreCase) || W.Equals(P->GetName(), ESearchCase::IgnoreCase); });
            if (!bFound)
            {
                OutUnknown.Add(W);
            }
        }
        return Columns;
    }

    TSharedPtr<FJsonObject> RowToJson(const TArray<const FProperty*>& Columns, const uint8* RowData)
    {
        auto Values = MakeShared<FJsonObject>();
        for (const FProperty* Column : Columns)
        {
            Values->SetStringField(DataTableUtils::GetPropertyExportName(Column),
                DataTableUtils::GetPropertyValueAsString(Column, RowData, EDataTableExportFlags::None));
        }
        return Values;
    }

    TArray<FString> ReadStringArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field)
    {
        TArray<FString> Out;
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (Params->TryGetArrayField(Field, Values) && Values)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Values)
            {
                Out.Add(Value->AsString());
            }
        }
        return Out;
    }
}

void FDataTableCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    using namespace MCPDataTable;

    // datatable.get_rows — read-only. Params: table (path, required), rows ([row names]; default
    // all), columns ([column names]; default all), max (default 500).
    // Values are the data table's own export text (what the JSON source / CSV export would hold).
    // Returns { table, row_struct, source_file, row_count, columns: [{name, type}], rows: [{name,
    // values: {column: text}}], missing_rows, unknown_columns, truncated }.
    Registry.Register(TEXT("datatable.get_rows"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UDataTable* Table = LoadTable(Params, OutError);
            if (!Table) return nullptr;
            const UScriptStruct* RowStruct = Table->GetRowStruct();

            TArray<FString> UnknownColumns;
            const TArray<const FProperty*> Columns = SelectColumns(RowStruct, ReadStringArray(Params, TEXT("columns")), UnknownColumns);
            const TArray<FString> WantedRows = ReadStringArray(Params, TEXT("rows"));
            double MaxRows = 500.0;
            Params->TryGetNumberField(TEXT("max"), MaxRows);
            const int32 Max = FMath::Max(1, static_cast<int32>(MaxRows));

            TArray<TSharedPtr<FJsonValue>> Rows;
            TArray<FString> MissingRows;
            int32 Matched = 0;
            auto AddRow = [&](const FName& RowName, const uint8* RowData)
            {
                ++Matched;
                if (Rows.Num() >= Max) return;
                auto Row = MakeShared<FJsonObject>();
                Row->SetStringField(TEXT("name"), RowName.ToString());
                Row->SetObjectField(TEXT("values"), RowToJson(Columns, RowData));
                Rows.Add(MakeShared<FJsonValueObject>(Row));
            };

            if (WantedRows.Num() > 0)
            {
                for (const FString& RowName : WantedRows)
                {
                    if (const uint8* RowData = Table->FindRowUnchecked(FName(*RowName)))
                    {
                        AddRow(FName(*RowName), RowData);
                    }
                    else
                    {
                        MissingRows.Add(RowName);
                    }
                }
            }
            else
            {
                for (const TPair<FName, uint8*>& Pair : Table->GetRowMap())
                {
                    AddRow(Pair.Key, Pair.Value);
                }
            }

            TArray<TSharedPtr<FJsonValue>> ColumnInfo;
            for (const FProperty* Column : Columns)
            {
                auto Info = MakeShared<FJsonObject>();
                Info->SetStringField(TEXT("name"), DataTableUtils::GetPropertyExportName(Column));
                Info->SetStringField(TEXT("type"), Column->GetCPPType());
                ColumnInfo.Add(MakeShared<FJsonValueObject>(Info));
            }

            auto ToStrings = [](const TArray<FString>& Strings)
            {
                TArray<TSharedPtr<FJsonValue>> Values;
                for (const FString& S : Strings) Values.Add(MakeShared<FJsonValueString>(S));
                return Values;
            };

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("table"), Table->GetPathName());
            Result->SetStringField(TEXT("row_struct"), RowStruct->GetName());
            Result->SetStringField(TEXT("source_file"), SourceFile(Table));
            Result->SetNumberField(TEXT("row_count"), Table->GetRowMap().Num());
            Result->SetArrayField(TEXT("columns"), ColumnInfo);
            Result->SetArrayField(TEXT("rows"), Rows);
            Result->SetArrayField(TEXT("missing_rows"), ToStrings(MissingRows));
            Result->SetArrayField(TEXT("unknown_columns"), ToStrings(UnknownColumns));
            Result->SetBoolField(TEXT("truncated"), Matched > Max);
            return Result;
        });

    // datatable.reimport — re-reads the table from its JSON source (Content/.TableSource/*.json,
    // as recorded in the table's import data) or from json_path. Params: table (required),
    // json_path (optional override), dry_run (default true — report only).
    // The JSON is parsed into a scratch table first; a file that yields no rows is refused, so a
    // broken file cannot empty the real table. Marks the asset dirty when applied; never saves
    // (asset.save after approval).
    // Returns { table, source_file, dry_run, applied, added, removed, changed: [{row, columns:
    // [{column, old, new}]}], unchanged_count, problems }.
    Registry.Register(TEXT("datatable.reimport"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UDataTable* Table = LoadTable(Params, OutError);
            if (!Table) return nullptr;
            UScriptStruct* RowStruct = const_cast<UScriptStruct*>(Table->GetRowStruct());

            FString JsonPath;
            Params->TryGetStringField(TEXT("json_path"), JsonPath);
            if (JsonPath.IsEmpty())
            {
                JsonPath = SourceFile(Table);
            }
            else if (FPaths::IsRelative(JsonPath))
            {
                JsonPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / JsonPath);
            }
            if (JsonPath.IsEmpty() || !FPaths::GetExtension(JsonPath).Equals(TEXT("json"), ESearchCase::IgnoreCase))
            {
                Fail(OutError, FString::Printf(TEXT("No JSON source for %s (import data says '%s') — pass json_path"), *Table->GetName(), *JsonPath));
                return nullptr;
            }
            FString Json;
            if (!FFileHelper::LoadFileToString(Json, *JsonPath))
            {
                Fail(OutError, FString::Printf(TEXT("Could not read %s"), *JsonPath));
                return nullptr;
            }

            bool bDryRun = true;
            Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

            // Parse into a scratch table with the same row struct; nothing touches the real one yet.
            TStrongObjectPtr<UDataTable> Scratch(NewObject<UDataTable>(GetTransientPackage(), NAME_None, RF_Transient));
            Scratch->RowStruct = RowStruct;
            const TArray<FString> Problems = Scratch->CreateTableFromJSONString(Json);

            TArray<FString> Added, Removed;
            TArray<TSharedPtr<FJsonValue>> Changed;
            int32 Unchanged = 0;
            for (const TPair<FName, uint8*>& Pair : Scratch->GetRowMap())
            {
                const uint8* OldRow = Table->FindRowUnchecked(Pair.Key);
                if (!OldRow)
                {
                    Added.Add(Pair.Key.ToString());
                    continue;
                }
                if (RowStruct->CompareScriptStruct(OldRow, Pair.Value, PPF_None))
                {
                    ++Unchanged;
                    continue;
                }
                TArray<TSharedPtr<FJsonValue>> Columns;
                for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
                {
                    if (!It->Identical_InContainer(OldRow, Pair.Value))
                    {
                        auto Column = MakeShared<FJsonObject>();
                        Column->SetStringField(TEXT("column"), DataTableUtils::GetPropertyExportName(*It));
                        Column->SetStringField(TEXT("old"), DataTableUtils::GetPropertyValueAsString(*It, OldRow, EDataTableExportFlags::None));
                        Column->SetStringField(TEXT("new"), DataTableUtils::GetPropertyValueAsString(*It, Pair.Value, EDataTableExportFlags::None));
                        Columns.Add(MakeShared<FJsonValueObject>(Column));
                    }
                }
                auto Row = MakeShared<FJsonObject>();
                Row->SetStringField(TEXT("row"), Pair.Key.ToString());
                Row->SetArrayField(TEXT("columns"), Columns);
                Changed.Add(MakeShared<FJsonValueObject>(Row));
            }
            for (const TPair<FName, uint8*>& Pair : Table->GetRowMap())
            {
                if (!Scratch->FindRowUnchecked(Pair.Key))
                {
                    Removed.Add(Pair.Key.ToString());
                }
            }

            auto ToStrings = [](const TArray<FString>& Strings)
            {
                TArray<TSharedPtr<FJsonValue>> Values;
                for (const FString& S : Strings) Values.Add(MakeShared<FJsonValueString>(S));
                return Values;
            };

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("table"), Table->GetPathName());
            Result->SetStringField(TEXT("source_file"), JsonPath);
            Result->SetBoolField(TEXT("dry_run"), bDryRun);
            Result->SetArrayField(TEXT("added"), ToStrings(Added));
            Result->SetArrayField(TEXT("removed"), ToStrings(Removed));
            Result->SetArrayField(TEXT("changed"), Changed);
            Result->SetNumberField(TEXT("unchanged_count"), Unchanged);
            Result->SetArrayField(TEXT("problems"), ToStrings(Problems));

            const bool bNoDifference = Added.Num() == 0 && Removed.Num() == 0 && Changed.Num() == 0;
            if (bDryRun || bNoDifference)
            {
                Result->SetBoolField(TEXT("applied"), false);
                return Result;
            }
            if (Scratch->GetRowMap().Num() == 0)
            {
                Fail(OutError, FString::Printf(TEXT("%s produced no rows — refusing to empty %s. Problems: %s"),
                    *JsonPath, *Table->GetName(), *FString::Join(Problems, TEXT(" | "))));
                return nullptr;
            }

            Table->Modify();
            Table->CreateTableFromJSONString(Json);
            if (Table->AssetImportData)
            {
                // Records the file's new timestamp/hash so the editor stops reporting the source as changed.
                Table->AssetImportData->Update(JsonPath);
            }
            Table->PostEditChange();
            Table->MarkPackageDirty();

            Result->SetBoolField(TEXT("applied"), true);
            Result->SetBoolField(TEXT("asset_dirty"), Table->GetPackage()->IsDirty());
            return Result;
        });
}
