#include "MCPInspectorMVVMToolset.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "BPInspectorCommon.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "K2Node.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "MVVMBlueprintFunctionReference.h"
#include "MVVMBlueprintInstancedViewModel.h"
#include "MVVMBlueprintPin.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"
#include "MVVMBlueprintViewCondition.h"
#include "MVVMBlueprintViewConversionFunction.h"
#include "MVVMBlueprintViewEvent.h"
#include "MVVMBlueprintViewModelContext.h"
#include "MVVMDeveloperProjectSettings.h"
#include "MVVMEditorSubsystem.h"
#include "MVVMPropertyPath.h"
#include "MVVMViewModelBase.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Types/MVVMBindingMode.h"
#include "Types/MVVMConditionOperation.h"
#include "Types/MVVMExecutionMode.h"
#include "Types/MVVMFieldVariant.h"
#include "UObject/UnrealType.h"
#include "View/MVVMViewModelContextResolver.h"
#include "WidgetBlueprint.h"

#define LOCTEXT_NAMESPACE "MCPInspectorMVVMToolset"

namespace MCPInspectorMVVMToolset
{
	static FString Err(const FString& Msg) { return TEXT("ERROR|") + Msg; }

	static FString IdStr(const FGuid& Guid) { return Guid.ToString(EGuidFormats::DigitsWithHyphens); } // same text form as the official toolset's GuidConverter

	static FString ToJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	static FString ToObjectPath(const FString& Path)
	{
		return Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPackageName::GetShortName(Path);
	}

	template <typename T>
	static T* LoadAsset(const FString& InPath)
	{
		const FString P = InPath.TrimStartAndEnd();
		if (P.IsEmpty())
		{
			return nullptr;
		}
		if (T* Found = BPICommon::LoadTypedAsset<T>(ToObjectPath(P)))
		{
			return Found;
		}
		return BPICommon::LoadTypedAsset<T>(P);
	}

	static UMVVMEditorSubsystem* GetSubsystem()
	{
		return GEditor ? GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>() : nullptr;
	}

	/** Class used to resolve "Self" fields: the skeleton class knows members that were added but not compiled yet. */
	static UClass* GetSelfClass(const UBlueprint* BP)
	{
		UClass* Class = BP->SkeletonGeneratedClass;
		if (!Class)
		{
			Class = BP->GeneratedClass;
		}
		return Class;
	}

	struct FWidgetCtx
	{
		UWidgetBlueprint* WB = nullptr;
		UMVVMEditorSubsystem* Sub = nullptr;
		UMVVMBlueprintView* View = nullptr;
	};

	/** bCreateView = UMVVMEditorSubsystem::RequestView (creates the extension/view), otherwise GetView (no mutation). */
	static bool LoadWidget(const FString& WidgetPath, FWidgetCtx& Out, FString& OutError, bool bRequireView, bool bCreateView = false)
	{
		Out.WB = LoadAsset<UWidgetBlueprint>(WidgetPath);
		if (!Out.WB)
		{
			OutError = TEXT("widget blueprint not found: ") + WidgetPath;
			return false;
		}
		Out.Sub = GetSubsystem();
		if (!Out.Sub)
		{
			OutError = TEXT("UMVVMEditorSubsystem not available (is the UMG Viewmodel plugin enabled?)");
			return false;
		}
		Out.View = bCreateView ? Out.Sub->RequestView(Out.WB) : Out.Sub->GetView(Out.WB);
		if (!Out.View && bRequireView)
		{
			OutError = TEXT("widget has no MVVM view (no viewmodels or bindings yet): ") + WidgetPath;
			return false;
		}
		return true;
	}

	static FMVVMBlueprintViewBinding* FindBinding(UMVVMBlueprintView* View, const FString& BindingId, FString& OutError)
	{
		FGuid Id;
		if (!FGuid::Parse(BindingId.TrimStartAndEnd(), Id))
		{
			OutError = TEXT("invalid binding id: ") + BindingId;
			return nullptr;
		}
		FMVVMBlueprintViewBinding* Binding = View->GetBinding(Id);
		if (!Binding)
		{
			OutError = TEXT("binding not found: ") + BindingId;
		}
		return Binding;
	}

	/** Viewmodel by name, then by id, then by class (name, name without _C, class path, generating Blueprint path). */
	static const FMVVMBlueprintViewModelContext* FindViewModelContext(const UMVVMBlueprintView* View, const FString& InSpec, FString& OutError)
	{
		const FString Spec = InSpec.TrimStartAndEnd();
		if (Spec.IsEmpty() || !View)
		{
			OutError = TEXT("viewmodel not found: ") + InSpec;
			return nullptr;
		}
		if (const FMVVMBlueprintViewModelContext* ByName = View->FindViewModel(FName(*Spec)))
		{
			return ByName;
		}
		FGuid Id;
		if (FGuid::Parse(Spec, Id))
		{
			if (const FMVVMBlueprintViewModelContext* ById = View->FindViewModel(Id))
			{
				return ById;
			}
		}
		const FMVVMBlueprintViewModelContext* Match = nullptr;
		int32 NumMatches = 0;
		for (const FMVVMBlueprintViewModelContext& Context : View->GetViewModels())
		{
			const UClass* Class = Context.GetViewModelClass();
			if (!Class)
			{
				continue;
			}
			FString ShortName = Class->GetName();
			ShortName.RemoveFromEnd(TEXT("_C"));
			const bool bMatch = Spec.Equals(Class->GetName(), ESearchCase::IgnoreCase)
				|| Spec.Equals(ShortName, ESearchCase::IgnoreCase)
				|| Spec.Equals(Class->GetPathName(), ESearchCase::IgnoreCase)
				|| (Class->ClassGeneratedBy && (Spec.Equals(Class->ClassGeneratedBy->GetPathName(), ESearchCase::IgnoreCase)
					|| Spec.Equals(Class->ClassGeneratedBy->GetPackage()->GetName(), ESearchCase::IgnoreCase)));
			if (bMatch)
			{
				Match = &Context;
				++NumMatches;
			}
		}
		if (NumMatches == 1)
		{
			return Match;
		}
		OutError = NumMatches > 1
			? FString::Printf(TEXT("'%s' matches %d viewmodels by class; use the viewmodel name"), *Spec, NumMatches)
			: TEXT("viewmodel not found: ") + Spec;
		return nullptr;
	}

	// ---- path parsing ("<Context>.<Field>[.<Field>...]") -----------------------------------------------

	static UStruct* NextStruct(const FProperty* Property)
	{
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
		{
			return StructProp->Struct;
		}
		if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property))
		{
			return ObjProp->PropertyClass;
		}
		return nullptr;
	}

	/**
	 * Builds a FMVVMBlueprintPropertyPath like the official MVVMToolset GetPropertyPath (SetSelfContext / SetWidgetName /
	 * SetViewModelId + AppendPropertyPath), but appends every segment (the official one appends only the leaf) and accepts functions.
	 */
	static bool ParsePropertyPath(UWidgetBlueprint* WB, const UMVVMBlueprintView* View, const FString& InSpec, FMVVMBlueprintPropertyPath& OutPath, FString& OutError, bool* bOutLastIsFunction = nullptr)
	{
		TArray<FString> Parts;
		InSpec.TrimStartAndEnd().ParseIntoArray(Parts, TEXT("."), true);
		if (Parts.Num() < 2)
		{
			OutError = FString::Printf(TEXT("invalid path '%s', expected <ViewModel|Widget|Self>.<Field>[.<Field>...]"), *InSpec);
			return false;
		}

		FMVVMBlueprintPropertyPath Result;
		UStruct* Current = nullptr;
		const FString ContextName = Parts[0].TrimStartAndEnd();
		FString VMError;
		if (ContextName.Equals(TEXT("Self"), ESearchCase::IgnoreCase))
		{
			Current = GetSelfClass(WB);
			Result.SetSelfContext();
		}
		else if (const FMVVMBlueprintViewModelContext* VM = View ? FindViewModelContext(View, ContextName, VMError) : nullptr)
		{
			Current = VM->GetViewModelClass();
			Result.SetViewModelId(VM->GetViewModelId());
		}
		else if (UWidget* Widget = WB->WidgetTree ? WB->WidgetTree->FindWidget(FName(*ContextName)) : nullptr)
		{
			Current = Widget->GetClass();
			Result.SetWidgetName(Widget->GetFName());
		}
		else
		{
			OutError = FString::Printf(TEXT("'%s' is not a viewmodel, a widget of the widget tree or 'Self'%s"), *ContextName,
				VMError.Contains(TEXT("matches")) ? *(TEXT(" (") + VMError + TEXT(")")) : TEXT(""));
			return false;
		}

		bool bLastIsFunction = false;
		for (int32 Index = 1; Index < Parts.Num(); ++Index)
		{
			const FName FieldName(*Parts[Index].TrimStartAndEnd());
			if (!Current)
			{
				OutError = FString::Printf(TEXT("cannot resolve '%s' in '%s': previous field is not a struct or object"), *FieldName.ToString(), *InSpec);
				return false;
			}
			if (const FProperty* Property = Current->FindPropertyByName(FieldName))
			{
				Result.AppendPropertyPath(WB, UE::MVVM::FMVVMConstFieldVariant(Property));
				Current = NextStruct(Property);
				bLastIsFunction = false;
				continue;
			}
			const UClass* CurrentClass = Cast<UClass>(Current);
			if (const UFunction* Function = CurrentClass ? CurrentClass->FindFunctionByName(FieldName) : nullptr)
			{
				Result.AppendPropertyPath(WB, UE::MVVM::FMVVMConstFieldVariant(Function));
				const FProperty* ReturnProperty = Function->GetReturnProperty();
				Current = ReturnProperty ? NextStruct(ReturnProperty) : nullptr;
				bLastIsFunction = true;
				continue;
			}
			OutError = FString::Printf(TEXT("field '%s' not found on '%s'"), *FieldName.ToString(), *Current->GetName());
			return false;
		}

		if (!Result.IsValid() || !Result.HasPaths())
		{
			OutError = TEXT("could not build a valid path from ") + InSpec;
			return false;
		}
		if (bOutLastIsFunction)
		{
			*bOutLastIsFunction = bLastIsFunction;
		}
		OutPath = Result;
		return true;
	}

	// ---- pre/post edit notifications (same as UE::MVVM::Private::OnBindingPreEditChange etc. in MVVMEditorSubsystem.cpp, which are not exported) ----

	struct FScopedPropertyEdit
	{
		UObject* Object = nullptr;
		FProperty* Leaf = nullptr;
		FEditPropertyChain Chain;

		/** OuterProperty may be null (property directly on the object). Private UPROPERTYs are found by name through reflection. */
		FScopedPropertyEdit(UObject* InObject, FProperty* OuterProperty, FProperty* InLeaf)
			: Object(InObject)
			, Leaf(InLeaf ? InLeaf : OuterProperty)
		{
			Object->SetFlags(RF_Transactional);
			if (Leaf)
			{
				if (OuterProperty && OuterProperty != Leaf)
				{
					Chain.AddTail(OuterProperty);
				}
				Chain.AddTail(Leaf);
				Chain.SetActivePropertyNode(Leaf);
				Object->PreEditChange(Chain); // calls Modify()
			}
			else
			{
				Object->Modify();
			}
		}

		~FScopedPropertyEdit()
		{
			if (Leaf)
			{
				FPropertyChangedEvent ChangeEvent(Leaf, EPropertyChangeType::ValueSet);
				FPropertyChangedChainEvent ChainEvent(Chain, ChangeEvent);
				Object->PostEditChangeChainProperty(ChainEvent);
			}
		}
	};

	static FProperty* ViewProperty(const TCHAR* Name) { return UMVVMBlueprintView::StaticClass()->FindPropertyByName(Name); }
	static FProperty* BindingProperty(FName Name) { return FMVVMBlueprintViewBinding::StaticStruct()->FindPropertyByName(Name); }
	static FProperty* ContextProperty(FName Name) { return FMVVMBlueprintViewModelContext::StaticStruct()->FindPropertyByName(Name); }

	// ---- enums ----------------------------------------------------------------------------------------

	static bool ParseBindingMode(const FString& In, EMVVMBindingMode& Out)
	{
		const FString S = In.TrimStartAndEnd().Replace(TEXT(" "), TEXT("")).ToLower();
		if (S == TEXT("onetimetodestination") || S == TEXT("onetimetowidget")) { Out = EMVVMBindingMode::OneTimeToDestination; return true; }
		if (S == TEXT("onewaytodestination") || S == TEXT("onewaytowidget") || S == TEXT("towidget")) { Out = EMVVMBindingMode::OneWayToDestination; return true; }
		if (S == TEXT("twoway")) { Out = EMVVMBindingMode::TwoWay; return true; }
		if (S == TEXT("onewaytosource") || S == TEXT("onewaytoviewmodel") || S == TEXT("toviewmodel")) { Out = EMVVMBindingMode::OneWayToSource; return true; }
		return false;
	}

	static FString BindingModeStr(EMVVMBindingMode Mode)
	{
		switch (Mode)
		{
		case EMVVMBindingMode::OneTimeToDestination: return TEXT("OneTimeToDestination");
		case EMVVMBindingMode::OneWayToDestination: return TEXT("OneWayToDestination");
		case EMVVMBindingMode::TwoWay: return TEXT("TwoWay");
		case EMVVMBindingMode::OneTimeToSource: return TEXT("OneTimeToSource");
		case EMVVMBindingMode::OneWayToSource: return TEXT("OneWayToSource");
		default: return TEXT("Unknown");
		}
	}

	static bool ParseExecutionMode(const FString& In, bool& bOutDefault, EMVVMExecutionMode& Out)
	{
		const FString S = In.TrimStartAndEnd().ToLower();
		bOutDefault = false;
		if (S.IsEmpty() || S == TEXT("default") || S == TEXT("none") || S == TEXT("reset")) { bOutDefault = true; return true; }
		if (S == TEXT("immediate")) { Out = EMVVMExecutionMode::Immediate; return true; }
		if (S == TEXT("delayed")) { Out = EMVVMExecutionMode::Delayed; return true; }
		if (S == TEXT("tick")) { Out = EMVVMExecutionMode::Tick; return true; }
		if (S == TEXT("delayedwhensharedelseimmediate") || S == TEXT("auto")) { Out = EMVVMExecutionMode::DelayedWhenSharedElseImmediate; return true; }
		return false;
	}

	static FString ExecutionModeStr(EMVVMExecutionMode Mode)
	{
		switch (Mode)
		{
		case EMVVMExecutionMode::Immediate: return TEXT("Immediate");
		case EMVVMExecutionMode::Delayed: return TEXT("Delayed");
		case EMVVMExecutionMode::Tick: return TEXT("Tick");
		case EMVVMExecutionMode::DelayedWhenSharedElseImmediate: return TEXT("DelayedWhenSharedElseImmediate");
		default: return TEXT("Unknown");
		}
	}

	static bool ParseCreationType(const FString& In, EMVVMBlueprintViewModelContextCreationType& Out)
	{
		const FString S = In.TrimStartAndEnd().ToLower();
		if (S == TEXT("manual")) { Out = EMVVMBlueprintViewModelContextCreationType::Manual; return true; }
		if (S == TEXT("createinstance")) { Out = EMVVMBlueprintViewModelContextCreationType::CreateInstance; return true; }
		if (S == TEXT("globalviewmodelcollection") || S == TEXT("global")) { Out = EMVVMBlueprintViewModelContextCreationType::GlobalViewModelCollection; return true; }
		if (S == TEXT("propertypath")) { Out = EMVVMBlueprintViewModelContextCreationType::PropertyPath; return true; }
		if (S == TEXT("resolver")) { Out = EMVVMBlueprintViewModelContextCreationType::Resolver; return true; }
		if (S == TEXT("context")) { Out = EMVVMBlueprintViewModelContextCreationType::Context; return true; }
		return false;
	}

	static FString CreationTypeStr(EMVVMBlueprintViewModelContextCreationType Type)
	{
		switch (Type)
		{
		case EMVVMBlueprintViewModelContextCreationType::Manual: return TEXT("Manual");
		case EMVVMBlueprintViewModelContextCreationType::CreateInstance: return TEXT("CreateInstance");
		case EMVVMBlueprintViewModelContextCreationType::GlobalViewModelCollection: return TEXT("GlobalViewModelCollection");
		case EMVVMBlueprintViewModelContextCreationType::PropertyPath: return TEXT("PropertyPath");
		case EMVVMBlueprintViewModelContextCreationType::Resolver: return TEXT("Resolver");
		case EMVVMBlueprintViewModelContextCreationType::Context: return TEXT("Context");
		default: return TEXT("Unknown");
		}
	}

	static bool ParseConditionOperation(const FString& In, EMVVMConditionOperation& Out)
	{
		const FString S = In.TrimStartAndEnd().ToLower();
		if (S == TEXT("equal") || S == TEXT("==")) { Out = EMVVMConditionOperation::Equal; return true; }
		if (S == TEXT("notequal") || S == TEXT("!=")) { Out = EMVVMConditionOperation::NotEqual; return true; }
		if (S == TEXT("morethan") || S == TEXT(">")) { Out = EMVVMConditionOperation::MoreThan; return true; }
		if (S == TEXT("morethanorequal") || S == TEXT(">=")) { Out = EMVVMConditionOperation::MoreThanOrEqual; return true; }
		if (S == TEXT("lessthan") || S == TEXT("<")) { Out = EMVVMConditionOperation::LessThan; return true; }
		if (S == TEXT("lessthanorequal") || S == TEXT("<=")) { Out = EMVVMConditionOperation::LessThanOrEqual; return true; }
		if (S == TEXT("betweeninclusive")) { Out = EMVVMConditionOperation::BetweenInclusive; return true; }
		if (S == TEXT("betweenexclusive")) { Out = EMVVMConditionOperation::BetweenExclusive; return true; }
		return false;
	}

	static FString SourceStr(EMVVMBlueprintFieldPathSource Source)
	{
		switch (Source)
		{
		case EMVVMBlueprintFieldPathSource::Widget: return TEXT("Widget");
		case EMVVMBlueprintFieldPathSource::ViewModel: return TEXT("ViewModel");
		case EMVVMBlueprintFieldPathSource::SelfContext: return TEXT("Self");
		default: return TEXT("None");
		}
	}

	static FString StatusStr(EBlueprintStatus Status)
	{
		switch (Status)
		{
		case BS_Dirty: return TEXT("Dirty");
		case BS_Error: return TEXT("Error");
		case BS_UpToDate: return TEXT("UpToDate");
		case BS_UpToDateWithWarnings: return TEXT("UpToDateWithWarnings");
		case BS_BeingCreated: return TEXT("BeingCreated");
		default: return TEXT("Unknown");
		}
	}

	// ---- JSON --------------------------------------------------------------------------------------------

	static TSharedRef<FJsonObject> PathToJson(UWidgetBlueprint* WB, const UMVVMBlueprintView* View, const FMVVMBlueprintPropertyPath& Path)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("valid"), Path.IsValid());
		if (!Path.IsValid())
		{
			return Obj;
		}
		const EMVVMBlueprintFieldPathSource Source = Path.GetSource(WB);
		Obj->SetStringField(TEXT("source"), SourceStr(Source));
		Obj->SetStringField(TEXT("text"), Path.ToString(WB, false, false));

		FString Spec;
		if (Source == EMVVMBlueprintFieldPathSource::ViewModel)
		{
			Obj->SetStringField(TEXT("viewModelId"), IdStr(Path.GetViewModelId()));
			const FMVVMBlueprintViewModelContext* VM = View ? View->FindViewModel(Path.GetViewModelId()) : nullptr;
			Spec = VM ? VM->GetViewModelName().ToString() : IdStr(Path.GetViewModelId());
			if (VM)
			{
				Obj->SetStringField(TEXT("viewModelName"), Spec);
			}
		}
		else if (Source == EMVVMBlueprintFieldPathSource::Widget)
		{
			Spec = Path.GetWidgetName().ToString();
			Obj->SetStringField(TEXT("widget"), Spec);
		}
		else
		{
			Spec = TEXT("Self");
		}

		TArray<TSharedPtr<FJsonValue>> Fields;
		for (const FName& Name : Path.GetFieldNames(GetSelfClass(WB)))
		{
			Fields.Add(MakeShared<FJsonValueString>(Name.ToString()));
			Spec += TEXT(".") + Name.ToString();
		}
		Obj->SetArrayField(TEXT("fields"), Fields);
		Obj->SetStringField(TEXT("spec"), Spec);
		return Obj;
	}

	static TSharedRef<FJsonObject> PinToJson(UWidgetBlueprint* WB, const UMVVMBlueprintView* View, const FMVVMBlueprintPin& Pin)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		const TArrayView<const FName> Names = Pin.GetId().GetNames();
		FString Name;
		for (const FName& N : Names)
		{
			Name += (Name.IsEmpty() ? TEXT("") : TEXT(".")) + N.ToString();
		}
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("id"), Pin.GetId().ToString());
		Obj->SetStringField(TEXT("value"), Pin.GetValueAsString(GetSelfClass(WB)));
		if (Pin.UsedPathAsValue())
		{
			Obj->SetObjectField(TEXT("path"), PathToJson(WB, View, Pin.GetPath()));
		}
		Obj->SetBoolField(TEXT("split"), Pin.IsSplit());
		Obj->SetBoolField(TEXT("orphaned"), Pin.GetStatus() == EMVVMBlueprintPinStatus::Orphaned);
		return Obj;
	}

	/** Saved pins plus, if the transient wrapper node already exists, its input pins that have no saved value yet. */
	static TArray<TSharedPtr<FJsonValue>> PinsToJson(UWidgetBlueprint* WB, const UMVVMBlueprintView* View, TArrayView<const FMVVMBlueprintPin> SavedPins, const UK2Node* WrapperNode)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		TSet<FName> Listed;
		for (const FMVVMBlueprintPin& Pin : SavedPins)
		{
			Out.Add(MakeShared<FJsonValueObject>(PinToJson(WB, View, Pin)));
			if (Pin.GetId().GetNames().Num() == 1)
			{
				Listed.Add(Pin.GetId().GetNames()[0]);
			}
		}
		if (WrapperNode)
		{
			for (const UEdGraphPin* GraphPin : WrapperNode->Pins)
			{
				if (GraphPin && FMVVMBlueprintPin::IsInputPin(GraphPin) && !Listed.Contains(GraphPin->PinName) && GraphPin->ParentPin == nullptr)
				{
					TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), GraphPin->PinName.ToString());
					Obj->SetStringField(TEXT("value"), GraphPin->GetDefaultAsString());
					Obj->SetBoolField(TEXT("linked"), GraphPin->LinkedTo.Num() > 0);
					Obj->SetBoolField(TEXT("split"), GraphPin->SubPins.Num() > 0);
					Obj->SetBoolField(TEXT("orphaned"), GraphPin->bOrphanedPin);
					Out.Add(MakeShared<FJsonValueObject>(Obj));
				}
			}
		}
		return Out;
	}

	static TSharedPtr<FJsonObject> ConversionToJson(UWidgetBlueprint* WB, const UMVVMBlueprintView* View, const UMVVMBlueprintViewConversionFunction* Conversion)
	{
		if (!Conversion)
		{
			return nullptr;
		}
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		const FMVVMBlueprintFunctionReference Ref = Conversion->GetConversionFunction();
		Obj->SetStringField(TEXT("function"), Ref.GetName().ToString());
		Obj->SetStringField(TEXT("reference"), Ref.ToString());
		switch (Ref.GetType())
		{
		case EMVVMBlueprintFunctionReferenceType::Function:
		{
			Obj->SetStringField(TEXT("type"), TEXT("Function"));
			if (const UFunction* Function = Ref.GetFunction(WB))
			{
				Obj->SetStringField(TEXT("owner"), Function->GetOwnerClass() ? Function->GetOwnerClass()->GetPathName() : FString());
			}
			break;
		}
		case EMVVMBlueprintFunctionReferenceType::Node:
		{
			Obj->SetStringField(TEXT("type"), TEXT("Node"));
			const TSubclassOf<UK2Node> NodeClass = Ref.GetNode();
			Obj->SetStringField(TEXT("nodeClass"), NodeClass.Get() ? NodeClass.Get()->GetPathName() : FString());
			break;
		}
		default:
			Obj->SetStringField(TEXT("type"), TEXT("None"));
			break;
		}
		Obj->SetBoolField(TEXT("valid"), Conversion->IsValid(WB));
		Obj->SetStringField(TEXT("wrapperGraph"), Conversion->GetWrapperGraphName().ToString());
		Obj->SetArrayField(TEXT("arguments"), PinsToJson(WB, View, Conversion->GetPins(), Conversion->GetWrapperNode()));
		return Obj;
	}

	static void AddMessages(TArray<TSharedPtr<FJsonValue>>& Out, const TArray<FText>& Texts, const TCHAR* Severity)
	{
		for (const FText& Text : Texts)
		{
			TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
			M->SetStringField(TEXT("severity"), Severity);
			M->SetStringField(TEXT("text"), Text.ToString());
			Out.Add(MakeShared<FJsonValueObject>(M));
		}
	}

	static TArray<TSharedPtr<FJsonValue>> BindingMessagesToJson(const UMVVMBlueprintView* View, const FGuid& Id)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		AddMessages(Out, View->GetBindingMessages(Id, UE::MVVM::EBindingMessageType::Error), TEXT("Error"));
		AddMessages(Out, View->GetBindingMessages(Id, UE::MVVM::EBindingMessageType::Warning), TEXT("Warning"));
		AddMessages(Out, View->GetBindingMessages(Id, UE::MVVM::EBindingMessageType::Info), TEXT("Info"));
		return Out;
	}

	static TSharedRef<FJsonObject> ViewModelToJson(const FMVVMBlueprintViewModelContext& Context)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Context.GetViewModelName().ToString());
		Obj->SetStringField(TEXT("id"), IdStr(Context.GetViewModelId()));
		Obj->SetStringField(TEXT("class"), Context.GetViewModelClass() ? Context.GetViewModelClass()->GetPathName() : FString());
		Obj->SetStringField(TEXT("creationType"), CreationTypeStr(Context.CreationType));
		Obj->SetStringField(TEXT("globalViewModelIdentifier"), Context.GlobalViewModelIdentifier.ToString());
		Obj->SetStringField(TEXT("propertyPath"), Context.ViewModelPropertyPath);
		Obj->SetStringField(TEXT("resolver"), Context.Resolver ? Context.Resolver->GetClass()->GetPathName() : FString());
		Obj->SetBoolField(TEXT("instanced"), Context.InstancedViewModel.Get() != nullptr);
		Obj->SetBoolField(TEXT("exposeInstanceInEditor"), Context.bExposeInstanceInEditor);
		Obj->SetBoolField(TEXT("createSetterFunction"), Context.bCreateSetterFunction);
		Obj->SetBoolField(TEXT("createGetterFunction"), Context.bCreateGetterFunction);
		Obj->SetBoolField(TEXT("optional"), Context.bOptional);
		Obj->SetBoolField(TEXT("globalViewModelCollectionUpdate"), Context.bGlobalViewModelCollectionUpdate);
		Obj->SetBoolField(TEXT("useAsInterface"), Context.bUseAsInterface);
		Obj->SetBoolField(TEXT("canRename"), Context.CanRename());
		Obj->SetBoolField(TEXT("canEdit"), Context.bCanEdit);
		Obj->SetBoolField(TEXT("canRemove"), Context.bCanRemove);
		TArray<TSharedPtr<FJsonValue>> Allowed;
		if (Context.GetViewModelClass())
		{
			for (EMVVMBlueprintViewModelContextCreationType Type : UE::MVVM::GetAllowedContextCreationType(Context.GetViewModelClass()))
			{
				Allowed.Add(MakeShared<FJsonValueString>(CreationTypeStr(Type)));
			}
		}
		Obj->SetArrayField(TEXT("allowedCreationTypes"), Allowed);
		return Obj;
	}

	static TSharedRef<FJsonObject> EventToJson(UWidgetBlueprint* WB, const UMVVMBlueprintView* View, const UMVVMBlueprintViewEvent* Event)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Event->GetName());
		Obj->SetObjectField(TEXT("eventPath"), PathToJson(WB, View, Event->GetEventPath()));
		Obj->SetObjectField(TEXT("destination"), PathToJson(WB, View, Event->GetDestinationPath()));
		Obj->SetBoolField(TEXT("enabled"), Event->bEnabled);
		Obj->SetBoolField(TEXT("compile"), Event->bCompile);
		Obj->SetArrayField(TEXT("arguments"), PinsToJson(WB, View, Event->GetPins(), Event->GetWrapperNode()));
		TArray<TSharedPtr<FJsonValue>> Messages;
		AddMessages(Messages, Event->GetCompilationMessages(UMVVMBlueprintViewEvent::EMessageType::Error), TEXT("Error"));
		AddMessages(Messages, Event->GetCompilationMessages(UMVVMBlueprintViewEvent::EMessageType::Warning), TEXT("Warning"));
		AddMessages(Messages, Event->GetCompilationMessages(UMVVMBlueprintViewEvent::EMessageType::Info), TEXT("Info"));
		Obj->SetArrayField(TEXT("messages"), Messages);
		return Obj;
	}

	static TSharedRef<FJsonObject> ConditionToJson(UWidgetBlueprint* WB, const UMVVMBlueprintView* View, const UMVVMBlueprintViewCondition* Condition)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Condition->GetName());
		Obj->SetObjectField(TEXT("conditionPath"), PathToJson(WB, View, Condition->GetConditionPath()));
		Obj->SetStringField(TEXT("operation"), LexToString(Condition->GetOperation()));
		Obj->SetNumberField(TEXT("value"), Condition->GetOperationValue());
		Obj->SetNumberField(TEXT("maxValue"), Condition->GetOperationMaxValue());
		Obj->SetObjectField(TEXT("destination"), PathToJson(WB, View, Condition->GetDestinationPath()));
		Obj->SetBoolField(TEXT("enabled"), Condition->bEnabled);
		Obj->SetBoolField(TEXT("compile"), Condition->bCompile);
		Obj->SetArrayField(TEXT("arguments"), PinsToJson(WB, View, Condition->GetPins(), nullptr));
		TArray<TSharedPtr<FJsonValue>> Messages;
		AddMessages(Messages, Condition->GetCompilationMessages(UMVVMBlueprintViewCondition::EMessageType::Error), TEXT("Error"));
		AddMessages(Messages, Condition->GetCompilationMessages(UMVVMBlueprintViewCondition::EMessageType::Warning), TEXT("Warning"));
		AddMessages(Messages, Condition->GetCompilationMessages(UMVVMBlueprintViewCondition::EMessageType::Info), TEXT("Info"));
		Obj->SetArrayField(TEXT("messages"), Messages);
		return Obj;
	}

	// ---- pins ---------------------------------------------------------------------------------------------

	/** "path:<spec>" => bind to a path ("path:" alone = unbind), "value:<v>" or anything else => literal. */
	static void SplitValueOrPath(const FString& In, bool& bOutIsPath, FString& OutText)
	{
		bOutIsPath = false;
		OutText = In;
		if (In.StartsWith(TEXT("path:"), ESearchCase::IgnoreCase))
		{
			bOutIsPath = true;
			OutText = In.Mid(5).TrimStartAndEnd();
		}
		else if (In.StartsWith(TEXT("value:"), ESearchCase::IgnoreCase))
		{
			OutText = In.Mid(6);
		}
	}

	/** Find the saved pin id by its full id text or by its single name; otherwise build one from "A.B" (validated by the caller). */
	static FMVVMBlueprintPinId ResolvePinId(TArrayView<const FMVVMBlueprintPin> SavedPins, const FString& ArgumentName)
	{
		const FString Arg = ArgumentName.TrimStartAndEnd();
		for (const FMVVMBlueprintPin& Pin : SavedPins)
		{
			if (Pin.GetId().ToString().Equals(Arg, ESearchCase::IgnoreCase))
			{
				return Pin.GetId();
			}
		}
		for (const FMVVMBlueprintPin& Pin : SavedPins)
		{
			const TArrayView<const FName> Names = Pin.GetId().GetNames();
			if (Names.Num() == 1 && Names[0].ToString().Equals(Arg, ESearchCase::IgnoreCase))
			{
				return Pin.GetId();
			}
		}
		TArray<FString> Parts;
		Arg.ParseIntoArray(Parts, TEXT("."), true);
		TArray<FName> Names;
		for (const FString& Part : Parts)
		{
			Names.Add(FName(*Part));
		}
		return FMVVMBlueprintPinId(MoveTemp(Names));
	}

	/** Same conversion as SGraphPin editing: GetPinDefaultValuesFromString + IsPinDefaultValid + TrySetDefaultValue. */
	static bool SetPinLiteral(UEdGraphPin* GraphPin, const FString& Value, FString& OutError)
	{
		const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
		FString UseDefaultValue;
		TObjectPtr<UObject> UseDefaultObject = nullptr;
		FText UseDefaultText;
		K2Schema->GetPinDefaultValuesFromString(GraphPin->PinType, GraphPin->GetOwningNode(), Value, UseDefaultValue, UseDefaultObject, UseDefaultText);
		const FString Invalid = K2Schema->IsPinDefaultValid(GraphPin, UseDefaultValue, UseDefaultObject, UseDefaultText);
		if (!Invalid.IsEmpty())
		{
			OutError = FString::Printf(TEXT("invalid value '%s' for argument '%s': %s"), *Value, *GraphPin->PinName.ToString(), *Invalid);
			return false;
		}
		K2Schema->TrySetDefaultValue(*GraphPin, Value, /*bMarkAsModified*/ true);
		return true;
	}

	// ---- references to a viewmodel -------------------------------------------------------------------------

	static bool PinsUseViewModel(TArrayView<const FMVVMBlueprintPin> Pins, const FGuid& ViewModelId)
	{
		for (const FMVVMBlueprintPin& Pin : Pins)
		{
			if (Pin.UsedPathAsValue() && Pin.GetPath().GetViewModelId() == ViewModelId)
			{
				return true;
			}
		}
		return false;
	}

	static bool BindingUsesViewModel(const FMVVMBlueprintViewBinding& Binding, const FGuid& ViewModelId)
	{
		if (Binding.SourcePath.GetViewModelId() == ViewModelId || Binding.DestinationPath.GetViewModelId() == ViewModelId)
		{
			return true;
		}
		for (bool bForward : { true, false })
		{
			if (const UMVVMBlueprintViewConversionFunction* Conversion = Binding.Conversion.GetConversionFunction(bForward))
			{
				if (PinsUseViewModel(Conversion->GetPins(), ViewModelId))
				{
					return true;
				}
			}
		}
		return false;
	}

	static UMVVMBlueprintViewEvent* FindEvent(UMVVMBlueprintView* View, const FString& Id)
	{
		for (const TObjectPtr<UMVVMBlueprintViewEvent>& Event : View->GetEvents())
		{
			if (Event && Event->GetName().Equals(Id.TrimStartAndEnd(), ESearchCase::IgnoreCase))
			{
				return Event;
			}
		}
		return nullptr;
	}

	static UMVVMBlueprintViewCondition* FindCondition(UMVVMBlueprintView* View, const FString& Id)
	{
		for (const TObjectPtr<UMVVMBlueprintViewCondition>& Condition : View->GetConditions())
		{
			if (Condition && Condition->GetName().Equals(Id.TrimStartAndEnd(), ESearchCase::IgnoreCase))
			{
				return Condition;
			}
		}
		return nullptr;
	}

	/** Resolver class from "/Script/Module.Class", a class object path or a Blueprint asset path. */
	static UClass* LoadClassArg(const FString& InPath)
	{
		const FString P = InPath.TrimStartAndEnd();
		if (P.IsEmpty())
		{
			return nullptr;
		}
		if (UClass* Class = LoadObject<UClass>(nullptr, *P))
		{
			return Class;
		}
		if (UBlueprint* BP = LoadAsset<UBlueprint>(P))
		{
			return BP->GeneratedClass;
		}
		return nullptr;
	}

	static UBlueprint* LoadViewModelBlueprint(const FString& Path, FString& OutError)
	{
		UBlueprint* BP = LoadAsset<UBlueprint>(Path);
		if (!BP)
		{
			OutError = TEXT("blueprint not found: ") + Path;
			return nullptr;
		}
		if (!BP->ParentClass || !BP->ParentClass->IsChildOf(UMVVMViewModelBase::StaticClass()))
		{
			OutError = TEXT("not a viewmodel blueprint (parent class must derive from MVVMViewModelBase): ") + Path;
			return nullptr;
		}
		return BP;
	}
}


// ============================================================================================================
// bindings
// ============================================================================================================

FString UMCPInspectorMVVMToolset::SetBindingMode(const FString& WidgetPath, const FString& BindingId, const FString& Mode)
{
	using namespace MCPInspectorMVVMToolset;
	FWidgetCtx Ctx;
	FString Error;
	if (!LoadWidget(WidgetPath, Ctx, Error, true))
	{
		return Err(Error);
	}
	FMVVMBlueprintViewBinding* Binding = FindBinding(Ctx.View, BindingId, Error);
	if (!Binding)
	{
		return Err(Error);
	}
	EMVVMBindingMode NewMode = EMVVMBindingMode::OneWayToDestination;
	if (!ParseBindingMode(Mode, NewMode))
	{
		return Err(TEXT("invalid mode '") + Mode + TEXT("' (OneTimeToDestination, OneWayToDestination, TwoWay, OneWayToSource; OneTimeToSource is hidden in the editor and not supported)"));
	}
	Ctx.Sub->SetBindingTypeForBinding(Ctx.WB, *Binding, NewMode); // own transaction + pre/post edit + MarkBlueprintAsModified
	return Binding->BindingType == NewMode ? FString(TEXT("OK")) : Err(TEXT("binding mode was not changed"));
}

FString UMCPInspectorMVVMToolset::SetBindingExecutionMode(const FString& WidgetPath, const FString& BindingId, const FString& ExecutionMode)
{
	using namespace MCPInspectorMVVMToolset;
	FWidgetCtx Ctx;
	FString Error;
	if (!LoadWidget(WidgetPath, Ctx, Error, true))
	{
		return Err(Error);
	}
	FMVVMBlueprintViewBinding* Binding = FindBinding(Ctx.View, BindingId, Error);
	if (!Binding)
	{
		return Err(Error);
	}
	bool bDefault = false;
	EMVVMExecutionMode NewMode = EMVVMExecutionMode::Immediate;
	if (!ParseExecutionMode(ExecutionMode, bDefault, NewMode))
	{
		return Err(TEXT("invalid execution mode '") + ExecutionMode + TEXT("' (Default, Immediate, Delayed, Tick, DelayedWhenSharedElseImmediate)"));
	}
	if (bDefault)
	{
		Ctx.Sub->ResetExecutionModeForBinding(Ctx.WB, *Binding);
		return Binding->bOverrideExecutionMode ? Err(TEXT("execution mode override was not reset")) : FString(TEXT("OK"));
	}
	if (!GetDefault<UMVVMDeveloperProjectSettings>()->IsExecutionModeAllowed(NewMode))
	{
		return Err(TEXT("execution mode ") + ExecutionModeStr(NewMode) + TEXT(" is not allowed by the project MVVM settings (Allowed Execution Mode)"));
	}
	Ctx.Sub->OverrideExecutionModeForBinding(Ctx.WB, *Binding, NewMode);
	return (Binding->bOverrideExecutionMode && Binding->OverrideExecutionMode == NewMode) ? FString(TEXT("OK")) : Err(TEXT("execution mode was not changed"));
}

FString UMCPInspectorMVVMToolset::SetBindingEnabled(const FString& WidgetPath, const FString& BindingId, bool bEnabled, bool bCompile)
{
	using namespace MCPInspectorMVVMToolset;
	FWidgetCtx Ctx;
	FString Error;
	if (!LoadWidget(WidgetPath, Ctx, Error, true))
	{
		return Err(Error);
	}
	FMVVMBlueprintViewBinding* Binding = FindBinding(Ctx.View, BindingId, Error);
	if (!Binding)
	{
		return Err(Error);
	}
	const FScopedTransaction Transaction(LOCTEXT("SetBindingEnabled", "Set Binding Enabled/Compile"));
	Ctx.Sub->SetEnabledForBinding(Ctx.WB, *Binding, bEnabled);
	Ctx.Sub->SetCompileForBinding(Ctx.WB, *Binding, bCompile);
	return (Binding->bEnabled == bEnabled && Binding->bCompile == bCompile) ? FString(TEXT("OK")) : Err(TEXT("flags were not changed"));
}

FString UMCPInspectorMVVMToolset::UpdateBindingPaths(const FString& WidgetPath, const FString& BindingId, const FString& SourcePath, const FString& DestinationPath)
{
	using namespace MCPInspectorMVVMToolset;
	FWidgetCtx Ctx;
	FString Error;
	if (!LoadWidget(WidgetPath, Ctx, Error, true))
	{
		return Err(Error);
	}
	FMVVMBlueprintViewBinding* Binding = FindBinding(Ctx.View, BindingId, Error);
	if (!Binding)
	{
		return Err(Error);
	}
	const bool bSetSource = !SourcePath.TrimStartAndEnd().IsEmpty();
	const bool bSetDestination = !DestinationPath.TrimStartAndEnd().IsEmpty();
	if (!bSetSource && !bSetDestination)
	{
		return Err(TEXT("nothing to change: SourcePath and DestinationPath are both empty"));
	}

	FMVVMBlueprintPropertyPath NewSource;
	FMVVMBlueprintPropertyPath NewDestination;
	if (bSetSource)
	{
		if (Binding->Conversion.SourceToDestinationConversion)
		{
			return Err(TEXT("the source side is driven by the source->destination conversion function; change its argument with SetConversionFunctionArgument (bSourceToDestination=true)"));
		}
		if (!ParsePropertyPath(Ctx.WB, Ctx.View, SourcePath, NewSource, Error))
		{
			return Err(TEXT("source: ") + Error);
		}
	}
	if (bSetDestination)
	{
		if (Binding->Conversion.DestinationToSourceConversion)
		{
			return Err(TEXT("the destination side is driven by the destination->source conversion function; change its argument with SetConversionFunctionArgument (bSourceToDestination=false)"));
		}
		if (!ParsePropertyPath(Ctx.WB, Ctx.View, DestinationPath, NewDestination, Error))
		{
			return Err(TEXT("destination: ") + Error);
		}
	}

	{
		const FScopedTransaction Transaction(LOCTEXT("UpdateBindingPaths", "Update Binding Paths"));
		if (bSetSource)
		{
			Ctx.Sub->SetSourcePathForBinding(Ctx.WB, *Binding, NewSource);
		}
		if (bSetDestination)
		{
			// bAllowEventConversion = false: never turn the binding into a view event (that would delete the binding).
			Ctx.Sub->SetDestinationPathForBinding(Ctx.WB, *Binding, NewDestination, false);
		}
	}

	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("id"), IdStr(Binding->BindingId));
	Obj->SetObjectField(TEXT("source"), PathToJson(Ctx.WB, Ctx.View, Binding->SourcePath));
	Obj->SetObjectField(TEXT("destination"), PathToJson(Ctx.WB, Ctx.View, Binding->DestinationPath));
	return ToJson(Obj);
}

FString UMCPInspectorMVVMToolset::SetConversionFunctionArgument(const FString& WidgetPath, const FString& BindingId, const FString& ArgumentName, const FString& ValueOrPath, bool bSourceToDestination)
{
	using namespace MCPInspectorMVVMToolset;
	FWidgetCtx Ctx;
	FString Error;
	if (!LoadWidget(WidgetPath, Ctx, Error, true))
	{
		return Err(Error);
	}
	FMVVMBlueprintViewBinding* Binding = FindBinding(Ctx.View, BindingId, Error);
	if (!Binding)
	{
		return Err(Error);
	}
	UMVVMBlueprintViewConversionFunction* Conversion = Binding->Conversion.GetConversionFunction(bSourceToDestination);
	if (!Conversion)
	{
		return Err(FString::Printf(TEXT("binding has no %s conversion function"), bSourceToDestination ? TEXT("source->destination") : TEXT("destination->source")));
	}

	const FMVVMBlueprintPinId PinId = ResolvePinId(Conversion->GetPins(), ArgumentName);
	if (!PinId.IsValid())
	{
		return Err(TEXT("invalid argument name: ") + ArgumentName);
	}
	// Creates the transient wrapper graph if needed; must be non-null before SetPathForConversionFunctionArgument (it check()s the pin).
	UEdGraphPin* GraphPin = Ctx.Sub->GetConversionFunctionArgumentPin(Ctx.WB, *Binding, PinId, bSourceToDestination);
	if (!GraphPin || !FMVVMBlueprintPin::IsInputPin(GraphPin))
	{
		return Err(TEXT("argument not found on the conversion function: ") + ArgumentName + TEXT(" (see DumpWidgetMVVM conversion.arguments)"));
	}

	bool bIsPath = false;
	FString Text;
	SplitValueOrPath(ValueOrPath, bIsPath, Text);

	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("argument"), PinId.ToString());

	if (bIsPath)
	{
		FMVVMBlueprintPropertyPath Path; // empty = unbind
		if (!Text.IsEmpty() && !ParsePropertyPath(Ctx.WB, Ctx.View, Text, Path, Error))
		{
			return Err(Error);
		}
		Ctx.Sub->SetPathForConversionFunctionArgument(Ctx.WB, *Binding, PinId, Path, bSourceToDestination);
		const FMVVMBlueprintPropertyPath Result = Ctx.Sub->GetPathForConversionFunctionArgument(Ctx.WB, *Binding, PinId, bSourceToDestination);
		Obj->SetObjectField(TEXT("path"), PathToJson(Ctx.WB, Ctx.View, Result));
		return ToJson(Obj);
	}

	const FScopedTransaction Transaction(LOCTEXT("SetConversionArgument", "Set Conversion Function Argument"));
	if (GraphPin->LinkedTo.Num() > 0)
	{
		// The argument is bound to a path: unbind first (removes the getter nodes), like picking a literal in the editor.
		Ctx.Sub->SetPathForConversionFunctionArgument(Ctx.WB, *Binding, PinId, FMVVMBlueprintPropertyPath(), bSourceToDestination);
		GraphPin = Ctx.Sub->GetConversionFunctionArgumentPin(Ctx.WB, *Binding, PinId, bSourceToDestination);
		if (!GraphPin)
		{
			return Err(TEXT("argument pin disappeared after unbinding"));
		}
	}
	{
		FScopedPropertyEdit Edit(Ctx.View, ViewProperty(TEXT("Bindings")), BindingProperty(GET_MEMBER_NAME_CHECKED(FMVVMBlueprintViewBinding, Conversion)));
		Conversion->Modify();
		if (!SetPinLiteral(GraphPin, Text, Error))
		{
			return Err(Error);
		}
		Conversion->SavePinValues(Ctx.WB);
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(Ctx.WB);
	Obj->SetStringField(TEXT("value"), GraphPin->GetDefaultAsString());
	return ToJson(Obj);
}

// ============================================================================================================
// viewmodels on a widget
// ============================================================================================================

FString UMCPInspectorMVVMToolset::RemoveViewModelFromWidget(const FString& WidgetPath, const FString& ViewModelName, bool bRemoveReferencingBindings)
{
	using namespace MCPInspectorMVVMToolset;
	FWidgetCtx Ctx;
	FString Error;
	if (!LoadWidget(WidgetPath, Ctx, Error, true))
	{
		return Err(Error);
	}
	const FMVVMBlueprintViewModelContext* VM = FindViewModelContext(Ctx.View, ViewModelName, Error);
	if (!VM)
	{
		return Err(Error);
	}
	if (!VM->bCanRemove)
	{
		return Err(TEXT("viewmodel cannot be removed (bCanRemove=false): ") + VM->GetViewModelName().ToString());
	}
	const FGuid VMId = VM->GetViewModelId();
	const FName VMName = VM->GetViewModelName();

	TArray<FGuid> BindingIds;
	TArray<TSharedPtr<FJsonValue>> BindingsJson;
	for (const FMVVMBlueprintViewBinding& Binding : Ctx.View->GetBindings())
	{
		if (BindingUsesViewModel(Binding, VMId))
		{
			BindingIds.Add(Binding.BindingId);
			TSharedRef<FJsonObject> B = MakeShared<FJsonObject>();
			B->SetStringField(TEXT("id"), IdStr(Binding.BindingId));
			B->SetStringField(TEXT("text"), Binding.GetDisplayNameString(Ctx.WB));
			BindingsJson.Add(MakeShared<FJsonValueObject>(B));
		}
	}
	TArray<UMVVMBlueprintViewEvent*> Events;
	TArray<TSharedPtr<FJsonValue>> EventsJson;
	for (const TObjectPtr<UMVVMBlueprintViewEvent>& Event : Ctx.View->GetEvents())
	{
		if (Event && (Event->GetEventPath().GetViewModelId() == VMId || Event->GetDestinationPath().GetViewModelId() == VMId || PinsUseViewModel(Event->GetPins(), VMId)))
		{
			Events.Add(Event);
			EventsJson.Add(MakeShared<FJsonValueString>(Event->GetName()));
		}
	}
	TArray<UMVVMBlueprintViewCondition*> Conditions;
	TArray<TSharedPtr<FJsonValue>> ConditionsJson;
	for (const TObjectPtr<UMVVMBlueprintViewCondition>& Condition : Ctx.View->GetConditions())
	{
		if (Condition && (Condition->GetConditionPath().GetViewModelId() == VMId || Condition->GetDestinationPath().GetViewModelId() == VMId || PinsUseViewModel(Condition->GetPins(), VMId)))
		{
			Conditions.Add(Condition);
			ConditionsJson.Add(MakeShared<FJsonValueString>(Condition->GetName()));
		}
	}
	const bool bUsedInGraphs = FBlueprintEditorUtils::IsVariableUsed(Ctx.WB, VMName);

	{
		// UMVVMEditorSubsystem::RemoveViewModel has no transaction of its own; the viewmodel panel wraps it like this.
		const FScopedTransaction Transaction(LOCTEXT("RemoveViewModel", "Remove viewmodel"));
		Ctx.View->Modify();
		if (bRemoveReferencingBindings)
		{
			for (const FGuid& Id : BindingIds)
			{
				if (const FMVVMBlueprintViewBinding* Binding = Ctx.View->GetBinding(Id))
				{
					Ctx.Sub->RemoveBinding(Ctx.WB, *Binding);
				}
			}
			for (UMVVMBlueprintViewEvent* Event : Events)
			{
				Ctx.Sub->RemoveEvent(Ctx.WB, Event);
			}
			for (UMVVMBlueprintViewCondition* Condition : Conditions)
			{
				Ctx.Sub->RemoveCondition(Ctx.WB, Condition);
			}
		}
		Ctx.Sub->RemoveViewModel(Ctx.WB, VMName);
	}
	if (Ctx.View->FindViewModel(VMId))
	{
		return Err(TEXT("viewmodel was not removed: ") + VMName.ToString());
	}

	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("removed"), VMName.ToString());
	Obj->SetArrayField(TEXT("referencingBindings"), BindingsJson);
	Obj->SetArrayField(TEXT("referencingEvents"), EventsJson);
	Obj->SetArrayField(TEXT("referencingConditions"), ConditionsJson);
	Obj->SetBoolField(TEXT("referencesRemoved"), bRemoveReferencingBindings);
	Obj->SetBoolField(TEXT("usedInGraphs"), bUsedInGraphs);
	return ToJson(Obj);
}

FString UMCPInspectorMVVMToolset::SetViewModelCreationType(const FString& WidgetPath, const FString& ViewModelName, const FString& CreationType, const FString& Argument)
{
	using namespace MCPInspectorMVVMToolset;
	FWidgetCtx Ctx;
	FString Error;
	if (!LoadWidget(WidgetPath, Ctx, Error, true))
	{
		return Err(Error);
	}
	const FMVVMBlueprintViewModelContext* Found = FindViewModelContext(Ctx.View, ViewModelName, Error);
	if (!Found)
	{
		return Err(Error);
	}
	FMVVMBlueprintViewModelContext* VM = Ctx.View->FindViewModel(Found->GetViewModelId()); // mutable overload
	check(VM);
	if (!VM->bCanEdit)
	{
		return Err(TEXT("viewmodel is not editable (bCanEdit=false)"));
	}
	if (VM->InstancedViewModel.Get() != nullptr)
	{
		return Err(TEXT("instanced viewmodels (created in the view) always use CreateInstance"));
	}
	EMVVMBlueprintViewModelContextCreationType NewType = EMVVMBlueprintViewModelContextCreationType::CreateInstance;
	if (!ParseCreationType(CreationType, NewType))
	{
		return Err(TEXT("invalid creation type '") + CreationType + TEXT("' (Manual, CreateInstance, GlobalViewModelCollection, PropertyPath, Resolver, Context)"));
	}
	UClass* VMClass = VM->GetViewModelClass();
	const TArray<EMVVMBlueprintViewModelContextCreationType> Allowed = VMClass ? UE::MVVM::GetAllowedContextCreationType(VMClass) : TArray<EMVVMBlueprintViewModelContextCreationType>();
	if (!Allowed.Contains(NewType))
	{
		FString List;
		for (EMVVMBlueprintViewModelContextCreationType Type : Allowed)
		{
			List += (List.IsEmpty() ? TEXT("") : TEXT(", ")) + CreationTypeStr(Type);
		}
		return Err(FString::Printf(TEXT("creation type %s is not allowed for %s (class metadata / project settings); allowed: %s"), *CreationTypeStr(NewType), VMClass ? *VMClass->GetName() : TEXT("<null>"), *List));
	}

	const FString Arg = Argument.TrimStartAndEnd();
	TObjectPtr<UMVVMViewModelContextResolver> NewResolver = nullptr;
	if (NewType == EMVVMBlueprintViewModelContextCreationType::GlobalViewModelCollection && Arg.IsEmpty())
	{
		return Err(TEXT("GlobalViewModelCollection needs Argument = the global viewmodel identifier"));
	}
	if (NewType == EMVVMBlueprintViewModelContextCreationType::PropertyPath && Arg.IsEmpty())
	{
		return Err(TEXT("PropertyPath needs Argument = the property/function path evaluated on the widget"));
	}
	if (NewType == EMVVMBlueprintViewModelContextCreationType::Resolver)
	{
		if (!Arg.IsEmpty())
		{
			UClass* ResolverClass = LoadClassArg(Arg);
			if (!ResolverClass || !ResolverClass->IsChildOf(UMVVMViewModelContextResolver::StaticClass()))
			{
				return Err(TEXT("not a MVVMViewModelContextResolver class: ") + Arg);
			}
			if (ResolverClass->HasAnyClassFlags(CLASS_Abstract))
			{
				return Err(TEXT("resolver class is abstract: ") + Arg);
			}
			if (!ResolverClass->GetDefaultObject<UMVVMViewModelContextResolver>()->DoesSupportViewModelClass(VMClass))
			{
				return Err(TEXT("resolver does not support viewmodel class ") + (VMClass ? VMClass->GetName() : FString()));
			}
			NewResolver = NewObject<UMVVMViewModelContextResolver>(Ctx.View, ResolverClass, NAME_None, RF_Public | RF_Transactional);
		}
		else if (!VM->Resolver)
		{
			FMVVMBlueprintViewModelContext Temp = *VM; // CreateDefaultResolver checks the context's own CreationType
			Temp.CreationType = EMVVMBlueprintViewModelContextCreationType::Resolver;
			NewResolver = Temp.CreateDefaultResolver(Ctx.View);
			if (!NewResolver)
			{
				return Err(TEXT("no resolver class given and the project MVVM setting 'Default Resolver Value' is empty or does not support this viewmodel"));
			}
		}
	}

	{
		const FScopedTransaction Transaction(LOCTEXT("SetCreationType", "Set Viewmodel Creation Type"));
		FScopedPropertyEdit Edit(Ctx.View, ViewProperty(TEXT("AvailableViewModels")), ContextProperty(GET_MEMBER_NAME_CHECKED(FMVVMBlueprintViewModelContext, CreationType)));
		VM->CreationType = NewType;
		// Same side effects as FBlueprintViewModelContextDetailCustomization::HandleCreationTypeChanged.
		VM->bOptional = NewType == EMVVMBlueprintViewModelContextCreationType::Manual || NewType == EMVVMBlueprintViewModelContextCreationType::Context;
		VM->bCreateSetterFunction = NewType == EMVVMBlueprintViewModelContextCreationType::Manual;
		if (NewType == EMVVMBlueprintViewModelContextCreationType::GlobalViewModelCollection)
		{
			VM->GlobalViewModelIdentifier = FName(*Arg);
		}
		else if (NewType == EMVVMBlueprintViewModelContextCreationType::PropertyPath)
		{
			VM->ViewModelPropertyPath = Arg;
		}
		else if (NewType == EMVVMBlueprintViewModelContextCreationType::Resolver && NewResolver)
		{
			VM->Resolver = NewResolver;
		}
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.WB); // generated getter/setter/variable flags depend on it

	const FMVVMBlueprintViewModelContext* Updated = Ctx.View->FindViewModel(Found->GetViewModelId());
	return Updated ? ToJson(ViewModelToJson(*Updated)) : Err(TEXT("viewmodel disappeared"));
}

FString UMCPInspectorMVVMToolset::SetViewModelOptions(const FString& WidgetPath, const FString& ViewModelName, bool bExposeInstanceInEditor, bool bCreateSetterFunction, bool bOptional)
{
	using namespace MCPInspectorMVVMToolset;
	FWidgetCtx Ctx;
	FString Error;
	if (!LoadWidget(WidgetPath, Ctx, Error, true))
	{
		return Err(Error);
	}
	const FMVVMBlueprintViewModelContext* Found = FindViewModelContext(Ctx.View, ViewModelName, Error);
	if (!Found)
	{
		return Err(Error);
	}
	FMVVMBlueprintViewModelContext* VM = Ctx.View->FindViewModel(Found->GetViewModelId());
	check(VM);
	if (!VM->bCanEdit)
	{
		return Err(TEXT("viewmodel is not editable (bCanEdit=false)"));
	}

	TArray<TSharedPtr<FJsonValue>> Warnings;
	if (VM->CreationType == EMVVMBlueprintViewModelContextCreationType::Manual && (!bCreateSetterFunction || !bOptional))
	{
		Warnings.Add(MakeShared<FJsonValueString>(TEXT("Manual viewmodels always get a setter and are always optional")));
	}
	if (VM->CreationType == EMVVMBlueprintViewModelContextCreationType::Context && !bOptional)
	{
		Warnings.Add(MakeShared<FJsonValueString>(TEXT("Context viewmodels are always optional")));
	}
	if (VM->CreationType != EMVVMBlueprintViewModelContextCreationType::CreateInstance && bExposeInstanceInEditor)
	{
		Warnings.Add(MakeShared<FJsonValueString>(TEXT("ExposeInstanceInEditor is only used with CreateInstance")));
	}
	if (bCreateSetterFunction && !GetDefault<UMVVMDeveloperProjectSettings>()->bAllowGeneratedViewModelSetter)
	{
		Warnings.Add(MakeShared<FJsonValueString>(TEXT("project setting 'Allow Generated Viewmodel Setter' is off")));
	}

	{
		const FScopedTransaction Transaction(LOCTEXT("SetViewModelOptions", "Set Viewmodel Options"));
		FScopedPropertyEdit Edit(Ctx.View, ViewProperty(TEXT("AvailableViewModels")), nullptr);
		VM->bExposeInstanceInEditor = bExposeInstanceInEditor;
		VM->bCreateSetterFunction = bCreateSetterFunction;
		VM->bOptional = bOptional;
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.WB);

	const FMVVMBlueprintViewModelContext* Updated = Ctx.View->FindViewModel(Found->GetViewModelId());
	if (!Updated)
	{
		return Err(TEXT("viewmodel disappeared"));
	}
	TSharedRef<FJsonObject> Obj = ViewModelToJson(*Updated);
	Obj->SetArrayField(TEXT("warnings"), Warnings);
	return ToJson(Obj);
}

FString UMCPInspectorMVVMToolset::RenameViewModel(const FString& WidgetPath, const FString& ViewModelName, const FString& NewName)
{
	using namespace MCPInspectorMVVMToolset;
	FWidgetCtx Ctx;
	FString Error;
	if (!LoadWidget(WidgetPath, Ctx, Error, true))
	{
		return Err(Error);
	}
	const FMVVMBlueprintViewModelContext* VM = FindViewModelContext(Ctx.View, ViewModelName, Error);
	if (!VM)
	{
		return Err(Error);
	}
	if (!VM->CanRename())
	{
		return Err(TEXT("viewmodel cannot be renamed (bCanRename=false or used as interface)"));
	}
	const FName OldName = VM->GetViewModelName();
	const FName NewFName(*NewName.TrimStartAndEnd());
	if (NewFName.IsNone())
	{
		return Err(TEXT("empty new name"));
	}
	FText RenameError;
	if (!Ctx.Sub->VerifyViewModelRename(Ctx.WB, OldName, NewFName, RenameError))
	{
		return Err(RenameError.ToString());
	}

	// UMVVMEditorSubsystem::RenameViewModel may open a modal dialog (LoadBlueprintReferencersForViewModelRename).
	// Do the "Yes" path silently: load unloaded Blueprints that reference this widget, then rename on the view.
	TArray<TSharedPtr<FJsonValue>> Loaded;
	{
		FARFilter Filter;
		Filter.bIncludeOnlyOnDiskAssets = true;
		Filter.bRecursiveClasses = true;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		IAssetRegistry& AssetRegistry = IAssetRegistry::GetChecked();
		AssetRegistry.GetReferencers(Ctx.WB->GetPackage()->GetFName(), Filter.PackageNames, UE::AssetRegistry::EDependencyCategory::Package);
		if (!Filter.PackageNames.IsEmpty())
		{
			TArray<FAssetData> Referencers;
			AssetRegistry.GetAssets(Filter, Referencers);
			for (const FAssetData& Asset : Referencers)
			{
				if (Asset.FastGetAsset(false) == nullptr && Asset.FastGetAsset(true) != nullptr)
				{
					Loaded.Add(MakeShared<FJsonValueString>(Asset.GetObjectPathString()));
				}
			}
		}
	}

	bool bRenamed = false;
	{
		const FScopedTransaction Transaction(LOCTEXT("RenameViewModel", "Rename Viewmodel"));
		Ctx.View->Modify();
		bRenamed = Ctx.View->RenameViewModel(OldName, NewFName); // updates variable references + MarkBlueprintAsStructurallyModified
	}
	if (!bRenamed)
	{
		return Err(TEXT("rename failed"));
	}
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("oldName"), OldName.ToString());
	Obj->SetStringField(TEXT("newName"), NewFName.ToString());
	Obj->SetArrayField(TEXT("loadedReferencers"), Loaded);
	return ToJson(Obj);
}

// ============================================================================================================
// viewmodel Blueprints
// ============================================================================================================

FString UMCPInspectorMVVMToolset::RemoveViewModelProperty(const FString& ViewModelPath, const FString& PropertyName)
{
	using namespace MCPInspectorMVVMToolset;
	FString Error;
	UBlueprint* BP = LoadViewModelBlueprint(ViewModelPath, Error);
	if (!BP)
	{
		return Err(Error);
	}
	const FName VarName(*PropertyName.TrimStartAndEnd());
	const int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(BP, VarName);
	if (VarIndex == INDEX_NONE)
	{
		return Err(TEXT("variable not declared in this blueprint: ") + PropertyName);
	}
	const bool bFieldNotify = BP->NewVariables[VarIndex].HasMetaData(FBlueprintMetadata::MD_FieldNotify);
	const bool bUsed = FBlueprintEditorUtils::IsVariableUsed(BP, VarName);
	{
		const FScopedTransaction Transaction(LOCTEXT("RemoveViewModelProperty", "Remove Viewmodel Property"));
		BP->Modify();
		FBlueprintEditorUtils::RemoveMemberVariable(BP, VarName); // also removes its nodes and marks structurally modified
	}
	if (FBlueprintEditorUtils::FindNewVariableIndex(BP, VarName) != INDEX_NONE)
	{
		return Err(TEXT("variable was not removed: ") + PropertyName);
	}
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("removed"), VarName.ToString());
	Obj->SetBoolField(TEXT("fieldNotify"), bFieldNotify);
	Obj->SetBoolField(TEXT("wasUsedInGraphs"), bUsed);
	return ToJson(Obj);
}

FString UMCPInspectorMVVMToolset::SetViewModelPropertyDefault(const FString& ViewModelPath, const FString& PropertyName, const FString& ValueText)
{
	using namespace MCPInspectorMVVMToolset;
	FString Error;
	UBlueprint* BP = LoadViewModelBlueprint(ViewModelPath, Error);
	if (!BP)
	{
		return Err(Error);
	}
	const FName VarName(*PropertyName.TrimStartAndEnd());
	if (FBlueprintEditorUtils::FindNewVariableIndex(BP, VarName) == INDEX_NONE)
	{
		return Err(TEXT("variable not declared in this blueprint: ") + PropertyName);
	}

	bool bCompiled = false;
	const FProperty* Property = BP->GeneratedClass ? FindFProperty<FProperty>(BP->GeneratedClass, VarName) : nullptr;
	if (!Property)
	{
		// Added but never compiled: the class default object does not have it yet.
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave);
		bCompiled = true;
		Property = BP->GeneratedClass ? FindFProperty<FProperty>(BP->GeneratedClass, VarName) : nullptr;
	}
	UObject* CDO = BP->GeneratedClass ? BP->GeneratedClass->GetDefaultObject(false) : nullptr;
	if (!Property || !CDO)
	{
		return Err(TEXT("variable is not on the generated class (does the blueprint compile?): ") + PropertyName);
	}

	{
		// Same as editing the value in the Class Defaults / variable details panel: write the CDO.
		const FScopedTransaction Transaction(LOCTEXT("SetViewModelPropertyDefault", "Set Viewmodel Property Default"));
		CDO->Modify();
		if (!FBlueprintEditorUtils::PropertyValueFromString(Property, ValueText, reinterpret_cast<uint8*>(CDO), CDO))
		{
			return Err(FString::Printf(TEXT("could not import '%s' into %s (%s)"), *ValueText, *PropertyName, *Property->GetCPPType()));
		}
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	FString ReadBack;
	Property->ExportText_InContainer(0, ReadBack, CDO, CDO, CDO, PPF_None);
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("property"), VarName.ToString());
	Obj->SetStringField(TEXT("value"), ReadBack);
	Obj->SetBoolField(TEXT("compiled"), bCompiled);
	return ToJson(Obj);
}

// ============================================================================================================
// events / conditions
// ============================================================================================================

FString UMCPInspectorMVVMToolset::AddViewEvent(const FString& WidgetPath, const FString& EventPath, const FString& DestinationFunctionPath)
{
	using namespace MCPInspectorMVVMToolset;
	if (!GetDefault<UMVVMDeveloperProjectSettings>()->bAllowBindingEvent)
	{
		return Err(TEXT("view events are disabled by the project MVVM setting 'Allow Binding Event'"));
	}
	FWidgetCtx Ctx;
	FString Error;
	if (!LoadWidget(WidgetPath, Ctx, Error, true, /*bCreateView*/ true))
	{
		return Err(Error);
	}
	FMVVMBlueprintPropertyPath Path;
	if (!ParsePropertyPath(Ctx.WB, Ctx.View, EventPath, Path, Error))
	{
		return Err(TEXT("event path: ") + Error);
	}
	if (!UMVVMBlueprintViewEvent::Supports(Ctx.WB, Path))
	{
		return Err(TEXT("event path must end with a multicast delegate property (e.g. Button.OnClicked): ") + EventPath);
	}
	FMVVMBlueprintPropertyPath Destination;
	bool bIsFunction = false;
	if (!ParsePropertyPath(Ctx.WB, Ctx.View, DestinationFunctionPath, Destination, Error, &bIsFunction))
	{
		return Err(TEXT("destination: ") + Error);
	}
	if (!bIsFunction)
	{
		return Err(TEXT("destination must end with a function: ") + DestinationFunctionPath);
	}

	UMVVMBlueprintViewEvent* Event = nullptr;
	{
		const FScopedTransaction Transaction(LOCTEXT("AddViewEvent", "MVVM Add Event"));
		Event = Ctx.Sub->AddEvent(Ctx.WB);
		if (!Event)
		{
			return Err(TEXT("UMVVMEditorSubsystem::AddEvent failed"));
		}
		Ctx.Sub->SetEventPath(Event, Path, /*bRequestBindingConversion*/ false);
		Ctx.Sub->SetEventDestinationPath(Event, Destination);
		FBlueprintEditorUtils::MarkBlueprintAsModified(Ctx.WB);
	}
	return ToJson(EventToJson(Ctx.WB, Ctx.View, Event));
}

FString UMCPInspectorMVVMToolset::SetViewEventArgument(const FString& WidgetPath, const FString& EventId, const FString& ArgumentName, const FString& ValueOrPath)
{
	using namespace MCPInspectorMVVMToolset;
	FWidgetCtx Ctx;
	FString Error;
	if (!LoadWidget(WidgetPath, Ctx, Error, true))
	{
		return Err(Error);
	}
	UMVVMBlueprintViewEvent* Event = FindEvent(Ctx.View, EventId);
	if (!Event)
	{
		return Err(TEXT("event not found: ") + EventId);
	}
	const FMVVMBlueprintPinId PinId = ResolvePinId(Event->GetPins(), ArgumentName);
	UEdGraphPin* GraphPin = PinId.IsValid() ? Event->GetOrCreateGraphPin(PinId) : nullptr;
	if (!GraphPin || !FMVVMBlueprintPin::IsInputPin(GraphPin))
	{
		return Err(TEXT("argument not found on the event destination function: ") + ArgumentName);
	}

	bool bIsPath = false;
	FString Text;
	SplitValueOrPath(ValueOrPath, bIsPath, Text);
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("argument"), PinId.ToString());

	if (bIsPath)
	{
		FMVVMBlueprintPropertyPath Path;
		if (!Text.IsEmpty() && !ParsePropertyPath(Ctx.WB, Ctx.View, Text, Path, Error))
		{
			return Err(Error);
		}
		Ctx.Sub->SetEventArgumentPath(Event, PinId, Path);
		Obj->SetObjectField(TEXT("path"), PathToJson(Ctx.WB, Ctx.View, Event->GetPinPath(PinId)));
		return ToJson(Obj);
	}

	const FScopedTransaction Transaction(LOCTEXT("SetEventArgument", "Set Event Argument"));
	if (GraphPin->LinkedTo.Num() > 0)
	{
		Ctx.Sub->SetEventArgumentPath(Event, PinId, FMVVMBlueprintPropertyPath());
		GraphPin = Event->GetOrCreateGraphPin(PinId);
		if (!GraphPin)
		{
			return Err(TEXT("argument pin disappeared after unbinding"));
		}
	}
	{
		// Same as UE::MVVM::Private::OnEventPreEditChange/OnEventPostEditChange(Event, "SavedPins").
		FScopedPropertyEdit Edit(Event, nullptr, UMVVMBlueprintViewEvent::StaticClass()->FindPropertyByName(TEXT("SavedPins")));
		if (!SetPinLiteral(GraphPin, Text, Error))
		{
			return Err(Error);
		}
		Event->SavePinValues();
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(Ctx.WB);
	Obj->SetStringField(TEXT("value"), GraphPin->GetDefaultAsString());
	return ToJson(Obj);
}

FString UMCPInspectorMVVMToolset::RemoveViewEvent(const FString& WidgetPath, const FString& EventId)
{
	using namespace MCPInspectorMVVMToolset;
	FWidgetCtx Ctx;
	FString Error;
	if (!LoadWidget(WidgetPath, Ctx, Error, true))
	{
		return Err(Error);
	}
	UMVVMBlueprintViewEvent* Event = FindEvent(Ctx.View, EventId);
	if (!Event)
	{
		return Err(TEXT("event not found: ") + EventId);
	}
	Ctx.Sub->RemoveEvent(Ctx.WB, Event); // own transaction, View->Modify, MarkBlueprintAsModified
	return FindEvent(Ctx.View, EventId) ? Err(TEXT("event was not removed")) : FString(TEXT("OK"));
}

FString UMCPInspectorMVVMToolset::AddViewCondition(const FString& WidgetPath, const FString& ConditionPath, const FString& Operation, float Value, float MaxValue, const FString& DestinationFunctionPath)
{
	using namespace MCPInspectorMVVMToolset;
	const UMVVMDeveloperProjectSettings* Settings = GetDefault<UMVVMDeveloperProjectSettings>();
	if (!Settings->bAllowConditionBinding)
	{
		return Err(TEXT("view conditions are disabled by the project MVVM setting 'Allow Condition Binding'"));
	}
	FWidgetCtx Ctx;
	FString Error;
	if (!LoadWidget(WidgetPath, Ctx, Error, true, /*bCreateView*/ true))
	{
		return Err(Error);
	}
	FMVVMBlueprintPropertyPath Path;
	if (!ParsePropertyPath(Ctx.WB, Ctx.View, ConditionPath, Path, Error))
	{
		return Err(TEXT("condition path: ") + Error);
	}
	if (Path.GetSource(Ctx.WB) == EMVVMBlueprintFieldPathSource::Widget && !Settings->bAllowWidgetInConditionSource)
	{
		return Err(TEXT("condition source must be a viewmodel field (widget sources are disabled, like in the condition row)"));
	}
	EMVVMConditionOperation Op = EMVVMConditionOperation::Equal;
	if (!ParseConditionOperation(Operation, Op))
	{
		return Err(TEXT("invalid operation '") + Operation + TEXT("' (Equal, NotEqual, MoreThan, MoreThanOrEqual, LessThan, LessThanOrEqual, BetweenInclusive, BetweenExclusive)"));
	}
	FMVVMBlueprintPropertyPath Destination;
	bool bIsFunction = false;
	if (!ParsePropertyPath(Ctx.WB, Ctx.View, DestinationFunctionPath, Destination, Error, &bIsFunction))
	{
		return Err(TEXT("destination: ") + Error);
	}
	if (!bIsFunction)
	{
		return Err(TEXT("destination must end with a function: ") + DestinationFunctionPath);
	}

	UMVVMBlueprintViewCondition* Condition = nullptr;
	{
		const FScopedTransaction Transaction(LOCTEXT("AddViewCondition", "MVVM Add Condition"));
		Condition = Ctx.Sub->AddCondition(Ctx.WB);
		if (!Condition)
		{
			return Err(TEXT("UMVVMEditorSubsystem::AddCondition failed"));
		}
		Ctx.Sub->SetConditionPath(Condition, Path, /*bRequestBindingConversion*/ false);
		Ctx.Sub->SetConditionOperation(Condition, Op);
		Ctx.Sub->SetConditionOperationValue(Condition, Value);
		Ctx.Sub->SetConditionOperationMaxValue(Condition, MaxValue);
		Ctx.Sub->SetConditionDestinationPath(Condition, Destination);
		FBlueprintEditorUtils::MarkBlueprintAsModified(Ctx.WB);
	}
	return ToJson(ConditionToJson(Ctx.WB, Ctx.View, Condition));
}

FString UMCPInspectorMVVMToolset::RemoveViewCondition(const FString& WidgetPath, const FString& ConditionId)
{
	using namespace MCPInspectorMVVMToolset;
	FWidgetCtx Ctx;
	FString Error;
	if (!LoadWidget(WidgetPath, Ctx, Error, true))
	{
		return Err(Error);
	}
	UMVVMBlueprintViewCondition* Condition = FindCondition(Ctx.View, ConditionId);
	if (!Condition)
	{
		return Err(TEXT("condition not found: ") + ConditionId);
	}
	Ctx.Sub->RemoveCondition(Ctx.WB, Condition);
	return FindCondition(Ctx.View, ConditionId) ? Err(TEXT("condition was not removed")) : FString(TEXT("OK"));
}

// ============================================================================================================
// inspection
// ============================================================================================================

FString UMCPInspectorMVVMToolset::DumpWidgetMVVM(const FString& WidgetPath)
{
	using namespace MCPInspectorMVVMToolset;
	FWidgetCtx Ctx;
	FString Error;
	if (!LoadWidget(WidgetPath, Ctx, Error, /*bRequireView*/ false))
	{
		return Err(Error);
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("widget"), Ctx.WB->GetPathName());
	Root->SetStringField(TEXT("status"), StatusStr(Ctx.WB->Status));
	Root->SetBoolField(TEXT("hasView"), Ctx.View != nullptr);

	TArray<TSharedPtr<FJsonValue>> ViewModels;
	TArray<TSharedPtr<FJsonValue>> Bindings;
	TArray<TSharedPtr<FJsonValue>> Events;
	TArray<TSharedPtr<FJsonValue>> Conditions;
	if (UMVVMBlueprintView* View = Ctx.View)
	{
		if (const UMVVMBlueprintViewSettings* Settings = View->GetSettings())
		{
			TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
			S->SetBoolField(TEXT("initializeSourcesOnConstruct"), Settings->bInitializeSourcesOnConstruct);
			S->SetBoolField(TEXT("initializeBindingsOnConstruct"), Settings->bInitializeBindingsOnConstruct);
			S->SetBoolField(TEXT("initializeEventsOnConstruct"), Settings->bInitializeEventsOnConstruct);
			S->SetBoolField(TEXT("createViewWithoutBindings"), Settings->bCreateViewWithoutBindings);
			Root->SetObjectField(TEXT("settings"), S);
		}

		for (const FMVVMBlueprintViewModelContext& Context : View->GetViewModels())
		{
			ViewModels.Add(MakeShared<FJsonValueObject>(ViewModelToJson(Context)));
		}

		for (const FMVVMBlueprintViewBinding& Binding : View->GetBindings())
		{
			TSharedRef<FJsonObject> B = MakeShared<FJsonObject>();
			B->SetStringField(TEXT("id"), IdStr(Binding.BindingId));
			B->SetStringField(TEXT("text"), Binding.GetDisplayNameString(Ctx.WB));
			B->SetObjectField(TEXT("source"), PathToJson(Ctx.WB, View, Binding.SourcePath));
			B->SetObjectField(TEXT("destination"), PathToJson(Ctx.WB, View, Binding.DestinationPath));
			B->SetStringField(TEXT("mode"), BindingModeStr(Binding.BindingType));
			B->SetStringField(TEXT("executionMode"), Binding.bOverrideExecutionMode ? ExecutionModeStr(Binding.OverrideExecutionMode) : FString(TEXT("Default")));
			B->SetBoolField(TEXT("enabled"), Binding.bEnabled);
			B->SetBoolField(TEXT("compile"), Binding.bCompile);
			TSharedRef<FJsonObject> Conversion = MakeShared<FJsonObject>();
			if (TSharedPtr<FJsonObject> Forward = ConversionToJson(Ctx.WB, View, Binding.Conversion.GetConversionFunction(true)))
			{
				Conversion->SetObjectField(TEXT("sourceToDestination"), Forward);
			}
			if (TSharedPtr<FJsonObject> Backward = ConversionToJson(Ctx.WB, View, Binding.Conversion.GetConversionFunction(false)))
			{
				Conversion->SetObjectField(TEXT("destinationToSource"), Backward);
			}
			B->SetObjectField(TEXT("conversion"), Conversion);
			B->SetArrayField(TEXT("messages"), BindingMessagesToJson(View, Binding.BindingId));
			Bindings.Add(MakeShared<FJsonValueObject>(B));
		}

		for (const TObjectPtr<UMVVMBlueprintViewEvent>& Event : View->GetEvents())
		{
			if (Event)
			{
				Events.Add(MakeShared<FJsonValueObject>(EventToJson(Ctx.WB, View, Event)));
			}
		}
		for (const TObjectPtr<UMVVMBlueprintViewCondition>& Condition : View->GetConditions())
		{
			if (Condition)
			{
				Conditions.Add(MakeShared<FJsonValueObject>(ConditionToJson(Ctx.WB, View, Condition)));
			}
		}
	}
	Root->SetArrayField(TEXT("viewModels"), ViewModels);
	Root->SetArrayField(TEXT("bindings"), Bindings);
	Root->SetArrayField(TEXT("events"), Events);
	Root->SetArrayField(TEXT("conditions"), Conditions);
	return ToJson(Root);
}

FString UMCPInspectorMVVMToolset::CompileWidgetMVVM(const FString& WidgetPath)
{
	using namespace MCPInspectorMVVMToolset;
	FWidgetCtx Ctx;
	FString Error;
	if (!LoadWidget(WidgetPath, Ctx, Error, /*bRequireView*/ false))
	{
		return Err(Error);
	}
	FCompilerResultsLog Log;
	Log.SetSilentMode(true);
	FKismetEditorUtilities::CompileBlueprint(Ctx.WB, EBlueprintCompileOptions::SkipSave, &Log);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("status"), StatusStr(Ctx.WB->Status));
	Root->SetNumberField(TEXT("errors"), Log.NumErrors);
	Root->SetNumberField(TEXT("warnings"), Log.NumWarnings);
	TArray<TSharedPtr<FJsonValue>> Messages;
	for (const TSharedRef<FTokenizedMessage>& Message : Log.Messages)
	{
		FString Severity;
		switch (Message->GetSeverity())
		{
		case EMessageSeverity::Error: Severity = TEXT("Error"); break;
		case EMessageSeverity::Warning:
		case EMessageSeverity::PerformanceWarning: Severity = TEXT("Warning"); break;
		default: Severity = TEXT("Info"); break;
		}
		TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("severity"), Severity);
		M->SetStringField(TEXT("text"), Message->ToText().ToString());
		Messages.Add(MakeShared<FJsonValueObject>(M));
	}
	Root->SetArrayField(TEXT("messages"), Messages);

	// Per-binding messages are filled by the MVVM view compiler during the compile above.
	TArray<TSharedPtr<FJsonValue>> BindingMessages;
	if (UMVVMBlueprintView* View = Ctx.Sub->GetView(Ctx.WB))
	{
		auto AddEntries = [&BindingMessages](const FString& Id, const TCHAR* Kind, const TCHAR* Severity, const TArray<FText>& Texts)
		{
			for (const FText& Text : Texts)
			{
				TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
				M->SetStringField(TEXT("id"), Id);
				M->SetStringField(TEXT("kind"), Kind);
				M->SetStringField(TEXT("severity"), Severity);
				M->SetStringField(TEXT("text"), Text.ToString());
				BindingMessages.Add(MakeShared<FJsonValueObject>(M));
			}
		};
		for (const FMVVMBlueprintViewBinding& Binding : View->GetBindings())
		{
			const FString Id = IdStr(Binding.BindingId);
			AddEntries(Id, TEXT("binding"), TEXT("Error"), View->GetBindingMessages(Binding.BindingId, UE::MVVM::EBindingMessageType::Error));
			AddEntries(Id, TEXT("binding"), TEXT("Warning"), View->GetBindingMessages(Binding.BindingId, UE::MVVM::EBindingMessageType::Warning));
			AddEntries(Id, TEXT("binding"), TEXT("Info"), View->GetBindingMessages(Binding.BindingId, UE::MVVM::EBindingMessageType::Info));
		}
		for (const TObjectPtr<UMVVMBlueprintViewEvent>& Event : View->GetEvents())
		{
			if (Event)
			{
				AddEntries(Event->GetName(), TEXT("event"), TEXT("Error"), Event->GetCompilationMessages(UMVVMBlueprintViewEvent::EMessageType::Error));
				AddEntries(Event->GetName(), TEXT("event"), TEXT("Warning"), Event->GetCompilationMessages(UMVVMBlueprintViewEvent::EMessageType::Warning));
			}
		}
		for (const TObjectPtr<UMVVMBlueprintViewCondition>& Condition : View->GetConditions())
		{
			if (Condition)
			{
				AddEntries(Condition->GetName(), TEXT("condition"), TEXT("Error"), Condition->GetCompilationMessages(UMVVMBlueprintViewCondition::EMessageType::Error));
				AddEntries(Condition->GetName(), TEXT("condition"), TEXT("Warning"), Condition->GetCompilationMessages(UMVVMBlueprintViewCondition::EMessageType::Warning));
			}
		}
	}
	Root->SetArrayField(TEXT("bindingMessages"), BindingMessages);
	return ToJson(Root);
}

#undef LOCTEXT_NAMESPACE
