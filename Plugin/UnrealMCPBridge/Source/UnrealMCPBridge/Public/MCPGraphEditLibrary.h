#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MCPGraphEditLibrary.generated.h"

class UBlueprint;
class UAnimBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

UCLASS()
class UNREALMCPBRIDGE_API UMCPGraphEditLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    // Adds a K2Node_CallFunction node to the named graph that calls
    // FunctionClassPath::FunctionName. Returns the new node's name, or empty
    // string on failure.
    UFUNCTION(BlueprintCallable, Category = "MCP|Graph", meta = (DevelopmentOnly))
    static FString AddCallFunctionNode(
        UBlueprint* Blueprint,
        const FString& GraphName,
        const FString& FunctionClassPath,
        const FString& FunctionName,
        int32 PosX,
        int32 PosY);

    // Adds a K2Node_IfThenElse (Branch) node. Returns the new node's name,
    // or empty string on failure.
    UFUNCTION(BlueprintCallable, Category = "MCP|Graph", meta = (DevelopmentOnly))
    static FString AddIfThenElseNode(
        UBlueprint* Blueprint,
        const FString& GraphName,
        int32 PosX,
        int32 PosY);

    // Adds a K2Node_VariableGet (self-context) for PropertyName, a property on the Blueprint's
    // own generated/parent class. Works in AnimGraph nested graphs too (GraphName is resolved
    // the same way as the other Add*Node functions). Returns the new node's name, or empty on failure.
    UFUNCTION(BlueprintCallable, Category = "MCP|Graph", meta = (DevelopmentOnly))
    static FString AddVariableGetNode(
        UBlueprint* Blueprint,
        const FString& GraphName,
        const FString& PropertyName,
        int32 PosX,
        int32 PosY);

    // Connects two pins. Returns true on success.
    // Links SrcNode.SrcPin -> DstNode.DstPin through the graph's schema (what an editor drag does), so
    // the pair is validated, both nodes are notified, and any needed conversion node is inserted.
    // Returns false (and logs the schema's reason) if the connection is refused.
    UFUNCTION(BlueprintCallable, Category = "MCP|Graph", meta = (DevelopmentOnly))
    static bool ConnectPins(
        UBlueprint* Blueprint,
        const FString& GraphName,
        const FString& SrcNodeName,
        const FString& SrcPinName,
        const FString& DstNodeName,
        const FString& DstPinName);

    // Disconnects a specific link between two pins. Returns true on success.
    UFUNCTION(BlueprintCallable, Category = "MCP|Graph", meta = (DevelopmentOnly))
    static bool DisconnectPinLink(
        UBlueprint* Blueprint,
        const FString& GraphName,
        const FString& NodeA,
        const FString& PinA,
        const FString& NodeB,
        const FString& PinB);

    // Compiles the Blueprint and marks the package dirty.
    UFUNCTION(BlueprintCallable, Category = "MCP|Graph", meta = (DevelopmentOnly))
    static bool CompileBlueprint(UBlueprint* Blueprint);

    // Removes a node from the named graph. Returns true on success.
    UFUNCTION(BlueprintCallable, Category = "MCP|Graph", meta = (DevelopmentOnly))
    static bool RemoveNode(
        UBlueprint* Blueprint,
        const FString& GraphName,
        const FString& NodeName);

    // Finds and removes a UEdGraph subobject of Blueprint by name that isn't reachable from any
    // live node (e.g. an inner state-machine graph orphaned by a node deleted before this class's
    // RemoveNode fix started calling DestroyNode()). Returns true if a matching orphan was found
    // and removed.
    UFUNCTION(BlueprintCallable, Category = "MCP|Graph", meta = (DevelopmentOnly))
    static bool RemoveOrphanedGraph(
        UBlueprint* Blueprint,
        const FString& GraphName);

    // Implements a Blueprint interface on the Blueprint (equivalent to Class Settings > Interfaces > Add).
    // InterfaceClassPath is the interface's generated class path — a BP interface asset's class,
    // e.g. "/Game/.../BPI_Foo.BPI_Foo_C", or a native "/Script/Module.UMyInterface". For interface
    // functions that have return values (or output params), this creates their function graphs
    // (Entry + Result nodes) which you then populate with nodes + ConnectPins. Returns true on success.
    UFUNCTION(BlueprintCallable, Category = "MCP|Graph", meta = (DevelopmentOnly))
    static bool ImplementInterface(UBlueprint* Blueprint, const FString& InterfaceClassPath);

    // Adds a K2Node_MakeStruct for the struct at StructPath (e.g. a UserDefinedStruct
    // "/Game/.../S_Foo.S_Foo", or a native "/Script/CoreUObject.Vector"). Each struct member
    // becomes an input pin; for UserDefinedStructs those pin names carry a GUID suffix
    // (e.g. "Velocity_12_ABC...") — fetch the real names via blueprint.get_graph_nodes before
    // ConnectPins. The struct output pin is named after the struct. Returns the new node's name,
    // or empty string on failure.
    UFUNCTION(BlueprintCallable, Category = "MCP|Graph", meta = (DevelopmentOnly))
    static FString AddMakeStructNode(
        UBlueprint* Blueprint,
        const FString& GraphName,
        const FString& StructPath,
        int32 PosX,
        int32 PosY);

    // Adds a K2Node_Select (the data multiplexer: pick one of N option values by an Index). Starts as
    // a wildcard 2-option select; wiring settles its types the way the editor does on a drag — connect a
    // bool/int/enum to its "Index" pin and connect the "Return Value" output to a typed pin, then the
    // option pins take that type. For a bool index the two options are "Option 0" (false) / "Option 1"
    // (true); set their literals with SetPinDefaultValue. Returns the new node's name, or empty on failure.
    UFUNCTION(BlueprintCallable, Category = "MCP|Graph", meta = (DevelopmentOnly))
    static FString AddSelectNode(
        UBlueprint* Blueprint,
        const FString& GraphName,
        int32 PosX,
        int32 PosY);

    // Sets an (unconnected) input pin's literal default value, through the graph schema so enum/struct/
    // object literals are validated and the node is notified (what typing into the pin in-editor does).
    // Value is the pin's serialized form: for a UserDefinedEnum pin that is the raw enumerator name
    // ("NewEnumerator0", "NewEnumerator1", ...), for numbers the number, for bools "true"/"false".
    // Returns false if the graph/node/pin is not found or the schema rejects the value.
    UFUNCTION(BlueprintCallable, Category = "MCP|Graph", meta = (DevelopmentOnly))
    static bool SetPinDefaultValue(
        UBlueprint* Blueprint,
        const FString& GraphName,
        const FString& NodeName,
        const FString& PinName,
        const FString& Value);

    // Sets an UObject-typed UPROPERTY directly on a graph node's C++ object, bypassing the script-access
    // protection that blocks set_editor_property (some K2 nodes keep essential refs in a plain, non-Edit
    // UPROPERTY — e.g. K2Node_SetCameraRigParameter::CameraRig). PropertyName is the C++ property name;
    // ObjectPath is a package path to load (empty string clears it to null). After the write the node is
    // reconstructed so its title/pins refresh the way the editor does. Returns false if the graph/node/
    // property is not found, the property is not object-typed, or the object fails to load.
    UFUNCTION(BlueprintCallable, Category = "MCP|Graph", meta = (DevelopmentOnly))
    static bool SetNodeObjectProperty(
        UBlueprint* Blueprint,
        const FString& GraphName,
        const FString& NodeName,
        const FString& PropertyName,
        const FString& ObjectPath);

    // ---- AnimGraph (AnimBlueprint-only) ----

    // Adds a State Machine node to the named AnimGraph (an AnimBlueprint's main graph is a
    // FunctionGraph named "AnimGraph", so the existing graph lookup finds it). Auto-creates the
    // machine's inner EditorStateMachineGraph + Entry node. Returns the new node's name, or empty on failure.
    UFUNCTION(BlueprintCallable, Category = "MCP|AnimGraph", meta = (DevelopmentOnly))
    static FString AddAnimStateMachineNode(
        UAnimBlueprint* AnimBlueprint,
        const FString& GraphName,
        int32 PosX,
        int32 PosY);

    // Adds a state to a state machine's inner graph. Auto-creates the state's BoundGraph
    // (with its Output Animation Pose sink). If this is the machine's first state, the Entry
    // node is auto-wired to it. Returns the new state node's name, or empty on failure.
    UFUNCTION(BlueprintCallable, Category = "MCP|AnimGraph", meta = (DevelopmentOnly))
    static FString AddAnimState(
        UAnimBlueprint* AnimBlueprint,
        const FString& GraphName,
        const FString& StateMachineNodeName,
        const FString& StateName,
        int32 PosX,
        int32 PosY);

    // Adds a BlendSpacePlayer node inside StateName's bound graph, sets its BlendSpace asset,
    // and wires its pose output to the state's Output Animation Pose sink. The blend space's
    // axis input pins (typically named X/Y) are left for ConnectPins. Returns the new node's
    // name, or empty on failure.
    UFUNCTION(BlueprintCallable, Category = "MCP|AnimGraph", meta = (DevelopmentOnly))
    static FString AddBlendSpacePlayerToState(
        UAnimBlueprint* AnimBlueprint,
        const FString& GraphName,
        const FString& StateMachineNodeName,
        const FString& StateName,
        const FString& BlendSpaceAssetPath,
        int32 PosX,
        int32 PosY);

    // Rebuilds a blend space's interpolation data (triangulation / grid) from its current SampleData.
    //
    // Needed because UBlendSpace::PostEditChangeProperty does NOT resample: editing SampleData only
    // empties GridSamples, and the rebuild lives in the blend space *editor UI*. So a blend space
    // authored from script (set_editor_property on BlendParameters/SampleData) has samples but no
    // interpolation data, and evaluates to the reference pose — the animation silently never plays.
    // Call this once after writing the samples.
    UFUNCTION(BlueprintCallable, Category = "MCP|Anim", meta = (DevelopmentOnly))
    static bool RebuildBlendSpace(UBlendSpace* BlendSpace);

    // Adds a plain (non-blend-space) SequencePlayer node inside StateName's bound graph, sets its
    // Sequence asset and loop flag, and wires its pose output to the state's Output Animation Pose
    // sink. Use for one-shot transition animations (Start/Stop clips) inside a dedicated state.
    // Returns the new node's name, or empty on failure.
    UFUNCTION(BlueprintCallable, Category = "MCP|AnimGraph", meta = (DevelopmentOnly))
    static FString AddSequencePlayerToState(
        UAnimBlueprint* AnimBlueprint,
        const FString& GraphName,
        const FString& StateMachineNodeName,
        const FString& StateName,
        const FString& SequenceAssetPath,
        bool bLoop,
        int32 PosX,
        int32 PosY);

    // Adds an anim graph node of an arbitrary class to the named graph — the escape hatch for the
    // node types this library has no dedicated Add* for (ModifyBone, TwoBoneIK, LookAt, ...).
    // NodeClassPath is a class path, e.g. "/Script/AnimGraph.AnimGraphNode_ModifyBone".
    //
    // The node's own settings are left at their class defaults: fetch the returned node by object path
    // (`<graph path>.<returned name>`) and set them via the reflection system, then wire it with
    // ConnectPins. Returns the new node's name, or empty on failure.
    UFUNCTION(BlueprintCallable, Category = "MCP|AnimGraph", meta = (DevelopmentOnly))
    static FString AddAnimGraphNode(
        UAnimBlueprint* AnimBlueprint,
        const FString& GraphName,
        const FString& NodeClassPath,
        int32 PosX,
        int32 PosY);

    // Points a Use Cached Pose node at a Save Cached Pose node. Needed because a pose output pin takes
    // only ONE link — feeding the same pose to two places (e.g. a layered blend's base *and* a slot)
    // means caching it. The link is a plain UPROPERTY with no editor exposure, so it cannot be set from
    // Python; hence this. Returns false if either node is missing or of the wrong type.
    UFUNCTION(BlueprintCallable, Category = "MCP|AnimGraph", meta = (DevelopmentOnly))
    static bool LinkUseCachedPose(
        UAnimBlueprint* AnimBlueprint,
        const FString& GraphName,
        const FString& UseNodeName,
        const FString& SaveNodeName);

    // Adds a Slot node (montage layering point) to the named top-level AnimGraph. SlotName must
    // match a slot defined on the Skeleton (e.g. "UpperBody"); a montage authored on that slot only
    // plays visibly once this node sits in series between the pose source and the output. Input pin
    // "Source", output pin "Pose" (same convention as the other Add*ToState nodes) — wire both with
    // ConnectPins/DisconnectPinLink. Returns the new node's name, or empty on failure.
    UFUNCTION(BlueprintCallable, Category = "MCP|AnimGraph", meta = (DevelopmentOnly))
    static FString AddAnimSlotNode(
        UAnimBlueprint* AnimBlueprint,
        const FString& GraphName,
        const FString& SlotName,
        int32 PosX,
        int32 PosY);

    // Adds a transition between two states in a state machine. If ConditionPropertyName is
    // non-empty, the transition's rule graph is wired to `Get <ConditionPropertyName>` (a bool
    // property on the AnimBlueprint's parent class, e.g. "bIsInAir") -> Result, optionally negated.
    // Returns the new transition node's name, or empty on failure.
    UFUNCTION(BlueprintCallable, Category = "MCP|AnimGraph", meta = (DevelopmentOnly))
    static FString AddAnimTransition(
        UAnimBlueprint* AnimBlueprint,
        const FString& GraphName,
        const FString& StateMachineNodeName,
        const FString& FromStateName,
        const FString& ToStateName,
        const FString& ConditionPropertyName,
        bool bNegateCondition);

    // ---- BlendSpace (asset editing) ----

    // Repositions an existing sample in a BlendSpace/BlendSpace1D asset. Unlike editing the
    // SampleData property directly (which the blend space's internal grid cache silently discards
    // on save), this goes through UBlendSpace::EditSampleValue so the grid is properly rebuilt.
    // SampleIndex is the sample's position in GetSampleData() (0-based, in authoring order).
    UFUNCTION(BlueprintCallable, Category = "MCP|Asset", meta = (DevelopmentOnly))
    static bool EditBlendSpaceSampleValue(
        class UBlendSpace* BlendSpace,
        int32 SampleIndex,
        float X,
        float Y);

    // Sets an axis's Min/Max range on a BlendSpace. BlendParameters is a protected fixed-size C
    // array (not reachable through the generic Python/Blueprint property accessors), so this
    // reaches it via FProperty reflection instead. AxisIndex: 0=X, 1=Y, 2=Z.
    UFUNCTION(BlueprintCallable, Category = "MCP|Asset", meta = (DevelopmentOnly))
    static bool SetBlendSpaceAxisRange(
        class UBlendSpace* BlendSpace,
        int32 AxisIndex,
        float Min,
        float Max);

    // Exposes a camera node property on a UCameraRigAsset as a blendable interface parameter —
    // the scriptable equivalent of "expose" on a property in the camera rig editor.
    //
    // Why this is not reachable from plain Python: the parameter list lives in
    // UBaseCameraObject::Interface, which has no editor-property access, so
    // get/set_editor_property refuses it. Only a name in that list can be driven by a
    // K2Node_SetCameraRigParameter; anything else throws "No such camera parameter: X" at
    // runtime and the write is silently dropped.
    //
    // NodeClass is a short class name ("BoomArmCameraNode"), NodeIndex picks among multiple
    // matches. The variable type is inferred from the property's F<Type>CameraParameter struct,
    // so a caller cannot pair a parameter with the wrong type. Rebuilds the rig on success —
    // the runtime's parameter definitions are produced by the build, not by the edit.
    // Returns empty string on success, or a human-readable reason on failure.
    UFUNCTION(BlueprintCallable, Category = "MCP|Camera", meta = (DevelopmentOnly))
    static FString ExposeCameraRigParameter(
        class UCameraRigAsset* Rig,
        const FString& ParameterName,
        const FString& NodeClass,
        const FString& PropertyName,
        int32 NodeIndex = 0);

    // Lists a rig's exposed blendable interface parameters as
    // "Name|Type|TargetNode.TargetProperty" entries. The counterpart read for
    // ExposeCameraRigParameter, and the way to check what a K2Node_SetCameraRigParameter may bind to.
    UFUNCTION(BlueprintCallable, Category = "MCP|Camera", meta = (DevelopmentOnly))
    static TArray<FString> ListCameraRigParameters(class UCameraRigAsset* Rig);

    // Resolves GraphName against Blueprint's top-level graphs, then (for AnimBlueprints) falls
    // back to nested AnimGraph graphs: state machine inner graphs, state bound graphs, and
    // transition rule graphs. Public so other command handlers (e.g. blueprint.get_graph_nodes)
    // can inspect nested anim graphs too. Not a UFUNCTION — C++-only, no Python/Blueprint exposure.
    static UEdGraph* FindGraphByName(UBlueprint* BP, const FString& GraphName);

private:
    static UEdGraphNode* FindNodeByName(UEdGraph* Graph, const FString& NodeName);
    static UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName);

    // Finds a state (by GetStateName()) in a state machine's inner graph.
    static class UAnimStateNodeBase* FindStateByName(class UAnimGraphNode_StateMachineBase* MachineNode, const FString& StateName);
};
