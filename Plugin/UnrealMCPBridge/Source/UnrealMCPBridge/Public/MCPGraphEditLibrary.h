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

    // ---- Blueprint authoring: structure (variables / functions / components / class defaults) ----

    // Adds a member variable of the given type. TypeString: bool|byte|int|int64|float|string|name|text|
    // vector|rotator|transform|object|class|struct|enum, or a direct /Script/... object/struct/enum path.
    // For object/class/struct/enum, SubTypeObjectPath names the class/struct/enum
    // (e.g. "/Script/Engine.StaticMeshComponent"). bIsArray wraps it in an array. DefaultValue is Unreal
    // import text (may be empty). Returns true on success.
    UFUNCTION(BlueprintCallable, Category = "MCP|Blueprint", meta = (DevelopmentOnly))
    static bool AddMemberVariable(
        UBlueprint* Blueprint,
        const FString& VarName,
        const FString& TypeString,
        const FString& SubTypeObjectPath,
        bool bIsArray,
        const FString& DefaultValue);

    // Adds a new user function graph (with editable entry/result nodes). Returns the graph name, or empty.
    UFUNCTION(BlueprintCallable, Category = "MCP|Blueprint", meta = (DevelopmentOnly))
    static FString AddFunctionGraph(UBlueprint* Blueprint, const FString& FunctionName);

    // Adds a component (by class path, e.g. "/Script/Engine.StaticMeshComponent") to the Blueprint's
    // construction script. If ParentComponentName names an existing component in the SCS, the new one is
    // attached under it; otherwise it is added at the root. Returns the component's variable name, or empty.
    UFUNCTION(BlueprintCallable, Category = "MCP|Blueprint", meta = (DevelopmentOnly))
    static FString AddComponent(
        UBlueprint* Blueprint,
        const FString& ComponentClassPath,
        const FString& ComponentName,
        const FString& ParentComponentName);

    // Sets a property on the Blueprint's class-default object (Class Defaults panel) by reflection import
    // text (same mechanism as statetree.set_node_property). Returns true on success.
    UFUNCTION(BlueprintCallable, Category = "MCP|Blueprint", meta = (DevelopmentOnly))
    static bool SetClassDefaultProperty(UBlueprint* Blueprint, const FString& PropertyName, const FString& ValueText);

    // Adds an implemented interface to the Blueprint. InterfaceClassPath is a native interface
    // (e.g. "/Script/Module.UMyInterface") or a BP interface's generated class
    // (e.g. "/Game/BPI_Thing.BPI_Thing_C"). Returns true on success.
    UFUNCTION(BlueprintCallable, Category = "MCP|Blueprint", meta = (DevelopmentOnly))
    static bool ImplementInterface(UBlueprint* Blueprint, const FString& InterfaceClassPath);

    // Adds an input (bIsOutput=false) or output (bIsOutput=true) parameter to a function graph's
    // entry/result node. TypeString/SubTypeObjectPath/bIsArray as in AddMemberVariable. Works for both
    // normal function graphs and Blueprint Interface function graphs (creates a result node if an output
    // is added and none exists yet). Returns the created pin's name, or empty on failure.
    UFUNCTION(BlueprintCallable, Category = "MCP|Blueprint", meta = (DevelopmentOnly))
    static FString AddFunctionParam(
        UBlueprint* Blueprint,
        const FString& FunctionGraphName,
        const FString& ParamName,
        const FString& TypeString,
        const FString& SubTypeObjectPath,
        bool bIsArray,
        bool bIsOutput);

    // ---- Blueprint authoring: graph nodes ----

    // Spawns a K2 node of an arbitrary class (e.g. "/Script/BlueprintGraph.K2Node_CustomEvent") into the
    // named graph and allocates its default pins. For call-function / variable get / variable set prefer the
    // dedicated adders (they also set the reference). Post-spawn, set node specifics via reflection and wire
    // with ConnectPins. Returns the new node's name, or empty on failure.
    UFUNCTION(BlueprintCallable, Category = "MCP|Graph", meta = (DevelopmentOnly))
    static FString AddK2Node(
        UBlueprint* Blueprint,
        const FString& GraphName,
        const FString& NodeClassPath,
        int32 PosX,
        int32 PosY);

    // Adds a K2Node_VariableSet (self-context) for PropertyName. Mirror of AddVariableGetNode.
    UFUNCTION(BlueprintCallable, Category = "MCP|Graph", meta = (DevelopmentOnly))
    static FString AddVariableSetNode(
        UBlueprint* Blueprint,
        const FString& GraphName,
        const FString& PropertyName,
        int32 PosX,
        int32 PosY);

    // Sets a node input pin's literal default value through the graph schema (what typing into the pin does).
    // Returns true on success (false if the graph/node/pin is missing).
    UFUNCTION(BlueprintCallable, Category = "MCP|Graph", meta = (DevelopmentOnly))
    static bool SetPinDefaultValue(
        UBlueprint* Blueprint,
        const FString& GraphName,
        const FString& NodeName,
        const FString& PinName,
        const FString& Value);

    // Resolves GraphName against Blueprint's top-level graphs, then (for AnimBlueprints) falls
    // back to nested AnimGraph graphs: state machine inner graphs, state bound graphs, and
    // transition rule graphs. Public so other command handlers (e.g. blueprint.get_graph_nodes)
    // can inspect nested anim graphs too. Not a UFUNCTION — C++-only, no Python/Blueprint exposure.
    static UEdGraph* FindGraphByName(UBlueprint* BP, const FString& GraphName);

private:
    static UEdGraphNode* FindNodeByName(UEdGraph* Graph, const FString& NodeName);
    static UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName);

    // Builds an FEdGraphPinType from a type string (see AddMemberVariable). Returns false if unrecognized.
    static bool MakePinTypeFromString(const FString& TypeString, const FString& SubTypeObjectPath, bool bIsArray, FEdGraphPinType& OutPinType);

    // Finds a state (by GetStateName()) in a state machine's inner graph.
    static class UAnimStateNodeBase* FindStateByName(class UAnimGraphNode_StateMachineBase* MachineNode, const FString& StateName);
};
