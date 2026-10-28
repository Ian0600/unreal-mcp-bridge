#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

// Central registry mapping JSON-RPC method names to command functions.
class UNREALMCPBRIDGE_API FMCPCommandRegistry
{
public:
    // Register a handler for the given method name.
    // Duplicate registration logs a warning and overwrites the previous entry.
    void Register(const FString& Method, FMCPCommandFunction Function);

    // Returns true if a handler is registered for the given method.
    bool Has(const FString& Method) const;

    // Invoke the handler for Method with Params.
    // Returns true on success and sets OutResult.
    // Returns false and sets OutError when the method is not found or the handler reports an error.
    bool Dispatch(
        const FString& Method,
        const TSharedPtr<FJsonObject>& Params,
        TSharedPtr<FJsonObject>& OutResult,
        MCPProtocol::FMCPError& OutError) const;

    // Every registered method name, sorted. editor.get_status returns it so a client can tell
    // which commands its own (possibly older) server does not wrap yet.
    TArray<FString> GetMethods() const;

private:
    TMap<FString, FMCPCommandFunction> Handlers;
};
