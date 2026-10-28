#include "MCPCommandRegistry.h"
#include "UnrealMCPBridgeModule.h"

void FMCPCommandRegistry::Register(const FString& Method, FMCPCommandFunction Function)
{
    if (Handlers.Contains(Method))
    {
        UE_LOG(LogMCPBridge, Warning,
            TEXT("MCPCommandRegistry: duplicate registration for method '%s' — overwriting."), *Method);
    }
    Handlers.Add(Method, MoveTemp(Function));
}

bool FMCPCommandRegistry::Has(const FString& Method) const
{
    return Handlers.Contains(Method);
}

bool FMCPCommandRegistry::Dispatch(
    const FString& Method,
    const TSharedPtr<FJsonObject>& Params,
    TSharedPtr<FJsonObject>& OutResult,
    MCPProtocol::FMCPError& OutError) const
{
    const FMCPCommandFunction* Handler = Handlers.Find(Method);
    if (!Handler)
    {
        OutError.Code    = MCPProtocol::FMCPError::MethodNotFound;
        OutError.Message = FString::Printf(TEXT("Method not found: %s"), *Method);
        return false;
    }

    OutResult = (*Handler)(Params, OutError);
    if (!OutResult.IsValid())
    {
        // Handler signalled failure via OutError.
        return false;
    }
    return true;
}

TArray<FString> FMCPCommandRegistry::GetMethods() const
{
    TArray<FString> Methods;
    Handlers.GetKeys(Methods);
    Methods.Sort();
    return Methods;
}
