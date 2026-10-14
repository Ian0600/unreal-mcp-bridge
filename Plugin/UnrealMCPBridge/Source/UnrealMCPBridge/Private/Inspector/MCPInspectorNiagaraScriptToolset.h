#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MCPInspectorNiagaraScriptToolset.generated.h"

/**
 * Niagara script graph authoring that the official Niagara toolsets lack: create module / dynamic input / function
 * script assets, edit script metadata, module inputs, parameter map reads/writes, Op / function call / Custom HLSL nodes,
 * pin links and defaults, compile standalone scripts, and create scratch pad scripts owned by a system or emitter.
 * Stack-level work (adding the module to an emitter, setting stack inputs, compile state, stack issues) stays with the
 * official NiagaraToolset_System (AddModule also accepts a scratch pad script object path, see CreateScratchPadScript).
 *
 * Paths and ids:
 *  - ScriptPath = asset path ("/Game/FX/NM_MyModule") or full object path. Scratch pad scripts are sub-objects:
 *    "/Game/FX/NS_Fire.NS_Fire:MyScratch" (system) or "/Game/FX/NE_Fire.NE_Fire:<container>.MyScratch" (emitter asset);
 *    CreateScratchPadScript and DumpScriptGraph return the exact path to use.
 *  - NodeId = UEdGraphNode::NodeGuid (32 hex digits, any FGuid text form accepted). See DumpScriptGraph.
 *  - Pin = pin id (UEdGraphPin::PinId or PersistentGuid) or pin name; prefix "in:" / "out:" to force the direction,
 *    e.g. "out:Particles.Color". Names are matched exactly (FName, case-insensitive).
 *  - Type = float, int, bool, vec2, vec3, vec4, color, quat, position, matrix, map (parameter map), a registered enum or
 *    struct path ("/Script/Niagara.ENiagaraExecutionState", "/Script/Niagara.NiagaraID", "/Game/Enums/E_MyEnum"),
 *    or a data interface class path ("/Script/Niagara.NiagaraDataInterfaceCurve").
 *  - Values use the Niagara pin default format of the type: float "0.5", int "3", bool "true", vec3 "1,2,3",
 *    color "(R=1,G=0,B=0,A=1)", quat "0,0,0,1", enum entry name.
 * Edits are transacted, mark the graph as needing recompile and dirty the package like the script editor. Nothing is saved
 * (save with the official asset tools). Edit the script while its asset editor / the owning system editor is closed:
 * open editors work on a copy and will not see (or will overwrite) these changes.
 */
UCLASS(BlueprintType)
class UMCPInspectorNiagaraScriptToolset : public UObject
{
	GENERATED_BODY()

public:

	// ---- scripts ----

	/**
	 * Create a Niagara script asset exactly like the content browser (UNiagaraModule/Function/DynamicInputScriptFactory):
	 * a copy of the project's default module/function/dynamic input script, or the procedural default graph
	 * (input map -> Map Get -> Map Set -> output) when that default is missing.
	 * @param AssetPath Package path of the new asset, e.g. "/Game/FX/NM_Tint". Fails if it exists.
	 * @param Usage Module, DynamicInput or Function.
	 * @return JSON {"path","usage","nodes":n,"compileStatus"} or "ERROR|...". Not compiled unless the factory compiled it.
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|NiagaraScript", meta = (AICallable))
	static FString CreateNiagaraScript(const FString& AssetPath, const FString& Usage);

	/**
	 * Set script metadata on the latest script version (FVersionedNiagaraScriptData). Empty arguments are left unchanged;
	 * "<none>" clears a text field.
	 * @param ScriptPath Script asset or scratch pad object path.
	 * @param Description Description shown in the module picker and tooltips.
	 * @param Category Library category, e.g. "Color".
	 * @param LibraryVisibility Exposed (alias Library), Unexposed or Hidden.
	 * @param ModuleUsageBitmask Usages the module may be added to: an integer bitmask, or names separated by ',' or '|',
	 *  e.g. "ParticleSpawn,ParticleUpdate" (the "Script" suffix is optional). "System" / "Emitter" / "Particle" add the
	 *  spawn+update pair of that level.
	 * @param Keywords Space separated search keywords.
	 * @return JSON with the resulting metadata (same shape as DumpScriptGraph "metadata") or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|NiagaraScript", meta = (AICallable))
	static FString SetScriptMetadata(const FString& ScriptPath, const FString& Description, const FString& Category, const FString& LibraryVisibility, const FString& ModuleUsageBitmask, const FString& Keywords);

	/**
	 * Compile a standalone script (module / function / dynamic input, asset or scratch pad) synchronously, like the
	 * script editor's Compile button (UNiagaraScript::RequestCompile waits for the compile job).
	 * @param ScriptPath Script asset or scratch pad object path.
	 * @param bForce Recompile even if the script and graph are in sync.
	 * @param bRefreshReferencers Refresh every loaded function call node that calls this script (system/emitter stacks,
	 *  other scripts) and mark them for recompile, like Apply in the script / scratch pad editor. Needed after editing a
	 *  scratch pad script before the owning system compiles. Dirties the referencing packages.
	 * @return JSON {"status","synchronized","errors","warnings","errorMessage","events":[{"severity","message","shortDescription","nodeId","pinId"}],
	 *  "refreshedReferencers":[node paths]} or "ERROR|...". "status" is the ENiagaraScriptCompileStatus name (NCS_UpToDate on success).
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|NiagaraScript", meta = (AICallable))
	static FString CompileNiagaraScript(const FString& ScriptPath, bool bForce, bool bRefreshReferencers);

	/**
	 * Dump the script graph with ids usable by these tools.
	 * @return JSON {"path","usage","isScratchPad","owner","versionGuid",
	 *  "metadata":{"description","category","keywords","libraryVisibility","moduleUsageBitmask","moduleUsages":[...],"suggested","deprecated","experimental"},
	 *  "compile":{"status","synchronized","errorMessage","events":[...]},
	 *  "inputs":[{"name","type","defaultMode","default","description","pinIds":[...]}] (module inputs = Module.* reads),
	 *  "writes":[{"name","type","nodeId","pinId"}] (parameter map writes = Map Set inputs),
	 *  "nodes":[{"id","class","title","x","y","op"?,"functionScript"?,"functionName"?,"hlsl"?,"input"?,"outputUsage"?,"outputs"?,
	 *    "pins":[{"id","name","dir","type","default","add"?,"hidden"?,"orphaned"?,"links":[{"node","pin","pinName"}]}]}]} or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|NiagaraScript", meta = (AICallable))
	static FString DumpScriptGraph(const FString& ScriptPath);

	// ---- module inputs and parameter map pins ----

	/**
	 * Add a module input: a "Module.<Name>" read on the script's Map Get node, registered as a graph parameter
	 * (like adding it in the Parameters panel), with an optional default value (default mode Value).
	 * @param Name Input name; "Module." is prepended when no namespace is given. Must be in the Module namespace.
	 * @param Type See class comment (float, vec3, color, ...).
	 * @param DefaultValue Default in Niagara pin format (empty = type default).
	 * @param MapGetNodeId Map Get node to add the read to. Empty = the Map Get fed by the input map (first one).
	 * @return JSON {"name","type","default","nodeId","pinId"} or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|NiagaraScript", meta = (AICallable))
	static FString AddScriptInput(const FString& ScriptPath, const FString& Name, const FString& Type, const FString& DefaultValue, const FString& MapGetNodeId);

	/**
	 * Remove a module input: removes its reads from every Map Get node (links are broken) and its graph parameter.
	 * @param Name Input name, "Module." optional.
	 * @return JSON {"removedPins":n,"removedParameter":bool} or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|NiagaraScript", meta = (AICallable))
	static FString RemoveScriptInput(const FString& ScriptPath, const FString& Name);

	/**
	 * Set the default value of an existing module input (default mode becomes Value).
	 * @param Name Input name, "Module." optional.
	 * @param DefaultValue Value in Niagara pin format.
	 * @return JSON {"name","type","default"} (value read back) or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|NiagaraScript", meta = (AICallable))
	static FString SetScriptInputDefault(const FString& ScriptPath, const FString& Name, const FString& DefaultValue);

	/**
	 * Add a parameter read pin (output) to a Map Get node, e.g. an attribute "Particles.Color" or "Engine.DeltaTime".
	 * @param NodeId Map Get node id. Empty = the first Map Get node.
	 * @param ParameterName Full name including namespace, e.g. "Particles.Color".
	 * @param Type Parameter type (must match the type the parameter already has in this graph).
	 * @return JSON {"nodeId","pinId","name","type"} or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|NiagaraScript", meta = (AICallable))
	static FString AddMapGetPin(const FString& ScriptPath, const FString& NodeId, const FString& ParameterName, const FString& Type);

	/**
	 * Add a parameter write pin (input) to a Map Set node, e.g. "Particles.Color".
	 * @param NodeId Map Set node id. Empty = the first Map Set node.
	 * @param ParameterName Full name including namespace.
	 * @param Type Parameter type.
	 * @return JSON {"nodeId","pinId","name","type"} or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|NiagaraScript", meta = (AICallable))
	static FString AddMapSetPin(const FString& ScriptPath, const FString& NodeId, const FString& ParameterName, const FString& Type);

	// ---- nodes ----

	/**
	 * Add a node to the script graph (same steps as the graph context menu action).
	 * @param NodeKind Op, FunctionCall, CustomHlsl, ParameterMapGet, ParameterMapSet or Reroute.
	 * @param Arg Op: op name, e.g. "Numeric::Add", "Numeric::Mul", "Numeric::Normalize", "Numeric::Lerp", "Vector3::Cross".
	 *  FunctionCall: function or dynamic input script path. CustomHlsl: HLSL body (empty = editor placeholder). Others: ignored.
	 * @param PosX Graph X position.
	 * @param PosY Graph Y position.
	 * @return JSON node (same shape as DumpScriptGraph "nodes" entries) or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|NiagaraScript", meta = (AICallable))
	static FString AddNode(const FString& ScriptPath, const FString& NodeKind, const FString& Arg, int32 PosX, int32 PosY);

	/**
	 * Remove a node (breaks its links). Nodes the editor does not allow deleting (e.g. the output node) are refused.
	 * @return "OK" or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|NiagaraScript", meta = (AICallable))
	static FString RemoveNode(const FString& ScriptPath, const FString& NodeId);

	/**
	 * Replace the HLSL body of a Custom HLSL node. Inputs and outputs are referenced by pin name in the code,
	 * e.g. "OutColor = InColor * Tint;".
	 * @return JSON node or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|NiagaraScript", meta = (AICallable))
	static FString SetCustomHlsl(const FString& ScriptPath, const FString& NodeId, const FString& Code);

	/**
	 * Add a typed input or output pin to a Custom HLSL node (updates the node signature and rebuilds its pins; links are kept).
	 * @param Name HLSL identifier ([A-Za-z_][A-Za-z0-9_]*), unique on the node.
	 * @param Type See class comment. "map" adds a parameter map pin (to read/write the map inside the HLSL).
	 * @param bIsOutput True for an output pin.
	 * @return JSON node or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|NiagaraScript", meta = (AICallable))
	static FString AddCustomHlslPin(const FString& ScriptPath, const FString& NodeId, const FString& Name, const FString& Type, bool bIsOutput);

	/**
	 * Remove an input or output pin of a Custom HLSL node (signature updated, links of that pin broken).
	 * @return JSON node or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|NiagaraScript", meta = (AICallable))
	static FString RemoveCustomHlslPin(const FString& ScriptPath, const FString& NodeId, const FString& Pin);

	// ---- pins ----

	/**
	 * Connect two pins through the Niagara graph schema (UEdGraphSchema_Niagara::TryCreateConnection): type checks,
	 * replacing an existing input link, numeric resolution and automatic conversion nodes behave like dragging a wire.
	 * @param FromPin Output pin (id or name). @param ToPin Input pin (id or name).
	 * @return JSON {"response","message"} or "ERROR|<schema message>".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|NiagaraScript", meta = (AICallable))
	static FString ConnectPins(const FString& ScriptPath, const FString& FromNodeId, const FString& FromPin, const FString& ToNodeId, const FString& ToPin);

	/**
	 * Break links of a pin: all of them, or only the link to OtherNodeId/OtherPin when given.
	 * @return JSON {"brokenLinks":n} or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|NiagaraScript", meta = (AICallable))
	static FString DisconnectPin(const FString& ScriptPath, const FString& NodeId, const FString& Pin, const FString& OtherNodeId, const FString& OtherPin);

	/**
	 * Set the inline default of an unlinked input pin (e.g. Op node "B", function call inputs, Custom HLSL inputs).
	 * Module input defaults on Map Get nodes are read-only: use SetScriptInputDefault.
	 * @param ValueText Value in Niagara pin format of the pin type.
	 * @return JSON {"pin","default"} (value read back) or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|NiagaraScript", meta = (AICallable))
	static FString SetPinDefault(const FString& ScriptPath, const FString& NodeId, const FString& Pin, const FString& ValueText);

	// ---- scratch pad ----

	/**
	 * Create a scratch pad script the way the scratch pad panel does ("+ Module" / "+ Dynamic Input"): a copy of the default
	 * module / dynamic input script owned by the system (UNiagaraSystem::ScratchPadScripts, shared by all its emitters) or,
	 * for an emitter asset, by the emitter's scratch pad container. Edit it with the other tools using the returned path,
	 * compile it with CompileNiagaraScript(bRefreshReferencers=true), and add it to a stack with the official
	 * NiagaraToolset_System.AddModule (ModuleAsset = the returned path; no separate tool is needed).
	 * @param OwnerPath Niagara system asset path (system scratch pad) or Niagara emitter asset path.
	 * @param Name Script name (made unique in the owner). Empty = "ScratchModule" / "ScratchDynamicInput".
	 * @param Usage Module or DynamicInput.
	 * @param TargetStackUsage Stack the module is meant for, added to its usage bitmask like the editor does, e.g.
	 *  "ParticleUpdate", "ParticleSpawn", "EmitterUpdate", "SystemUpdate". Empty = keep the default bitmask.
	 * @return JSON {"path","name","usage","owner","moduleUsages":[...]} or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|NiagaraScript", meta = (AICallable))
	static FString CreateScratchPadScript(const FString& OwnerPath, const FString& Name, const FString& Usage, const FString& TargetStackUsage);

	/**
	 * List the scratch pad scripts of a system (or emitter asset) with their object paths.
	 * @param OwnerPath Niagara system or emitter asset path.
	 * @return JSON {"owner","scripts":[{"path","name","usage","moduleUsages":[...],"compileStatus"}]} or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|NiagaraScript", meta = (AICallable))
	static FString ListScratchPadScripts(const FString& OwnerPath);
};
