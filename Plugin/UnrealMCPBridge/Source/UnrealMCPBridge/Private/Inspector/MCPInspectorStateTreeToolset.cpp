#include "MCPInspectorStateTreeToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/StateTreeConditionBlueprintBase.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/FeedbackContext.h"
#include "Misc/PackageName.h"
#include "PropertyBindingBindableStructDescriptor.h"
#include "PropertyBindingPath.h"
#include "ScopedTransaction.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StateTree.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeConditionBase.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeFactory.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "StateTreeTaskBase.h"
#include "StateTreeTypes.h"
#include "StateTreeViewModel.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "MCPInspectorStateTreeToolset"

namespace MCPInspectorStateTreeToolset
{
	static FString Err(const FString& Msg) { return TEXT("ERROR|") + Msg; }

	static FString ToObjectPath(const FString& Path)
	{
		return Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPackageName::GetShortName(Path);
	}

	static FString IdStr(const FGuid& Guid) { return Guid.ToString(EGuidFormats::Digits); }

	static FString ToJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	static UStateTree* LoadTree(const FString& Path)
	{
		const FString P = Path.TrimStartAndEnd();
		return P.IsEmpty() ? nullptr : LoadObject<UStateTree>(nullptr, *ToObjectPath(P));
	}

	/** UStateTreeEditorData::GetEditorData is a non-exported UFUNCTION; read the public UPROPERTY instead. */
	static UStateTreeEditorData* GetEditorData(UStateTree* ST)
	{
		return ST ? Cast<UStateTreeEditorData>(ST->EditorData) : nullptr;
	}

	static bool LoadTreeAndData(const FString& TreePath, UStateTree*& OutST, UStateTreeEditorData*& OutED, FString& OutError)
	{
		OutST = LoadTree(TreePath);
		if (!OutST)
		{
			OutError = TEXT("state tree not found: ") + TreePath;
			return false;
		}
		OutED = GetEditorData(OutST);
		if (!OutED)
		{
			OutError = TEXT("state tree has no editor data: ") + TreePath;
			return false;
		}
		return true;
	}

	/** Same as UStateTreeEditorMode::UpdateAsset after an edit, plus refreshing an open StateTree editor. */
	static void Commit(UStateTree* ST)
	{
		UStateTreeEditingSubsystem::ValidateStateTree(ST);
		UStateTreeEditingSubsystem::MarkAsModified(ST);
		ST->MarkPackageDirty();
		if (GEditor)
		{
			if (UStateTreeEditingSubsystem* Subsystem = GEditor->GetEditorSubsystem<UStateTreeEditingSubsystem>())
			{
				if (TSharedPtr<FStateTreeViewModel> ViewModel = Subsystem->FindViewModel(ST))
				{
					ViewModel->NotifyAssetChangedExternally();
				}
			}
		}
	}

	static TSharedPtr<FStateTreeViewModel> FindViewModel(UStateTree* ST)
	{
		if (GEditor)
		{
			if (UStateTreeEditingSubsystem* Subsystem = GEditor->GetEditorSubsystem<UStateTreeEditingSubsystem>())
			{
				return Subsystem->FindViewModel(ST);
			}
		}
		return nullptr;
	}

	// ---- enum parsing (manual, avoids depending on StaticEnum<> export of StateTreeModule enums) ----------

	static bool ParseStateType(const FString& In, EStateTreeStateType& Out)
	{
		const FString S = In.TrimStartAndEnd().ToLower();
		if (S.IsEmpty() || S == TEXT("state")) { Out = EStateTreeStateType::State; return true; }
		if (S == TEXT("group")) { Out = EStateTreeStateType::Group; return true; }
		if (S == TEXT("linked")) { Out = EStateTreeStateType::Linked; return true; }
		if (S == TEXT("linkedasset")) { Out = EStateTreeStateType::LinkedAsset; return true; }
		if (S == TEXT("subtree")) { Out = EStateTreeStateType::Subtree; return true; }
		return false;
	}

	static FString StateTypeStr(EStateTreeStateType T)
	{
		switch (T)
		{
		case EStateTreeStateType::State: return TEXT("State");
		case EStateTreeStateType::Group: return TEXT("Group");
		case EStateTreeStateType::Linked: return TEXT("Linked");
		case EStateTreeStateType::LinkedAsset: return TEXT("LinkedAsset");
		case EStateTreeStateType::Subtree: return TEXT("Subtree");
		default: return TEXT("Unknown");
		}
	}

	static bool ParseTrigger(const FString& In, EStateTreeTransitionTrigger& Out)
	{
		const FString S = In.TrimStartAndEnd().ToLower();
		if (S == TEXT("onstatecompleted") || S.IsEmpty()) { Out = EStateTreeTransitionTrigger::OnStateCompleted; return true; }
		if (S == TEXT("onstatesucceeded")) { Out = EStateTreeTransitionTrigger::OnStateSucceeded; return true; }
		if (S == TEXT("onstatefailed")) { Out = EStateTreeTransitionTrigger::OnStateFailed; return true; }
		if (S == TEXT("ontick")) { Out = EStateTreeTransitionTrigger::OnTick; return true; }
		if (S == TEXT("onevent")) { Out = EStateTreeTransitionTrigger::OnEvent; return true; }
		if (S == TEXT("ondelegate")) { Out = EStateTreeTransitionTrigger::OnDelegate; return true; }
		return false;
	}

	static FString TriggerStr(EStateTreeTransitionTrigger T)
	{
		switch (T)
		{
		case EStateTreeTransitionTrigger::OnStateCompleted: return TEXT("OnStateCompleted");
		case EStateTreeTransitionTrigger::OnStateSucceeded: return TEXT("OnStateSucceeded");
		case EStateTreeTransitionTrigger::OnStateFailed: return TEXT("OnStateFailed");
		case EStateTreeTransitionTrigger::OnTick: return TEXT("OnTick");
		case EStateTreeTransitionTrigger::OnEvent: return TEXT("OnEvent");
		case EStateTreeTransitionTrigger::OnDelegate: return TEXT("OnDelegate");
		default: return TEXT("None");
		}
	}

	static bool ParseTransitionType(const FString& In, EStateTreeTransitionType& Out)
	{
		const FString S = In.TrimStartAndEnd().ToLower();
		if (S == TEXT("gotostate")) { Out = EStateTreeTransitionType::GotoState; return true; }
		if (S == TEXT("succeeded")) { Out = EStateTreeTransitionType::Succeeded; return true; }
		if (S == TEXT("failed")) { Out = EStateTreeTransitionType::Failed; return true; }
		if (S == TEXT("nextstate")) { Out = EStateTreeTransitionType::NextState; return true; }
		if (S == TEXT("nextselectablestate")) { Out = EStateTreeTransitionType::NextSelectableState; return true; }
		if (S == TEXT("parent")) { Out = EStateTreeTransitionType::Parent; return true; }
		if (S == TEXT("nextparent")) { Out = EStateTreeTransitionType::NextParent; return true; }
		if (S == TEXT("nextselectableparent")) { Out = EStateTreeTransitionType::NextSelectableParent; return true; }
		if (S == TEXT("none")) { Out = EStateTreeTransitionType::None; return true; }
		return false;
	}

	static FString TransitionTypeStr(EStateTreeTransitionType T)
	{
		switch (T)
		{
		case EStateTreeTransitionType::None: return TEXT("None");
		case EStateTreeTransitionType::Succeeded: return TEXT("Succeeded");
		case EStateTreeTransitionType::Failed: return TEXT("Failed");
		case EStateTreeTransitionType::GotoState: return TEXT("GotoState");
		case EStateTreeTransitionType::Parent: return TEXT("Parent");
		case EStateTreeTransitionType::NextState: return TEXT("NextState");
		case EStateTreeTransitionType::NextSelectableState: return TEXT("NextSelectableState");
		case EStateTreeTransitionType::NextParent: return TEXT("NextParent");
		case EStateTreeTransitionType::NextSelectableParent: return TEXT("NextSelectableParent");
		default: return TEXT("Unknown");
		}
	}

	// ---- lookup ---------------------------------------------------------------------------------------

	struct FFound
	{
		UStateTreeState* State = nullptr;              // the id is a state
		UStateTreeState* OwnerState = nullptr;         // owner of a node/transition (nullptr for global nodes)
		FStateTreeEditorNode* Node = nullptr;          // the id is an editor node
		TArray<FStateTreeEditorNode>* NodeArray = nullptr;
		int32 NodeIndex = INDEX_NONE;                  // INDEX_NONE + Node => SingleTask
		FStateTreeTransition* Transition = nullptr;    // the id is a transition
		int32 TransitionIndex = INDEX_NONE;
	};

	static bool FindInNodeArray(TArray<FStateTreeEditorNode>& Array, const FGuid& Id, UStateTreeState* Owner, FFound& Out)
	{
		for (int32 i = 0; i < Array.Num(); ++i)
		{
			if (Array[i].ID == Id)
			{
				Out.OwnerState = Owner;
				Out.Node = &Array[i];
				Out.NodeArray = &Array;
				Out.NodeIndex = i;
				return true;
			}
		}
		return false;
	}

	static bool FindInState(UStateTreeState* State, const FGuid& Id, FFound& Out)
	{
		if (!State)
		{
			return false;
		}
		if (State->ID == Id)
		{
			Out.State = State;
			return true;
		}
		if (FindInNodeArray(State->EnterConditions, Id, State, Out)
			|| FindInNodeArray(State->Tasks, Id, State, Out)
			|| FindInNodeArray(State->Considerations, Id, State, Out))
		{
			return true;
		}
		if (State->SingleTask.ID == Id && State->SingleTask.ID.IsValid())
		{
			Out.OwnerState = State;
			Out.Node = &State->SingleTask;
			return true;
		}
		for (int32 t = 0; t < State->Transitions.Num(); ++t)
		{
			FStateTreeTransition& Transition = State->Transitions[t];
			if (Transition.ID == Id)
			{
				Out.OwnerState = State;
				Out.Transition = &Transition;
				Out.TransitionIndex = t;
				return true;
			}
			if (FindInNodeArray(Transition.Conditions, Id, State, Out))
			{
				return true;
			}
		}
		for (UStateTreeState* Child : State->Children)
		{
			if (FindInState(Child, Id, Out))
			{
				return true;
			}
		}
		return false;
	}

	static bool FindById(UStateTreeEditorData* ED, const FString& IdText, FFound& Out, FString& OutError)
	{
		FGuid Id;
		if (!FGuid::Parse(IdText.TrimStartAndEnd(), Id))
		{
			OutError = TEXT("invalid id: ") + IdText;
			return false;
		}
		if (FindInNodeArray(ED->Evaluators, Id, nullptr, Out) || FindInNodeArray(ED->GlobalTasks, Id, nullptr, Out))
		{
			return true;
		}
		for (UStateTreeState* SubTree : ED->SubTrees)
		{
			if (FindInState(SubTree, Id, Out))
			{
				return true;
			}
		}
		OutError = TEXT("id not found: ") + IdText;
		return false;
	}

	static UStateTreeState* FindState(UStateTreeEditorData* ED, const FString& IdText, FString& OutError)
	{
		FFound Found;
		if (!FindById(ED, IdText, Found, OutError))
		{
			return nullptr;
		}
		if (!Found.State)
		{
			OutError = TEXT("id is not a state: ") + IdText;
			return nullptr;
		}
		return Found.State;
	}

	// ---- node type resolution -------------------------------------------------------------------------

	/**
	 * Resolves a node type: a native struct derived from BaseStruct, or a Blueprint class derived from BlueprintBase.
	 * Exactly one of OutStruct / OutClass is set on success.
	 */
	static bool ResolveNodeType(const FString& InName, const UScriptStruct* BaseStruct, UClass* BlueprintBase,
		const UScriptStruct*& OutStruct, UClass*& OutClass, FString& OutError)
	{
		OutStruct = nullptr;
		OutClass = nullptr;
		const FString Name = InName.TrimStartAndEnd();

		if (Name.StartsWith(TEXT("/Script/")))
		{
			OutStruct = FindObject<UScriptStruct>(nullptr, *Name);
			if (!OutStruct)
			{
				OutClass = FindObject<UClass>(nullptr, *Name);
			}
		}
		else if (Name.StartsWith(TEXT("/")))
		{
			UObject* Obj = LoadObject<UObject>(nullptr, *ToObjectPath(Name));
			if (UBlueprint* BP = Cast<UBlueprint>(Obj))
			{
				OutClass = BP->GeneratedClass.Get();
			}
			else if (UClass* AsClass = Cast<UClass>(Obj))
			{
				OutClass = AsClass;
			}
		}
		else
		{
			TArray<FString> Candidates = { Name };
			if (Name.StartsWith(TEXT("F")))
			{
				Candidates.Add(Name.RightChop(1));
			}
			for (const FString& Candidate : Candidates)
			{
				if (UScriptStruct* S = FindFirstObject<UScriptStruct>(*Candidate, EFindFirstObjectOptions::NativeFirst))
				{
					OutStruct = S;
					break;
				}
			}
			if (!OutStruct)
			{
				OutClass = FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst);
			}
		}

		if (OutStruct)
		{
			if (!OutStruct->IsChildOf(BaseStruct))
			{
				OutError = FString::Printf(TEXT("%s is not a %s"), *OutStruct->GetPathName(), *BaseStruct->GetName());
				return false;
			}
			if (OutStruct->HasMetaData(TEXT("Hidden")))
			{
				OutError = FString::Printf(TEXT("%s is a hidden/base node type"), *OutStruct->GetPathName());
				return false;
			}
			return true;
		}
		if (OutClass)
		{
			if (!OutClass->IsChildOf(BlueprintBase) || OutClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				OutError = FString::Printf(TEXT("%s is not a concrete %s"), *OutClass->GetPathName(), *BlueprintBase->GetName());
				return false;
			}
			return true;
		}
		OutError = TEXT("node type not found: ") + Name;
		return false;
	}

	static bool CheckSchema(const UStateTreeEditorData* ED, const UScriptStruct* Struct, const UClass* Class, FString& OutError)
	{
		const UStateTreeSchema* Schema = ED->Schema;
		if (!Schema)
		{
			return true;
		}
		const bool bAllowed = Struct ? Schema->IsStructAllowed(Struct) : Schema->IsClassAllowed(Class);
		if (!bAllowed)
		{
			OutError = FString::Printf(TEXT("%s is not allowed by schema %s"),
				Struct ? *Struct->GetPathName() : *Class->GetPathName(), *Schema->GetClass()->GetName());
		}
		return bAllowed;
	}

	/**
	 * Initializes an editor node like the StateTree editor node picker (StateTreeEditorNodeUtils SetNodeTypeStruct / SetNodeTypeClass).
	 * Blueprint classes use the wrapper struct; the class is set through reflection (the wrapper types are not constructed here).
	 */
	static bool InitEditorNode(FStateTreeEditorNode& EdNode, UObject* Outer, const UScriptStruct* Struct, UClass* Class,
		const UScriptStruct* WrapperStruct, const TCHAR* WrapperClassProperty, FString& OutError)
	{
		if (Struct)
		{
			EdNode.InitializeAs(Outer, Struct);
			return true;
		}
		EdNode.InitializeAs(Outer, WrapperStruct);
		FObjectPropertyBase* ClassProp = FindFProperty<FObjectPropertyBase>(WrapperStruct, WrapperClassProperty);
		uint8* Memory = EdNode.Node.GetMutableMemory();
		if (!ClassProp || !Memory)
		{
			OutError = FString::Printf(TEXT("%s.%s not found"), *WrapperStruct->GetName(), WrapperClassProperty);
			return false;
		}
		ClassProp->SetObjectPropertyValue(ClassProp->ContainerPtrToValuePtr<void>(Memory), Class);
		EdNode.ReallocInstanceData(Outer); // creates the Blueprint instance object (GetInstanceDataType() == class), keeps ID
		return true;
	}

	// ---- reflection path helpers ("A.B[2].C") ---------------------------------------------------------

	static bool ParseSegment(const FString& Segment, FString& OutName, int32& OutIndex)
	{
		OutIndex = INDEX_NONE;
		int32 Open = INDEX_NONE;
		if (Segment.FindChar(TEXT('['), Open))
		{
			if (!Segment.EndsWith(TEXT("]")))
			{
				return false;
			}
			OutName = Segment.Left(Open);
			const FString IndexStr = Segment.Mid(Open + 1, Segment.Len() - Open - 2);
			if (!IndexStr.IsNumeric())
			{
				return false;
			}
			OutIndex = FCString::Atoi(*IndexStr);
		}
		else
		{
			OutName = Segment;
		}
		return !OutName.IsEmpty();
	}

	/** Resolves "A.B[2].C" in (Struct, Container). OutChain receives every resolved property (member first). */
	static bool ResolvePath(const UStruct* Struct, void* Container, const FString& Path,
		FProperty*& OutProp, void*& OutPtr, TArray<FProperty*>& OutChain, FString& OutError)
	{
		TArray<FString> Segments;
		Path.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() == 0)
		{
			OutError = TEXT("empty property path");
			return false;
		}
		const UStruct* CurStruct = Struct;
		void* CurContainer = Container;
		OutChain.Reset();

		for (int32 i = 0; i < Segments.Num(); ++i)
		{
			FString Name;
			int32 Index;
			if (!ParseSegment(Segments[i], Name, Index))
			{
				OutError = FString::Printf(TEXT("bad path segment '%s'"), *Segments[i]);
				return false;
			}
			FProperty* Prop = FindFProperty<FProperty>(CurStruct, *Name);
			if (!Prop)
			{
				OutError = FString::Printf(TEXT("property '%s' not found on %s"), *Name, *CurStruct->GetName());
				return false;
			}
			OutChain.Add(Prop);
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CurContainer);
			if (Index != INDEX_NONE)
			{
				if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
				{
					FScriptArrayHelper Helper(ArrayProp, ValuePtr);
					if (!Helper.IsValidIndex(Index))
					{
						OutError = FString::Printf(TEXT("index %d out of range for '%s' (num=%d)"), Index, *Name, Helper.Num());
						return false;
					}
					Prop = ArrayProp->Inner;
					ValuePtr = Helper.GetRawPtr(Index);
				}
				else if (Prop->ArrayDim > 1 && Index < Prop->ArrayDim)
				{
					ValuePtr = Prop->ContainerPtrToValuePtr<void>(CurContainer, Index);
				}
				else
				{
					OutError = FString::Printf(TEXT("'%s' is not an array or index out of range"), *Name);
					return false;
				}
			}
			if (i == Segments.Num() - 1)
			{
				OutProp = Prop;
				OutPtr = ValuePtr;
				return true;
			}
			if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
			{
				CurStruct = StructProp->Struct;
				CurContainer = ValuePtr;
			}
			else if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
			{
				UObject* Inner = ObjProp->GetObjectPropertyValue(ValuePtr);
				if (!Inner)
				{
					OutError = FString::Printf(TEXT("'%s' is null - cannot descend"), *Name);
					return false;
				}
				CurStruct = Inner->GetClass();
				CurContainer = Inner;
			}
			else
			{
				OutError = FString::Printf(TEXT("'%s' is not a struct/object - cannot descend"), *Name);
				return false;
			}
		}
		OutError = TEXT("unreachable");
		return false;
	}

	static bool ImportValue(FProperty* Prop, void* ValuePtr, UObject* Owner, const FString& ValueText, FString& OutError)
	{
		const FString Value = ValueText.TrimStartAndEnd();
		if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			if (Value.StartsWith(TEXT("/")))
			{
				StaticLoadObject(ObjProp->PropertyClass, nullptr, *ToObjectPath(Value));
			}
		}
		if (!Prop->ImportText_Direct(*Value, ValuePtr, Owner, PPF_None))
		{
			OutError = FString::Printf(TEXT("ImportText failed for %s = %s"), *Prop->GetName(), *Value);
			return false;
		}
		return true;
	}

	// ---- dump ---------------------------------------------------------------------------------------

	static TSharedRef<FJsonObject> DumpEdNode(const UStateTreeEditorData* ED, const FStateTreeEditorNode& N)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), IdStr(N.ID));
		O->SetStringField(TEXT("name"), N.GetName().ToString());
		const UScriptStruct* NodeStruct = N.Node.GetScriptStruct();
		O->SetStringField(TEXT("struct"), NodeStruct ? NodeStruct->GetPathName() : FString());
		if (N.InstanceObject)
		{
			O->SetStringField(TEXT("instanceClass"), N.InstanceObject->GetClass()->GetPathName());
		}
		else if (const UScriptStruct* InstanceStruct = N.Instance.GetScriptStruct())
		{
			O->SetStringField(TEXT("instanceStruct"), InstanceStruct->GetPathName());
		}
		O->SetNumberField(TEXT("indent"), N.ExpressionIndent);
		O->SetNumberField(TEXT("operand"), (double)(uint8)N.ExpressionOperand);
		if (NodeStruct)
		{
			O->SetStringField(TEXT("description"), ED->GetNodeDescription(N, EStateTreeNodeFormatting::Text).ToString());
		}
		return O;
	}

	static TArray<TSharedPtr<FJsonValue>> DumpEdNodes(const UStateTreeEditorData* ED, const TArray<FStateTreeEditorNode>& Nodes)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FStateTreeEditorNode& N : Nodes)
		{
			Arr.Add(MakeShared<FJsonValueObject>(DumpEdNode(ED, N)));
		}
		return Arr;
	}

	static TSharedRef<FJsonObject> DumpState(const UStateTreeEditorData* ED, const UStateTreeState* State)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), IdStr(State->ID));
		O->SetStringField(TEXT("name"), State->Name.ToString());
		O->SetStringField(TEXT("type"), StateTypeStr(State->Type));
		O->SetNumberField(TEXT("selectionBehavior"), (double)(uint8)State->SelectionBehavior);
		O->SetBoolField(TEXT("enabled"), State->bEnabled);
		if (State->Tag.IsValid())
		{
			O->SetStringField(TEXT("tag"), State->Tag.ToString());
		}
		if (State->LinkedSubtree.ID.IsValid())
		{
			O->SetStringField(TEXT("linkedSubtreeId"), IdStr(State->LinkedSubtree.ID));
		}
		if (State->LinkedAsset)
		{
			O->SetStringField(TEXT("linkedAsset"), State->LinkedAsset->GetPathName());
		}
		O->SetArrayField(TEXT("enterConditions"), DumpEdNodes(ED, State->EnterConditions));
		O->SetArrayField(TEXT("tasks"), DumpEdNodes(ED, State->Tasks));
		if (State->SingleTask.Node.IsValid())
		{
			O->SetObjectField(TEXT("singleTask"), DumpEdNode(ED, State->SingleTask));
		}
		O->SetArrayField(TEXT("considerations"), DumpEdNodes(ED, State->Considerations));

		TArray<TSharedPtr<FJsonValue>> Transitions;
		for (const FStateTreeTransition& T : State->Transitions)
		{
			TSharedRef<FJsonObject> TO = MakeShared<FJsonObject>();
			TO->SetStringField(TEXT("id"), IdStr(T.ID));
			TO->SetStringField(TEXT("trigger"), TriggerStr(T.Trigger));
			TO->SetStringField(TEXT("type"), TransitionTypeStr(T.State.LinkType));
			TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
			Target->SetStringField(TEXT("id"), T.State.ID.IsValid() ? IdStr(T.State.ID) : FString());
			Target->SetStringField(TEXT("name"), T.State.Name.ToString());
			TO->SetObjectField(TEXT("target"), Target);
			TO->SetNumberField(TEXT("priority"), (double)(uint8)T.Priority);
			TO->SetBoolField(TEXT("enabled"), T.bTransitionEnabled);
			if (T.RequiredEvent.Tag.IsValid())
			{
				TO->SetStringField(TEXT("eventTag"), T.RequiredEvent.Tag.ToString());
			}
			TO->SetArrayField(TEXT("conditions"), DumpEdNodes(ED, T.Conditions));
			Transitions.Add(MakeShared<FJsonValueObject>(TO));
		}
		O->SetArrayField(TEXT("transitions"), Transitions);

		TArray<TSharedPtr<FJsonValue>> Children;
		for (const UStateTreeState* Child : State->Children)
		{
			if (Child)
			{
				Children.Add(MakeShared<FJsonValueObject>(DumpState(ED, Child)));
			}
		}
		O->SetArrayField(TEXT("children"), Children);
		return O;
	}
}

// =====================================================================================================












// ---- property bindings ---------------------------------------------------------------------------------------

namespace MCPInspectorStateTreeToolset
{
	static bool GetBindableSources(const UStateTreeEditorData* ED, const FGuid& TargetId,
		TArray<TInstancedStruct<FPropertyBindingBindableStructDescriptor>>& Out)
	{
		ED->GetBindableStructs(TargetId, Out);
		return Out.Num() > 0;
	}

	static FInstancedPropertyBag* GetRootParameterBag(UStateTreeEditorData* ED)
	{
		// RootParameterPropertyBag is private in 5.8; GetRootParametersPropertyBag() is const-only.
		static const FName BagName(TEXT("RootParameterPropertyBag"));
		const FStructProperty* Prop = FindFProperty<FStructProperty>(UStateTreeEditorData::StaticClass(), BagName);
		return (Prop && Prop->Struct == FInstancedPropertyBag::StaticStruct()) ? Prop->ContainerPtrToValuePtr<FInstancedPropertyBag>(ED) : nullptr;
	}

	static bool ParseBagType(const FString& TypeStr, EPropertyBagPropertyType& OutType, UObject*& OutTypeObject, FString& OutError)
	{
		OutTypeObject = nullptr;
		FString Kind = TypeStr.TrimStartAndEnd(), Ref;
		TypeStr.TrimStartAndEnd().Split(TEXT(":"), &Kind, &Ref);
		Kind = Kind.ToLower();
		if (Kind == TEXT("bool")) OutType = EPropertyBagPropertyType::Bool;
		else if (Kind == TEXT("int")) OutType = EPropertyBagPropertyType::Int32;
		else if (Kind == TEXT("int64")) OutType = EPropertyBagPropertyType::Int64;
		else if (Kind == TEXT("float")) OutType = EPropertyBagPropertyType::Float;
		else if (Kind == TEXT("double")) OutType = EPropertyBagPropertyType::Double;
		else if (Kind == TEXT("name")) OutType = EPropertyBagPropertyType::Name;
		else if (Kind == TEXT("string")) OutType = EPropertyBagPropertyType::String;
		else if (Kind == TEXT("text")) OutType = EPropertyBagPropertyType::Text;
		else if (Kind == TEXT("enum")) { OutType = EPropertyBagPropertyType::Enum; OutTypeObject = LoadObject<UEnum>(nullptr, *ToObjectPath(Ref)); }
		else if (Kind == TEXT("struct")) { OutType = EPropertyBagPropertyType::Struct; OutTypeObject = LoadObject<UScriptStruct>(nullptr, *ToObjectPath(Ref)); }
		else if (Kind == TEXT("object")) { OutType = EPropertyBagPropertyType::Object; OutTypeObject = LoadObject<UClass>(nullptr, *Ref); }
		else if (Kind == TEXT("class")) { OutType = EPropertyBagPropertyType::Class; OutTypeObject = LoadObject<UClass>(nullptr, *Ref); }
		else { OutError = TEXT("unknown parameter type: ") + TypeStr; return false; }
		if (!Ref.IsEmpty() && !OutTypeObject) { OutError = TEXT("type object not found: ") + Ref; return false; }
		if ((Kind == TEXT("enum") || Kind == TEXT("struct") || Kind == TEXT("object") || Kind == TEXT("class")) && !OutTypeObject)
		{
			OutError = TEXT("type needs a path, e.g. object:/Script/Engine.Actor"); return false;
		}
		return true;
	}
}

FString UMCPInspectorStateTreeToolset::ListStateTreeBindingSources(const FString& TreePath, const FString& TargetNodeId)
{
	using namespace MCPInspectorStateTreeToolset;
	FString Error;
	UStateTree* ST = nullptr;
	UStateTreeEditorData* ED = nullptr;
	if (!LoadTreeAndData(TreePath, ST, ED, Error)) return Err(Error);
	FFound Found;
	if (!FindById(ED, TargetNodeId, Found, Error)) return Err(Error);
	if (!Found.Node) return Err(TEXT("target id is not a node (task/condition/evaluator): ") + TargetNodeId);

	TArray<TInstancedStruct<FPropertyBindingBindableStructDescriptor>> Sources;
	GetBindableSources(ED, Found.Node->ID, Sources);

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const TInstancedStruct<FPropertyBindingBindableStructDescriptor>& Src : Sources)
	{
		const FPropertyBindingBindableStructDescriptor* Desc = Src.GetPtr<FPropertyBindingBindableStructDescriptor>();
		if (!Desc || !Desc->ID.IsValid()) continue;
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), IdStr(Desc->ID));
		O->SetStringField(TEXT("name"), Desc->Name.ToString());
		O->SetStringField(TEXT("struct"), Desc->Struct ? Desc->Struct->GetPathName() : FString());
		TArray<TSharedPtr<FJsonValue>> Props;
		if (Desc->Struct)
		{
			for (TFieldIterator<FProperty> It(Desc->Struct); It; ++It)
			{
				TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("name"), It->GetName());
				P->SetStringField(TEXT("type"), It->GetCPPType());
				Props.Add(MakeShared<FJsonValueObject>(P));
			}
		}
		O->SetArrayField(TEXT("properties"), Props);
		Arr.Add(MakeShared<FJsonValueObject>(O));
	}
	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Arr, Writer);
	return Out;
}


FString UMCPInspectorStateTreeToolset::UnbindStateTreeProperty(const FString& TreePath, const FString& TargetNodeId, const FString& TargetPropertyPath)
{
	using namespace MCPInspectorStateTreeToolset;
	FString Error;
	UStateTree* ST = nullptr;
	UStateTreeEditorData* ED = nullptr;
	if (!LoadTreeAndData(TreePath, ST, ED, Error)) return Err(Error);
	FFound Found;
	if (!FindById(ED, TargetNodeId, Found, Error)) return Err(Error);
	if (!Found.Node) return Err(TEXT("target id is not a node: ") + TargetNodeId);

	FPropertyBindingPath TargetPath;
	TargetPath.SetStructID(Found.Node->ID);
	if (!TargetPath.FromString(TargetPropertyPath)) return Err(TEXT("invalid target property path: ") + TargetPropertyPath);

	const FScopedTransaction Transaction(LOCTEXT("UnbindProperty", "BPI: Unbind StateTree Property"));
	ED->Modify();
	ED->RemovePropertyBinding(TargetPath);
	Commit(ST);
	return TEXT("OK");
}

FString UMCPInspectorStateTreeToolset::AddStateTreeParameter(const FString& TreePath, const FString& Name, const FString& Type)
{
	using namespace MCPInspectorStateTreeToolset;
	FString Error;
	UStateTree* ST = nullptr;
	UStateTreeEditorData* ED = nullptr;
	if (!LoadTreeAndData(TreePath, ST, ED, Error)) return Err(Error);
	if (Name.TrimStartAndEnd().IsEmpty()) return Err(TEXT("empty parameter name"));

	FInstancedPropertyBag* Bag = GetRootParameterBag(ED);
	if (!Bag) return Err(TEXT("root parameter bag not accessible (engine layout changed)"));
	if (Bag->FindPropertyDescByName(FName(*Name))) return Err(TEXT("parameter already exists: ") + Name);

	EPropertyBagPropertyType BagType;
	UObject* TypeObject = nullptr;
	if (!ParseBagType(Type, BagType, TypeObject, Error)) return Err(Error);

	const FScopedTransaction Transaction(LOCTEXT("AddParameter", "BPI: Add StateTree Parameter"));
	ED->Modify();
	const EPropertyBagAlterationResult Result = Bag->AddProperty(FName(*Name), BagType, TypeObject, /*bOverwrite*/ false);
	if (Result != EPropertyBagAlterationResult::Success) return Err(TEXT("AddProperty failed"));
	Commit(ST);
	return IdStr(ED->GetRootParametersGuid());
}

#undef LOCTEXT_NAMESPACE
