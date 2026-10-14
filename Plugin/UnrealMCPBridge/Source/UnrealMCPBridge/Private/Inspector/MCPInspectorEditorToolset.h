#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MCPInspectorEditorToolset.generated.h"

/**
 * BlueprintInspector editor toolset: generic escape hatches the official UE MCP toolsets lack.
 * - Run arbitrary Python in the editor (unreal module available), e.g. to call
 *   unreal.MCPInspectorLibrary or any editor scripting API.
 * - Run editor console commands and capture their output.
 * - Fix up asset redirectors.
 * Local, personal tool. Disable with the console variable BlueprintInspector.EnablePythonTool 0.
 */
UCLASS(BlueprintType)
class UMCPInspectorEditorToolset : public UObject
{
	GENERATED_BODY()

public:


	/**
	 * Execute an editor console command (e.g. "stat fps", "obj list class=Blueprint", CVar set/get).
	 * @param Command Console command line.
	 * @return JSON: {"success":bool,"output":string} with the text the command printed.
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Editor", meta = (AICallable))
	static FString ExecuteConsoleCommand(const FString& Command);

	/**
	 * Fix up redirectors under the given content paths (re-save referencers, delete redirectors).
	 * Referencing packages are saved; check them out of source control first if required.
	 * @param ContentPaths Folders like "/Game/Characters" (recursive).
	 * @return JSON: {"found":int,"fixed":int,"remaining":[paths]}
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Editor", meta = (AICallable))
	static FString FixupRedirectors(const TArray<FString>& ContentPaths);

	// ---- BEGIN functions forwarding to unreal-mcp-bridge methods (generated from the bridge tool layer; BlueprintInspector has no dependency on that plugin) ----





















	// ---- END forwarding functions ----
};
