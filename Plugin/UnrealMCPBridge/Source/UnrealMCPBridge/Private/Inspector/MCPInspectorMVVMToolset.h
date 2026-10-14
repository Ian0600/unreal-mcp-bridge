#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MCPInspectorMVVMToolset.generated.h"

/**
 * UMG Viewmodel (MVVM) authoring that the official MVVM toolset lacks: binding mode / execution mode / enabled flags,
 * editing binding paths and conversion-function arguments, viewmodel removal / creation type / options / rename,
 * viewmodel Blueprint property removal and defaults, view events and conditions, and a full JSON dump.
 * Works through UMVVMEditorSubsystem like the MVVM editor panels do.
 *
 * Ids (interoperable with the official MVVM toolset):
 *  - BindingId = FMVVMBlueprintViewBinding::BindingId, written "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" (same as
 *    ListWidgetViewBindings / CreateViewBinding). Any FGuid text form is accepted.
 *  - ViewModelName = the viewmodel property name on the widget (FMVVMBlueprintViewModelContext::ViewModelName). The viewmodel
 *    id (GUID) or the viewmodel class name/path (what the official toolset uses) is also accepted when unambiguous.
 *  - Event / condition id = the object name of the UMVVMBlueprintViewEvent / UMVVMBlueprintViewCondition (see DumpWidgetMVVM).
 * Path syntax (same contexts as the official CreateViewBinding, written as one string): "<Context>.<Field>[.<Field>...]"
 *  where Context is a viewmodel name (or class name), a widget name from the widget tree, or "Self" (the widget blueprint).
 *  Fields are properties or functions, e.g. "VM_Player.Health", "Txt_Score.Text", "Self.SetScoreText", "VM_Player.Stats.Level".
 * Literal values use the UE text-import format of the pin type, e.g. "true", "3", "Hello", "(R=1,G=0,B=0,A=1)".
 * Edits are transacted and mark the Blueprint modified like the editor; they do not compile unless stated
 * (use CompileWidgetMVVM). Nothing is saved; save with the official asset tools.
 */
UCLASS(BlueprintType)
class UMCPInspectorMVVMToolset : public UObject
{
	GENERATED_BODY()

public:

	// ---- bindings ----

	/**
	 * Set the binding mode (direction) of a view binding (UMVVMEditorSubsystem::SetBindingTypeForBinding, like the binding row combo).
	 * @param WidgetPath Widget Blueprint asset path, e.g. "/Game/UI/WBP_HUD".
	 * @param BindingId Binding id (from ListWidgetViewBindings, CreateViewBinding or DumpWidgetMVVM).
	 * @param Mode OneTimeToDestination, OneWayToDestination, TwoWay or OneWayToSource ("ToWidget"/"ToViewModel" aliases accepted).
	 * @return "OK" or "ERROR|...". Not compiled; the compiler reports unsupported directions (e.g. TwoWay to a read-only field).
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|MVVM", meta = (AICallable))
	static FString SetBindingMode(const FString& WidgetPath, const FString& BindingId, const FString& Mode);

	/**
	 * Override or reset the execution mode of a view binding.
	 * @param ExecutionMode Default (remove the override), Immediate, Delayed, Tick or DelayedWhenSharedElseImmediate (alias "Auto").
	 *  Modes not in the project's MVVM "Allowed Execution Mode" setting are rejected.
	 * @return "OK" or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|MVVM", meta = (AICallable))
	static FString SetBindingExecutionMode(const FString& WidgetPath, const FString& BindingId, const FString& ExecutionMode);

	/**
	 * Set the Enabled and Compile flags of a view binding.
	 * @param bEnabled Binding enabled by default (instances may still enable it at runtime).
	 * @param bCompile False keeps the binding in the editor but excludes it from compilation.
	 * @return "OK" or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|MVVM", meta = (AICallable))
	static FString SetBindingEnabled(const FString& WidgetPath, const FString& BindingId, bool bEnabled, bool bCompile);

	/**
	 * Change the source and/or destination field of an existing binding (keeps its id, mode and flags).
	 * Refuses to replace a side that is driven by a conversion function (edit it with SetConversionFunctionArgument instead).
	 * @param SourcePath New source path, e.g. "VM_Player.Health". Empty = unchanged.
	 * @param DestinationPath New destination path, e.g. "Bar_Health.Percent". Empty = unchanged.
	 * @return JSON {"id","source","destination"} (resolved path text) or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|MVVM", meta = (AICallable))
	static FString UpdateBindingPaths(const FString& WidgetPath, const FString& BindingId, const FString& SourcePath, const FString& DestinationPath);

	/**
	 * Set an input of a binding's conversion function to a literal value or bind it to a field path.
	 * @param ArgumentName Conversion function parameter name, e.g. "bUseGrouping" (split pins: "Parent.Child"). See DumpWidgetMVVM.
	 * @param ValueOrPath "path:<Context>.<Field>" binds the argument to a field ("path:" alone unbinds it);
	 *  anything else is a literal in pin default-value format (a "value:" prefix is stripped), e.g. "false", "2", "Hello".
	 * @param bSourceToDestination True for the source->destination conversion, false for destination->source.
	 * @return JSON {"argument","value"|"path"} or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|MVVM", meta = (AICallable))
	static FString SetConversionFunctionArgument(const FString& WidgetPath, const FString& BindingId, const FString& ArgumentName, const FString& ValueOrPath, bool bSourceToDestination);

	// ---- viewmodels on a widget ----

	/**
	 * Remove a viewmodel from a widget and report the bindings, events and conditions that referenced it.
	 * @param ViewModelName Viewmodel name (or id / class name).
	 * @param bRemoveReferencingBindings Also remove those bindings, events and conditions (otherwise they are kept and will fail to compile).
	 * @return JSON {"removed":name,"referencingBindings":[{"id","text"}],"referencingEvents":[id],"referencingConditions":[id],
	 *  "referencesRemoved":bool,"usedInGraphs":bool} or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|MVVM", meta = (AICallable))
	static FString RemoveViewModelFromWidget(const FString& WidgetPath, const FString& ViewModelName, bool bRemoveReferencingBindings);

	/**
	 * Change how a widget gets its viewmodel instance. Applies the same side effects as the details panel
	 * (Manual => setter + optional; Context => optional).
	 * @param CreationType Manual, CreateInstance, GlobalViewModelCollection, PropertyPath, Resolver or Context
	 *  (must be allowed for the viewmodel class and by the project settings).
	 * @param Argument GlobalViewModelCollection: global viewmodel identifier (required). PropertyPath: the property/function path
	 *  evaluated on the widget, e.g. "GetOwningPlayer.MyViewModel" (required). Resolver: resolver class path
	 *  ("/Script/Module.MyResolver" or a Blueprint path); empty = project default resolver. Ignored for other types.
	 * @return JSON with the updated viewmodel (same shape as DumpWidgetMVVM "viewModels" entries) or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|MVVM", meta = (AICallable))
	static FString SetViewModelCreationType(const FString& WidgetPath, const FString& ViewModelName, const FString& CreationType, const FString& Argument);

	/**
	 * Set the advanced options of a widget viewmodel.
	 * @param bExposeInstanceInEditor Expose the instance on every widget instance for editing (only meaningful for CreateInstance).
	 * @param bCreateSetterFunction Generate a public setter (always on for Manual).
	 * @param bOptional Do not warn when the instance is missing (always on for Manual and Context).
	 * @return JSON with the updated viewmodel plus "warnings":[...] or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|MVVM", meta = (AICallable))
	static FString SetViewModelOptions(const FString& WidgetPath, const FString& ViewModelName, bool bExposeInstanceInEditor, bool bCreateSetterFunction, bool bOptional);

	/**
	 * Rename a widget viewmodel (renames the generated property and updates graph references, like the viewmodel panel).
	 * Unloaded Blueprints that reference this widget are loaded first (as the editor does when you answer Yes).
	 * @return JSON {"oldName","newName","loadedReferencers":[paths]} or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|MVVM", meta = (AICallable))
	static FString RenameViewModel(const FString& WidgetPath, const FString& ViewModelName, const FString& NewName);

	// ---- viewmodel Blueprints ----

	/**
	 * Remove a member variable (e.g. a FieldNotify property) from a viewmodel Blueprint (a UMVVMViewModelBase subclass).
	 * Graph nodes using it are removed with it; widget bindings in other assets that used it fail on their next compile.
	 * @param ViewModelPath Viewmodel Blueprint asset path, e.g. "/Game/UI/VM_Player".
	 * @return JSON {"removed","fieldNotify":bool,"wasUsedInGraphs":bool} or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|MVVM", meta = (AICallable))
	static FString RemoveViewModelProperty(const FString& ViewModelPath, const FString& PropertyName);

	/**
	 * Set the default value (class default object) of a viewmodel Blueprint variable.
	 * If the variable is not on the generated class yet (added but never compiled), the Blueprint is compiled first (not saved).
	 * @param ValueText Value in UE text-import format, e.g. "100", "true", "Hello", "(X=1,Y=2,Z=3)".
	 * @return JSON {"property","value","compiled":bool} (value read back) or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|MVVM", meta = (AICallable))
	static FString SetViewModelPropertyDefault(const FString& ViewModelPath, const FString& PropertyName, const FString& ValueText);

	// ---- view events / conditions ----

	/**
	 * Add a view event: when a multicast delegate fires, call a function (requires the MVVM "Allow Binding Event" project setting).
	 * @param EventPath Path to a multicast delegate property, e.g. "Btn_Buy.OnClicked" or "Self.OnVisibilityChanged".
	 * @param DestinationFunctionPath Function to call, e.g. "VM_Shop.Buy" or "Self.HandleClicked". Set its parameters with SetViewEventArgument.
	 * @return JSON {"id","eventPath","destination","arguments":[...]} or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|MVVM", meta = (AICallable))
	static FString AddViewEvent(const FString& WidgetPath, const FString& EventPath, const FString& DestinationFunctionPath);

	/**
	 * Set a parameter of a view event's destination function to a literal or to a field path (same ValueOrPath rules as SetConversionFunctionArgument).
	 * @param EventId Event id from AddViewEvent / DumpWidgetMVVM.
	 * @return JSON {"argument","value"|"path"} or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|MVVM", meta = (AICallable))
	static FString SetViewEventArgument(const FString& WidgetPath, const FString& EventId, const FString& ArgumentName, const FString& ValueOrPath);

	/**
	 * Remove a view event.
	 * @return "OK" or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|MVVM", meta = (AICallable))
	static FString RemoveViewEvent(const FString& WidgetPath, const FString& EventId);

	/**
	 * Add a view condition: when a numeric field satisfies a comparison, call a function
	 * (requires the MVVM "Allow Condition Binding" project setting).
	 * @param ConditionPath Source field, e.g. "VM_Player.Health".
	 * @param Operation Equal, NotEqual, MoreThan, MoreThanOrEqual, LessThan, LessThanOrEqual, BetweenInclusive or BetweenExclusive.
	 * @param Value Comparison value (lower bound for Between*).
	 * @param MaxValue Upper bound for Between* (ignored otherwise).
	 * @param DestinationFunctionPath Function to call, e.g. "Self.PlayLowHealthAnim".
	 * @return JSON {"id","conditionPath","operation","value","maxValue","destination"} or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|MVVM", meta = (AICallable))
	static FString AddViewCondition(const FString& WidgetPath, const FString& ConditionPath, const FString& Operation, float Value, float MaxValue, const FString& DestinationFunctionPath);

	/**
	 * Remove a view condition.
	 * @return "OK" or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|MVVM", meta = (AICallable))
	static FString RemoveViewCondition(const FString& WidgetPath, const FString& ConditionId);

	// ---- inspection ----

	/**
	 * Dump the widget's MVVM view with ids usable by these tools and by the official MVVM toolset.
	 * @return JSON {"widget","status","hasView","settings":{...},
	 *  "viewModels":[{"name","id","class","creationType","globalViewModelIdentifier","propertyPath","resolver","instanced",
	 *    "exposeInstanceInEditor","createSetterFunction","createGetterFunction","optional","canRename","canEdit","canRemove","allowedCreationTypes":[...]}],
	 *  "bindings":[{"id","text","source":path,"destination":path,"mode","executionMode","enabled","compile",
	 *    "conversion":{"sourceToDestination":conv?,"destinationToSource":conv?},"messages":[{"severity","text"}]}],
	 *  "events":[{"id","eventPath":path,"destination":path,"enabled","compile","arguments":[pin],"messages":[...]}],
	 *  "conditions":[{"id","conditionPath":path,"operation","value","maxValue","destination":path,"enabled","compile","arguments":[pin],"messages":[...]}]}
	 *  where path = {"valid","spec","text","source","widget"?,"viewModelId"?,"viewModelName"?,"fields":[...]} ("spec" is re-usable as a path argument),
	 *  conv = {"function","type","valid","wrapperGraph","arguments":[pin]} and pin = {"name","id","value","path"?,"split","orphaned","linked"?}.
	 *  Messages come from the last compile in this editor session (see CompileWidgetMVVM). Or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|MVVM", meta = (AICallable))
	static FString DumpWidgetMVVM(const FString& WidgetPath);

	/**
	 * Compile the Widget Blueprint (never saves) and return compiler and per-binding MVVM messages.
	 * @return JSON {"status","errors","warnings","messages":[{"severity","text"}],"bindingMessages":[{"id","kind","severity","text"}]} or "ERROR|...".
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintInspector|MVVM", meta = (AICallable))
	static FString CompileWidgetMVVM(const FString& WidgetPath);
};
