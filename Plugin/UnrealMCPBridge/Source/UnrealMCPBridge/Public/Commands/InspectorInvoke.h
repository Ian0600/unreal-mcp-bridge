#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

/**
 * Builds a command that forwards to a BlueprintInspector function (a static UFUNCTION on one of its
 * toolset classes) through UObject reflection.
 *
 * The functions live in this module (vendored from BlueprintInspector, Private/Inspector); the bridge builds
 * and loads without it, and the forwarded commands answer with an InternalError until the plugin
 * is present. The transport is untouched — these are ordinary registry commands.
 *
 * @param ClassPath    e.g. "/Script/UnrealMCPBridge.MCPInspectorLibrary"
 * @param FunctionName the UFUNCTION's C++ name
 * @param ParamKeys    JSON keys of the function's parameters, in declaration order (return value excluded)
 *
 * The result is {"value": <return value>}; a string return that holds a JSON object or array is
 * returned parsed, other strings as-is. Parameters missing from the request keep their zero value.
 */
UNREALMCPBRIDGE_API FMCPCommandFunction MakeInspectorCommand(const TCHAR* ClassPath, const TCHAR* FunctionName, TArray<FString> ParamKeys);
