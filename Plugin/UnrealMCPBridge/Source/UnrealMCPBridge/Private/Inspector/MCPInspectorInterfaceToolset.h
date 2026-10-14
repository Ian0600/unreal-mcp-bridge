#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MCPInspectorInterfaceToolset.generated.h"

/**
 * Blueprint Interface authoring (missing from the official UE MCP toolsets and the editor Python API):
 * create Blueprint Interface assets, add interface functions with typed parameters,
 * implement/remove interfaces on Blueprints, list implemented interfaces.
 *
 * Pin type strings: bool, byte, int, int64, float, double, string, name, text,
 * vector, rotator, transform, linearcolor, object:<ClassPath>, class:<ClassPath>,
 * softobject:<ClassPath>, struct:<StructPath>, enum:<EnumPath>, and the prefix "array:" e.g. "array:int".
 * Parameter list format: "Name:Type,Name2:Type2" (empty string = none).
 * Changes are compiled but NOT saved; save with the official asset tools or SaveBlueprint.
 */
UCLASS(BlueprintType)
class UMCPInspectorInterfaceToolset : public UObject
{
	GENERATED_BODY()

public:

	/**
	 * Create a new Blueprint Interface asset.
	 * @param AssetPath Package path of the new asset, e.g. "/Game/Interfaces/BPI_Interactable". Fails if it exists.
	 * @return Object path of the created interface, or an "ERROR|..." string.
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Interface", meta = (AICallable))
	static FString CreateBlueprintInterface(const FString& AssetPath);

	/**
	 * Add a function to a Blueprint Interface. Functions without outputs become implementable events.
	 * @param InterfacePath Blueprint Interface asset path.
	 * @param FunctionName New function name.
	 * @param Inputs "Name:Type,..." input parameters.
	 * @param Outputs "Name:Type,..." output parameters (makes it a function instead of an event).
	 * @return "OK" or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Interface", meta = (AICallable))
	static FString AddInterfaceFunction(const FString& InterfacePath, const FString& FunctionName, const FString& Inputs, const FString& Outputs);

	/**
	 * Make a Blueprint implement an interface (Blueprint Interface asset path or native interface class path like "/Script/Engine.Interface_AssetUserData").
	 * Adds stub graphs for interface functions. Compiles the Blueprint.
	 * @return "OK" or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Interface", meta = (AICallable))
	static FString ImplementInterface(const FString& BlueprintPath, const FString& InterfacePath);

	/**
	 * Remove an implemented interface from a Blueprint.
	 * @param bPreserveFunctions Keep the implemented function graphs as normal functions.
	 * @return "OK" or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Interface", meta = (AICallable))
	static FString RemoveInterface(const FString& BlueprintPath, const FString& InterfacePath, bool bPreserveFunctions);

	/**
	 * List interfaces implemented by a Blueprint (including inherited ones, marked).
	 * @return JSON array: [{"interface":path,"inherited":bool,"graphs":[names]}]
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|Interface", meta = (AICallable))
	static FString ListImplementedInterfaces(const FString& BlueprintPath);
};
