#include "MCPInspectorBehaviorTreeToolset.h"

#include "AIGraphNode.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Composites/BTComposite_SimpleParallel.h"
#include "BehaviorTree/Tasks/BTTask_RunBehavior.h"
#include "BehaviorTree/ValueOrBBKey.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTreeGraphNode_SubtreeTask.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "MCPInspectorBehaviorTreeToolset"

namespace MCPInspectorBehaviorTreeToolset
{
	static FString Err(const FString& Msg) { return TEXT("ERROR|") + Msg; }

	// Horizontal spacing used when placing new graph nodes (cosmetic, but X order = child order).
	static constexpr int32 SpacingX = 320;
	static constexpr int32 SpacingY = 200;

	// "/Game/A/B" -> "/Game/A/B.B"
	static FString ToObjectPath(const FString& Path)
	{
		return Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPackageName::GetShortName(Path);
	}

	template <typename T>
	static T* LoadAsset(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		return LoadObject<T>(nullptr, *ToObjectPath(Path.TrimStartAndEnd()));
	}

	static FString ToJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	static FString IdOf(const UEdGraphNode* Node)
	{
		return Node ? Node->NodeGuid.ToString(EGuidFormats::Digits) : FString();
	}

	static bool ValidateNewAssetPath(const FString& AssetPath, FString& OutError)
	{
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid package path: ") + AssetPath;
			return false;
		}
		if (FPackageName::DoesPackageExist(AssetPath) || FindPackage(nullptr, *AssetPath))
		{
			OutError = TEXT("asset already exists: ") + AssetPath;
			return false;
		}
		return true;
	}

	/**
	 * Resolve a node class from a native path, short name or Blueprint asset path.
	 * Short names are tried as-is and with the BT prefixes ("Wait" -> "BTTask_Wait", "Selector" -> "BTComposite_Selector").
	 */
	static UClass* ResolveNodeClass(const FString& InName, UClass* RequiredBase, FString& OutError)
	{
		const FString Name = InName.TrimStartAndEnd();
		UClass* Found = nullptr;

		if (Name.StartsWith(TEXT("/Script/")))
		{
			Found = FindObject<UClass>(nullptr, *Name);
			if (!Found)
			{
				Found = LoadObject<UClass>(nullptr, *Name);
			}
		}
		else if (Name.StartsWith(TEXT("/")))
		{
			// Blueprint asset ("/Game/AI/BTT_X" or "/Game/AI/BTT_X.BTT_X") or generated class path ("..._C").
			UObject* Obj = LoadObject<UObject>(nullptr, *ToObjectPath(Name));
			if (UBlueprint* BP = Cast<UBlueprint>(Obj))
			{
				Found = BP->GeneratedClass.Get();
			}
			else
			{
				Found = Cast<UClass>(Obj);
			}
		}
		else
		{
			const TCHAR* Prefixes[] = { TEXT(""), TEXT("BTComposite_"), TEXT("BTTask_"), TEXT("BTDecorator_"), TEXT("BTService_") };
			for (const TCHAR* Prefix : Prefixes)
			{
				UClass* Candidate = FindFirstObject<UClass>(*(FString(Prefix) + Name), EFindFirstObjectOptions::NativeFirst);
				if (Candidate && Candidate->IsChildOf(RequiredBase))
				{
					Found = Candidate;
					break;
				}
			}
		}

		if (!Found)
		{
			OutError = FString::Printf(TEXT("class not found: %s"), *Name);
			return nullptr;
		}
		if (!Found->IsChildOf(RequiredBase))
		{
			OutError = FString::Printf(TEXT("%s is not a %s"), *Found->GetPathName(), *RequiredBase->GetName());
			return nullptr;
		}
		if (Found->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			OutError = FString::Printf(TEXT("class is abstract/deprecated: %s"), *Found->GetPathName());
			return nullptr;
		}
		return Found;
	}

	/** Same steps as FBehaviorTreeEditor::RestoreBehaviorTree for a tree without a graph. */
	static UBehaviorTreeGraph* GetOrCreateGraph(UBehaviorTree* BT)
	{
		UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(BT->BTGraph);
		if (Graph)
		{
			return Graph;
		}

		BT->Modify();
		BT->BTGraph = FBlueprintEditorUtils::CreateNewGraph(BT, FName(TEXT("Behavior Tree")),
			UBehaviorTreeGraph::StaticClass(), UEdGraphSchema_BehaviorTree::StaticClass());
		Graph = Cast<UBehaviorTreeGraph>(BT->BTGraph);
		if (!Graph)
		{
			return nullptr;
		}
		Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
		Graph->OnCreated();   // SpawnMissingNodes: rebuilds graph nodes from an existing runtime tree, if any
		Graph->Initialize();
		Graph->UpdateAsset(UBehaviorTreeGraph::ClearDebuggerFlags | UBehaviorTreeGraph::KeepRebuildCounter);
		return Graph;
	}

	static UBehaviorTreeGraphNode_Root* FindRoot(UBehaviorTreeGraph* Graph)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UBehaviorTreeGraphNode_Root* Root = Cast<UBehaviorTreeGraphNode_Root>(Node))
			{
				return Root;
			}
		}
		return nullptr;
	}

	/** Finds a main node or a sub node (decorator/service). OutOwner receives the owning main node for sub nodes. */
	static UBehaviorTreeGraphNode* FindNode(UBehaviorTreeGraph* Graph, const FString& Id, UBehaviorTreeGraphNode** OutOwner = nullptr)
	{
		if (OutOwner)
		{
			*OutOwner = nullptr;
		}
		const FString Trimmed = Id.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || Trimmed.Equals(TEXT("root"), ESearchCase::IgnoreCase))
		{
			return FindRoot(Graph);
		}
		FGuid Guid;
		if (!FGuid::Parse(Trimmed, Guid))
		{
			return nullptr;
		}
		for (UEdGraphNode* EdNode : Graph->Nodes)
		{
			UBehaviorTreeGraphNode* Node = Cast<UBehaviorTreeGraphNode>(EdNode);
			if (!Node)
			{
				continue;
			}
			if (Node->NodeGuid == Guid)
			{
				return Node;
			}
			for (UBehaviorTreeGraphNode* Sub : Node->Decorators)
			{
				if (Sub && Sub->NodeGuid == Guid)
				{
					if (OutOwner) { *OutOwner = Node; }
					return Sub;
				}
			}
			for (UBehaviorTreeGraphNode* Sub : Node->Services)
			{
				if (Sub && Sub->NodeGuid == Guid)
				{
					if (OutOwner) { *OutOwner = Node; }
					return Sub;
				}
			}
		}
		return nullptr;
	}

	static bool IsSimpleParallel(const UBehaviorTreeGraphNode* Node)
	{
		return Node && Node->NodeInstance && Node->NodeInstance->IsA(UBTComposite_SimpleParallel::StaticClass());
	}

	/** Graph nodes linked to one output pin, in BT editor order (X, then Y; see FCompareNodeXLocation). */
	static TArray<UBehaviorTreeGraphNode*> GetPinChildren(const UEdGraphPin* Pin)
	{
		TArray<UBehaviorTreeGraphNode*> Out;
		if (!Pin)
		{
			return Out;
		}
		for (UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (UBehaviorTreeGraphNode* Child = Linked ? Cast<UBehaviorTreeGraphNode>(Linked->GetOwningNode()) : nullptr)
			{
				Out.Add(Child);
			}
		}
		Out.Sort([](const UBehaviorTreeGraphNode& A, const UBehaviorTreeGraphNode& B)
		{
			return A.NodePosX == B.NodePosX ? A.NodePosY < B.NodePosY : A.NodePosX < B.NodePosX;
		});
		return Out;
	}

	/** All children of a node in execution order (output pins in order, each sorted by X). */
	static TArray<UBehaviorTreeGraphNode*> GetChildren(const UBehaviorTreeGraphNode* Node)
	{
		TArray<UBehaviorTreeGraphNode*> Out;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output)
			{
				Out.Append(GetPinChildren(Pin));
			}
		}
		return Out;
	}

	static void CollectSubtree(UBehaviorTreeGraphNode* Node, TArray<UBehaviorTreeGraphNode*>& Out)
	{
		if (!Node || Out.Contains(Node))
		{
			return;
		}
		Out.Add(Node);
		for (UBehaviorTreeGraphNode* Child : GetChildren(Node))
		{
			CollectSubtree(Child, Out);
		}
	}

	static void ShiftSubtreeX(UBehaviorTreeGraphNode* Node, int32 DeltaX)
	{
		TArray<UBehaviorTreeGraphNode*> Nodes;
		CollectSubtree(Node, Nodes);
		for (UBehaviorTreeGraphNode* N : Nodes)
		{
			N->Modify();
			N->NodePosX += DeltaX;
		}
	}

	static int32 SubtreeMaxX(UBehaviorTreeGraphNode* Node)
	{
		TArray<UBehaviorTreeGraphNode*> Nodes;
		CollectSubtree(Node, Nodes);
		int32 MaxX = Node->NodePosX;
		for (UBehaviorTreeGraphNode* N : Nodes)
		{
			MaxX = FMath::Max(MaxX, N->NodePosX);
		}
		return MaxX;
	}

	/** Rebuild the runtime tree from the graph (what the BT editor does after every graph change) and refresh any open editor. */
	static void Commit(UBehaviorTree* BT, UBehaviorTreeGraph* Graph)
	{
		Graph->UpdateAsset(UBehaviorTreeGraph::ClearDebuggerFlags);
		Graph->NotifyGraphChanged();
		BT->MarkPackageDirty();
	}

	static UBehaviorTreeGraph* LoadTreeGraph(const FString& TreePath, UBehaviorTree*& OutBT, FString& OutError)
	{
		OutBT = LoadAsset<UBehaviorTree>(TreePath);
		if (!OutBT)
		{
			OutError = TEXT("behavior tree not found: ") + TreePath;
			return nullptr;
		}
		UBehaviorTreeGraph* Graph = GetOrCreateGraph(OutBT);
		if (!Graph || !FindRoot(Graph))
		{
			OutError = TEXT("could not create/find the behavior tree editor graph");
			return nullptr;
		}
		return Graph;
	}

	// ---- reflection path helpers ("A.B[2].C") ---------------------------------------------------

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

	/** Resolves a property path inside (Struct, Container). OutTopProp = first segment property (member property for change events). */
	static bool ResolvePath(const UStruct* Struct, void* Container, const FString& Path,
		FProperty*& OutProp, void*& OutPtr, FProperty*& OutTopProp, FString& OutError)
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
		OutTopProp = nullptr;

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
			if (i == 0)
			{
				OutTopProp = Prop;
			}
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

	static FString KindOf(const UBehaviorTreeGraphNode* Node)
	{
		if (Cast<UBehaviorTreeGraphNode_Root>(Node)) return TEXT("root");
		if (Cast<UBehaviorTreeGraphNode_Decorator>(Node)) return TEXT("decorator");
		if (Cast<UBehaviorTreeGraphNode_Service>(Node)) return TEXT("service");
		if (Cast<UBehaviorTreeGraphNode_Composite>(Node)) return TEXT("composite");
		if (Cast<UBehaviorTreeGraphNode_Task>(Node)) return TEXT("task");
		return Node ? Node->GetClass()->GetName() : TEXT("null");
	}

	/** Top-level FBlackboardKeySelector properties of a node instance -> selected key names. */
	static TSharedRef<FJsonObject> DumpKeys(const UObject* Instance)
	{
		TSharedRef<FJsonObject> Keys = MakeShared<FJsonObject>();
		if (!Instance)
		{
			return Keys;
		}
		for (TFieldIterator<FStructProperty> It(Instance->GetClass()); It; ++It)
		{
			if (It->Struct == FBlackboardKeySelector::StaticStruct())
			{
				const FBlackboardKeySelector* Selector = It->ContainerPtrToValuePtr<FBlackboardKeySelector>(Instance);
				Keys->SetStringField(It->GetName(), Selector->SelectedKeyName.ToString());
			}
			else if (It->Struct->IsChildOf(FValueOrBlackboardKeyBase::StaticStruct()))
			{
				const FValueOrBlackboardKeyBase* ValueOrKey = It->ContainerPtrToValuePtr<FValueOrBlackboardKeyBase>(Instance);
				Keys->SetStringField(It->GetName(), ValueOrKey->GetKey().IsNone() ? ValueOrKey->ToString() : TEXT("bb:") + ValueOrKey->GetKey().ToString());
			}
		}
		return Keys;
	}

	static TSharedRef<FJsonObject> DumpNodeBase(const UBehaviorTreeGraphNode* Node)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), IdOf(Node));
		O->SetStringField(TEXT("kind"), KindOf(Node));
		const UBTNode* Instance = Cast<UBTNode>(Node->NodeInstance);
		O->SetStringField(TEXT("class"), Instance ? Instance->GetClass()->GetPathName() : FString());
		O->SetStringField(TEXT("name"), Instance ? Instance->GetNodeName() : FString());
		O->SetStringField(TEXT("description"), Instance ? Instance->GetStaticDescription() : FString());
		O->SetNumberField(TEXT("executionIndex"), Instance ? (double)Instance->GetExecutionIndex() : -1.0);
		O->SetObjectField(TEXT("keys"), DumpKeys(Instance));
		if (!Node->ErrorMessage.IsEmpty())
		{
			O->SetStringField(TEXT("error"), Node->ErrorMessage);
		}
		return O;
	}

	static TSharedRef<FJsonObject> DumpNode(UBehaviorTreeGraphNode* Node, TSet<UBehaviorTreeGraphNode*>& Visited)
	{
		Visited.Add(Node);
		TSharedRef<FJsonObject> O = DumpNodeBase(Node);

		TArray<TSharedPtr<FJsonValue>> Decorators, Services, Children;
		for (UBehaviorTreeGraphNode* Sub : Node->Decorators)
		{
			if (Sub)
			{
				TSharedRef<FJsonObject> S = DumpNodeBase(Sub);
				if (Sub->bInjectedNode) { S->SetBoolField(TEXT("injected"), true); }
				Decorators.Add(MakeShared<FJsonValueObject>(S));
			}
		}
		for (UBehaviorTreeGraphNode* Sub : Node->Services)
		{
			if (Sub)
			{
				Services.Add(MakeShared<FJsonValueObject>(DumpNodeBase(Sub)));
			}
		}
		for (UBehaviorTreeGraphNode* Child : GetChildren(Node))
		{
			if (!Visited.Contains(Child))
			{
				Children.Add(MakeShared<FJsonValueObject>(DumpNode(Child, Visited)));
			}
		}
		O->SetArrayField(TEXT("decorators"), Decorators);
		O->SetArrayField(TEXT("services"), Services);
		O->SetArrayField(TEXT("children"), Children);
		return O;
	}
}

// =====================================================================================================
// Blackboard
// =====================================================================================================

FString UMCPInspectorBehaviorTreeToolset::CreateBlackboard(const FString& AssetPath, const FString& ParentBlackboardPath)
{
	using namespace MCPInspectorBehaviorTreeToolset;
	FString Error;
	if (!ValidateNewAssetPath(AssetPath, Error)) return Err(Error);

	UBlackboardData* Parent = nullptr;
	if (!ParentBlackboardPath.TrimStartAndEnd().IsEmpty())
	{
		Parent = LoadAsset<UBlackboardData>(ParentBlackboardPath);
		if (!Parent) return Err(TEXT("parent blackboard not found: ") + ParentBlackboardPath);
	}

	UPackage* Package = CreatePackage(*AssetPath);
	const FName AssetName(*FPackageName::GetShortName(AssetPath));
	// Same as UBlackboardDataFactory::FactoryCreateNew (plain NewObject).
	UBlackboardData* BB = NewObject<UBlackboardData>(Package, AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!BB) return Err(TEXT("NewObject<UBlackboardData> failed"));

	if (Parent)
	{
		FProperty* ParentProp = FindFProperty<FProperty>(UBlackboardData::StaticClass(), GET_MEMBER_NAME_CHECKED(UBlackboardData, Parent));
		BB->PreEditChange(ParentProp);
		BB->Parent = Parent;
		FPropertyChangedEvent Event(ParentProp, EPropertyChangeType::ValueSet);
		BB->PostEditChangeProperty(Event); // cycle check + UpdateParentKeys + UpdateIfHasSynchronizedKeys
	}

	FAssetRegistryModule::AssetCreated(BB);
	Package->MarkPackageDirty();
	return BB->GetPathName();
}

FString UMCPInspectorBehaviorTreeToolset::AddBlackboardKey(const FString& BlackboardPath, const FString& KeyName, const FString& KeyType, const FString& BaseClassOrEnumPath, bool bInstanceSynced)
{
	using namespace MCPInspectorBehaviorTreeToolset;
	UBlackboardData* BB = LoadAsset<UBlackboardData>(BlackboardPath);
	if (!BB) return Err(TEXT("blackboard not found: ") + BlackboardPath);

	const FString TrimmedName = KeyName.TrimStartAndEnd();
	if (TrimmedName.IsEmpty()) return Err(TEXT("empty key name"));
	const FName KeyFName(*TrimmedName);
	if (BB->GetKeyID(KeyFName) != FBlackboard::InvalidKey) return Err(TEXT("key already exists (here or in parent chain): ") + TrimmedName);

	const FString Type = KeyType.TrimStartAndEnd().ToLower();
	const FString Ref = BaseClassOrEnumPath.TrimStartAndEnd();
	UClass* KeyClass = nullptr;
	if (Type == TEXT("bool")) KeyClass = UBlackboardKeyType_Bool::StaticClass();
	else if (Type == TEXT("int")) KeyClass = UBlackboardKeyType_Int::StaticClass();
	else if (Type == TEXT("float")) KeyClass = UBlackboardKeyType_Float::StaticClass();
	else if (Type == TEXT("string")) KeyClass = UBlackboardKeyType_String::StaticClass();
	else if (Type == TEXT("name")) KeyClass = UBlackboardKeyType_Name::StaticClass();
	else if (Type == TEXT("vector")) KeyClass = UBlackboardKeyType_Vector::StaticClass();
	else if (Type == TEXT("rotator")) KeyClass = UBlackboardKeyType_Rotator::StaticClass();
	else if (Type == TEXT("object")) KeyClass = UBlackboardKeyType_Object::StaticClass();
	else if (Type == TEXT("class")) KeyClass = UBlackboardKeyType_Class::StaticClass();
	else if (Type == TEXT("enum")) KeyClass = UBlackboardKeyType_Enum::StaticClass();
	else return Err(TEXT("unknown key type: ") + KeyType + TEXT(" (bool,int,float,string,name,vector,rotator,object,class,enum)"));

	// Resolve type parameters before touching the asset.
	UClass* BaseClass = nullptr;
	UEnum* EnumType = nullptr;
	if (Type == TEXT("object") || Type == TEXT("class"))
	{
		if (!Ref.IsEmpty())
		{
			FString ClassError;
			BaseClass = ResolveNodeClass(Ref, UObject::StaticClass(), ClassError);
			if (!BaseClass) return Err(ClassError);
		}
	}
	else if (Type == TEXT("enum"))
	{
		if (Ref.IsEmpty()) return Err(TEXT("enum key requires an enum path in BaseClassOrEnumPath"));
		EnumType = Ref.StartsWith(TEXT("/Script/")) ? FindObject<UEnum>(nullptr, *Ref) : nullptr;
		if (!EnumType) EnumType = LoadAsset<UEnum>(Ref);
		if (!EnumType) return Err(TEXT("enum not found: ") + Ref);
	}

	// Same sequence as SBehaviorTreeBlackboardEditor::HandleKeyClassPicked.
	const FScopedTransaction Transaction(LOCTEXT("AddBBKey", "Add Blackboard Entry"));
	BB->SetFlags(RF_Transactional);
	BB->Modify();
	FProperty* KeysProp = FindFProperty<FProperty>(UBlackboardData::StaticClass(), GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys));
	BB->PreEditChange(KeysProp);

	FBlackboardEntry Entry;
	Entry.EntryName = KeyFName;
	Entry.KeyType = NewObject<UBlackboardKeyType>(BB, KeyClass);
	Entry.bInstanceSynced = bInstanceSynced ? 1 : 0;
	if (UBlackboardKeyType_Object* ObjKey = Cast<UBlackboardKeyType_Object>(Entry.KeyType))
	{
		if (BaseClass) ObjKey->BaseClass = BaseClass;
	}
	else if (UBlackboardKeyType_Class* ClassKey = Cast<UBlackboardKeyType_Class>(Entry.KeyType))
	{
		if (BaseClass) ClassKey->BaseClass = BaseClass;
	}
	else if (UBlackboardKeyType_Enum* EnumKey = Cast<UBlackboardKeyType_Enum>(Entry.KeyType))
	{
		EnumKey->EnumType = EnumType;
	}
	BB->Keys.Add(Entry);

	FPropertyChangedEvent Event(KeysProp, EPropertyChangeType::ArrayAdd);
	BB->PostEditChangeProperty(Event); // propagates to derived blackboards + OnBlackboardDataChanged
	BB->UpdateIfHasSynchronizedKeys();
	BB->MarkPackageDirty();
	return TEXT("OK");
}

FString UMCPInspectorBehaviorTreeToolset::RemoveBlackboardKey(const FString& BlackboardPath, const FString& KeyName)
{
	using namespace MCPInspectorBehaviorTreeToolset;
	UBlackboardData* BB = LoadAsset<UBlackboardData>(BlackboardPath);
	if (!BB) return Err(TEXT("blackboard not found: ") + BlackboardPath);

	const FName KeyFName(*KeyName.TrimStartAndEnd());
	const int32 Index = BB->Keys.IndexOfByPredicate([KeyFName](const FBlackboardEntry& E) { return E.EntryName == KeyFName; });
	if (Index == INDEX_NONE)
	{
		return BB->GetKeyID(KeyFName) != FBlackboard::InvalidKey
			? Err(TEXT("key is inherited from a parent blackboard; remove it there: ") + KeyName)
			: Err(TEXT("key not found: ") + KeyName);
	}

	// Same sequence as SBehaviorTreeBlackboardEditor::HandleDeleteEntry.
	const FScopedTransaction Transaction(LOCTEXT("RemoveBBKey", "Delete Blackboard Entry"));
	BB->SetFlags(RF_Transactional);
	BB->Modify();
	FProperty* KeysProp = FindFProperty<FProperty>(UBlackboardData::StaticClass(), GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys));
	BB->PreEditChange(KeysProp);
	BB->Keys.RemoveAt(Index);
	FPropertyChangedEvent Event(KeysProp, EPropertyChangeType::ArrayRemove);
	BB->PostEditChangeProperty(Event);
	BB->UpdateIfHasSynchronizedKeys();
	BB->MarkPackageDirty();
	return TEXT("OK");
}

// =====================================================================================================
// Behavior Tree
// =====================================================================================================

FString UMCPInspectorBehaviorTreeToolset::CreateBehaviorTree(const FString& AssetPath, const FString& BlackboardPath)
{
	using namespace MCPInspectorBehaviorTreeToolset;
	FString Error;
	if (!ValidateNewAssetPath(AssetPath, Error)) return Err(Error);

	UBlackboardData* BB = nullptr;
	if (!BlackboardPath.TrimStartAndEnd().IsEmpty())
	{
		BB = LoadAsset<UBlackboardData>(BlackboardPath);
		if (!BB) return Err(TEXT("blackboard not found: ") + BlackboardPath);
	}

	UPackage* Package = CreatePackage(*AssetPath);
	const FName AssetName(*FPackageName::GetShortName(AssetPath));
	// Same as UBehaviorTreeFactory::FactoryCreateNew (plain NewObject).
	UBehaviorTree* BT = NewObject<UBehaviorTree>(Package, AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!BT) return Err(TEXT("NewObject<UBehaviorTree> failed"));

	UBehaviorTreeGraph* Graph = GetOrCreateGraph(BT);
	UBehaviorTreeGraphNode_Root* Root = Graph ? FindRoot(Graph) : nullptr;
	if (!Root) return Err(TEXT("failed to create the behavior tree graph"));

	// Root::PostPlacedNewNode picks the AISystem default blackboard or *any loaded* blackboard; override explicitly.
	Root->BlackboardAsset = BB;
	Root->UpdateBlackboard();
	BT->BlackboardAsset = BB;
	Commit(BT, Graph);

	FAssetRegistryModule::AssetCreated(BT);
	Package->MarkPackageDirty();

	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("path"), BT->GetPathName());
	O->SetStringField(TEXT("rootId"), IdOf(Root));
	return ToJson(O);
}

FString UMCPInspectorBehaviorTreeToolset::AddBTNode(const FString& TreePath, const FString& ParentNodeId, const FString& NodeClass, int32 ChildIndex)
{
	using namespace MCPInspectorBehaviorTreeToolset;
	FString Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* Graph = LoadTreeGraph(TreePath, BT, Error);
	if (!Graph) return Err(Error);

	UBehaviorTreeGraphNode* SubOwner = nullptr;
	UBehaviorTreeGraphNode* Parent = FindNode(Graph, ParentNodeId, &SubOwner);
	if (!Parent) return Err(TEXT("parent node not found: ") + ParentNodeId);
	if (SubOwner) return Err(TEXT("parent is a decorator/service; use a composite or ROOT"));

	const bool bParentIsRoot = Parent->IsA(UBehaviorTreeGraphNode_Root::StaticClass());
	const bool bParentIsComposite = Parent->IsA(UBehaviorTreeGraphNode_Composite::StaticClass());
	if (!bParentIsRoot && !bParentIsComposite) return Err(TEXT("parent must be ROOT or a composite node (tasks cannot have children)"));

	UClass* Class = ResolveNodeClass(NodeClass, UBTNode::StaticClass(), Error);
	if (!Class) return Err(Error);
	const bool bIsComposite = Class->IsChildOf(UBTCompositeNode::StaticClass());
	const bool bIsTask = Class->IsChildOf(UBTTaskNode::StaticClass());
	if (!bIsComposite && !bIsTask) return Err(TEXT("node class must be a composite or task (use AddBTAuxNode for decorators/services): ") + Class->GetPathName());

	// Pick the output pin (pin categories are enforced again by UEdGraphSchema_BehaviorTree::CanCreateConnection).
	UEdGraphPin* OutPin = nullptr;
	int32 InsertIndex = ChildIndex;
	if (bParentIsRoot)
	{
		if (!bIsComposite) return Err(TEXT("ROOT accepts only a composite node"));
		OutPin = Parent->GetOutputPin(0);
		if (OutPin && OutPin->LinkedTo.Num() > 0) return Err(TEXT("ROOT already has a composite; remove it first or add under it"));
		InsertIndex = 0;
	}
	else if (IsSimpleParallel(Parent))
	{
		UEdGraphPin* MainPin = Parent->GetOutputPin(0);
		UEdGraphPin* BackgroundPin = Parent->GetOutputPin(1);
		int32 Slot = ChildIndex;
		if (Slot < 0)
		{
			Slot = (bIsTask && MainPin && MainPin->LinkedTo.Num() == 0) ? 0 : 1;
		}
		if (Slot == 0 && !bIsTask) return Err(TEXT("SimpleParallel main slot (index 0) accepts only a task"));
		OutPin = (Slot == 0) ? MainPin : BackgroundPin;
		if (!OutPin) return Err(TEXT("SimpleParallel pin not found"));
		if (OutPin->LinkedTo.Num() > 0) return Err(FString::Printf(TEXT("SimpleParallel slot %d is already used"), Slot));
		InsertIndex = 0;
	}
	else
	{
		OutPin = Parent->GetOutputPin(0);
	}
	if (!OutPin) return Err(TEXT("parent has no output pin"));

	// Graph node class: same mapping as UEdGraphSchema_BehaviorTree::GetGraphContextActions.
	UClass* GraphNodeClass = nullptr;
	if (bIsComposite)
	{
		if (Class->IsChildOf(UBTComposite_SimpleParallel::StaticClass()))
		{
			// UBehaviorTreeGraphNode_SimpleParallel is not exported (UCLASS() without MinimalAPI): find it by path.
			GraphNodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_SimpleParallel"));
			if (!GraphNodeClass) return Err(TEXT("BehaviorTreeGraphNode_SimpleParallel class not found"));
		}
		else
		{
			GraphNodeClass = UBehaviorTreeGraphNode_Composite::StaticClass();
		}
	}
	else
	{
		GraphNodeClass = (Class == UBTTask_RunBehavior::StaticClass())
			? UBehaviorTreeGraphNode_SubtreeTask::StaticClass()
			: UBehaviorTreeGraphNode_Task::StaticClass();
	}

	// Position: X order among siblings on this pin = execution order.
	TArray<UBehaviorTreeGraphNode*> Siblings = GetPinChildren(OutPin);
	if (InsertIndex < 0 || InsertIndex > Siblings.Num())
	{
		InsertIndex = Siblings.Num();
	}
	int32 PosX = Parent->NodePosX;
	if (Parent && IsSimpleParallel(Parent) && OutPin == Parent->GetOutputPin(1))
	{
		PosX += SpacingX;
	}
	if (Siblings.Num() > 0)
	{
		if (InsertIndex >= Siblings.Num())
		{
			PosX = SubtreeMaxX(Siblings.Last()) + SpacingX;
		}
		else
		{
			PosX = Siblings[InsertIndex]->NodePosX;
		}
	}
	const int32 PosY = Parent->NodePosY + SpacingY + 40 * (Parent->Decorators.Num() + Parent->Services.Num());

	const FScopedTransaction Transaction(LOCTEXT("AddBTNode", "Add Behavior Tree Node"));
	BT->Modify();
	Graph->Modify();

	if (InsertIndex < Siblings.Num())
	{
		for (int32 i = InsertIndex; i < Siblings.Num(); ++i)
		{
			ShiftSubtreeX(Siblings[i], SpacingX);
		}
	}

	// Same steps as FAISchemaAction_NewNode::PerformAction: add node, new guid, PostPlacedNewNode (creates NodeInstance from ClassData), pins.
	FGraphNodeCreator<UBehaviorTreeGraphNode> Creator(*Graph);
	UBehaviorTreeGraphNode* NewNode = Creator.CreateNode(/*bSelectNewNode*/ false, GraphNodeClass);
	UAIGraphNode::UpdateNodeClassDataFrom(Class, NewNode->ClassData);
	NewNode->NodePosX = PosX;
	NewNode->NodePosY = PosY;
	Creator.Finalize();

	if (!NewNode->NodeInstance)
	{
		NewNode->DestroyNode();
		Commit(BT, Graph);
		return Err(TEXT("node instance was not created for class ") + Class->GetPathName());
	}

	UEdGraphPin* InPin = NewNode->GetInputPin();
	if (!InPin || !Graph->GetSchema()->TryCreateConnection(OutPin, InPin))
	{
		NewNode->DestroyNode();
		Commit(BT, Graph);
		return Err(TEXT("connection refused by the behavior tree schema"));
	}

	Commit(BT, Graph);
	return IdOf(NewNode);
}

FString UMCPInspectorBehaviorTreeToolset::AddBTAuxNode(const FString& TreePath, const FString& NodeId, const FString& AuxClass)
{
	using namespace MCPInspectorBehaviorTreeToolset;
	FString Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* Graph = LoadTreeGraph(TreePath, BT, Error);
	if (!Graph) return Err(Error);

	UBehaviorTreeGraphNode* SubOwner = nullptr;
	UBehaviorTreeGraphNode* Target = FindNode(Graph, NodeId, &SubOwner);
	if (!Target || SubOwner) return Err(TEXT("composite/task node not found: ") + NodeId);
	if (!Target->IsA(UBehaviorTreeGraphNode_Composite::StaticClass()) && !Target->IsA(UBehaviorTreeGraphNode_Task::StaticClass()))
	{
		return Err(TEXT("decorators/services can only be attached to composite or task nodes"));
	}

	UClass* Class = ResolveNodeClass(AuxClass, UBTNode::StaticClass(), Error);
	if (!Class) return Err(Error);

	UClass* SubGraphClass = nullptr;
	if (Class->IsChildOf(UBTDecorator::StaticClass())) SubGraphClass = UBehaviorTreeGraphNode_Decorator::StaticClass();
	else if (Class->IsChildOf(UBTService::StaticClass())) SubGraphClass = UBehaviorTreeGraphNode_Service::StaticClass();
	else return Err(TEXT("aux class must derive from BTDecorator or BTService: ") + Class->GetPathName());

	const FScopedTransaction Transaction(LOCTEXT("AddBTAux", "Add Behavior Tree Decorator/Service"));
	BT->Modify();

	// Same as UAIGraphSchema::GetGraphNodeContextActions + FAISchemaAction_NewSubNode::PerformAction.
	UBehaviorTreeGraphNode* Sub = NewObject<UBehaviorTreeGraphNode>(Graph, SubGraphClass);
	UAIGraphNode::UpdateNodeClassDataFrom(Class, Sub->ClassData);
	Target->AddSubNode(Sub, Graph); // CreateNewGuid, PostPlacedNewNode, OnSubNodeAdded (Decorators/Services), UpdateAsset

	if (!Sub->NodeInstance)
	{
		Sub->DestroyNode();
		Commit(BT, Graph);
		return Err(TEXT("node instance was not created for class ") + Class->GetPathName());
	}

	Commit(BT, Graph);
	return IdOf(Sub);
}

FString UMCPInspectorBehaviorTreeToolset::SetBTNodeProperty(const FString& TreePath, const FString& NodeId, const FString& PropertyPath, const FString& ValueText)
{
	using namespace MCPInspectorBehaviorTreeToolset;
	FString Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* Graph = LoadTreeGraph(TreePath, BT, Error);
	if (!Graph) return Err(Error);

	UBehaviorTreeGraphNode* Node = FindNode(Graph, NodeId);
	if (!Node) return Err(TEXT("node not found: ") + NodeId);

	// ROOT: only the Blackboard asset is editable.
	if (UBehaviorTreeGraphNode_Root* Root = Cast<UBehaviorTreeGraphNode_Root>(Node))
	{
		if (!PropertyPath.TrimStartAndEnd().Equals(TEXT("BlackboardAsset"), ESearchCase::IgnoreCase))
		{
			return Err(TEXT("ROOT only supports PropertyPath 'BlackboardAsset'"));
		}
		UBlackboardData* BB = nullptr;
		const FString Value = ValueText.TrimStartAndEnd();
		if (!Value.IsEmpty() && !Value.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			BB = LoadAsset<UBlackboardData>(Value);
			if (!BB) return Err(TEXT("blackboard not found: ") + Value);
		}
		const FScopedTransaction Transaction(LOCTEXT("SetBTRootBB", "Set Behavior Tree Blackboard"));
		BT->Modify();
		Root->Modify();
		Root->BlackboardAsset = BB;
		Root->UpdateBlackboard();
		Commit(BT, Graph);
		return TEXT("OK");
	}

	UObject* Instance = Node->NodeInstance;
	if (!Instance) return Err(TEXT("node has no instance (missing class?)"));

	FProperty* Prop = nullptr;
	FProperty* TopProp = nullptr;
	void* ValuePtr = nullptr;
	if (!ResolvePath(Instance->GetClass(), Instance, PropertyPath.TrimStartAndEnd(), Prop, ValuePtr, TopProp, Error)) return Err(Error);

	const FString Value = ValueText.TrimStartAndEnd();
	const FScopedTransaction Transaction(LOCTEXT("SetBTNodeProp", "Set Behavior Tree Node Property"));
	BT->Modify();
	Instance->Modify();
	Instance->PreEditChange(TopProp);

	FStructProperty* StructProp = CastField<FStructProperty>(Prop);
	if (StructProp && StructProp->Struct == FBlackboardKeySelector::StaticStruct() && !Value.StartsWith(TEXT("(")))
	{
		// Key selector by key name.
		UBlackboardData* BB = BT->BlackboardAsset;
		if (!BB)
		{
			FPropertyChangedEvent Cancel(Prop);
			Cancel.SetActiveMemberProperty(TopProp);
			Instance->PostEditChangeProperty(Cancel);
			return Err(TEXT("behavior tree has no blackboard asset"));
		}
		FBlackboardKeySelector* Selector = static_cast<FBlackboardKeySelector*>(ValuePtr);
		const FName KeyFName(*Value);
		if (KeyFName.IsNone() || Value.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			Selector->SelectedKeyName = NAME_None;
			Selector->InvalidateResolvedKey();
		}
		else
		{
			const FBlackboard::FKey KeyID = BB->GetKeyID(KeyFName);
			if (KeyID == FBlackboard::InvalidKey)
			{
				FPropertyChangedEvent Cancel(Prop);
				Cancel.SetActiveMemberProperty(TopProp);
				Instance->PostEditChangeProperty(Cancel);
				return Err(FString::Printf(TEXT("key '%s' not found in blackboard %s"), *Value, *BB->GetPathName()));
			}
			const FBlackboardEntry* Entry = BB->GetKey(KeyID);
			if (Selector->AllowedTypes.Num() > 0 && Entry && Entry->KeyType)
			{
				bool bAllowed = false;
				for (UBlackboardKeyType* Filter : Selector->AllowedTypes)
				{
					if (Filter && Entry->KeyType->IsAllowedByFilter(Filter))
					{
						bAllowed = true;
						break;
					}
				}
				if (!bAllowed)
				{
					FPropertyChangedEvent Cancel(Prop);
					Cancel.SetActiveMemberProperty(TopProp);
					Instance->PostEditChangeProperty(Cancel);
					return Err(FString::Printf(TEXT("key '%s' (%s) is not allowed by this selector's type filter"), *Value, *Entry->KeyType->GetClass()->GetName()));
				}
			}
			Selector->SelectedKeyName = KeyFName;
			Selector->ResolveSelectedKey(*BB);
		}
	}
	else if (StructProp && StructProp->Struct->IsChildOf(FValueOrBlackboardKeyBase::StaticStruct()) && !Value.StartsWith(TEXT("(")))
	{
		// FValueOrBBKey_* (e.g. BTTask_Wait::WaitTime, BTTask_MoveTo::AcceptableRadius):
		// "bb:KeyName" binds the blackboard key, anything else sets DefaultValue and clears the key.
		FNameProperty* KeyProp = FindFProperty<FNameProperty>(StructProp->Struct, TEXT("Key"));
		if (!KeyProp)
		{
			FPropertyChangedEvent Cancel(Prop);
			Cancel.SetActiveMemberProperty(TopProp);
			Instance->PostEditChangeProperty(Cancel);
			return Err(TEXT("FValueOrBlackboardKeyBase::Key not found"));
		}
		FName* KeyPtr = KeyProp->ContainerPtrToValuePtr<FName>(ValuePtr);
		FString Failure;
		if (Value.StartsWith(TEXT("bb:"), ESearchCase::IgnoreCase))
		{
			const FName KeyFName(*Value.RightChop(3).TrimStartAndEnd());
			UBlackboardData* BB = BT->BlackboardAsset;
			const FBlackboard::FKey KeyID = BB ? BB->GetKeyID(KeyFName) : FBlackboard::InvalidKey;
			const FBlackboardEntry* Entry = BB ? BB->GetKey(KeyID) : nullptr;
			if (!Entry || !Entry->KeyType)
			{
				Failure = FString::Printf(TEXT("key '%s' not found in the tree's blackboard"), *KeyFName.ToString());
			}
			else if (!static_cast<FValueOrBlackboardKeyBase*>(ValuePtr)->IsCompatibleType(Entry->KeyType))
			{
				Failure = FString::Printf(TEXT("key '%s' (%s) is not compatible with %s"), *KeyFName.ToString(), *Entry->KeyType->GetClass()->GetName(), *StructProp->Struct->GetName());
			}
			else
			{
				*KeyPtr = KeyFName;
			}
		}
		else
		{
			FProperty* DefaultProp = FindFProperty<FProperty>(StructProp->Struct, TEXT("DefaultValue"));
			if (!DefaultProp)
			{
				Failure = TEXT("DefaultValue not found on ") + StructProp->Struct->GetName();
			}
			else if (!DefaultProp->ImportText_Direct(*Value, DefaultProp->ContainerPtrToValuePtr<void>(ValuePtr), Instance, PPF_None))
			{
				Failure = FString::Printf(TEXT("ImportText failed for %s.DefaultValue = %s"), *PropertyPath, *Value);
			}
			else
			{
				*KeyPtr = NAME_None;
			}
		}
		if (!Failure.IsEmpty())
		{
			FPropertyChangedEvent Cancel(Prop);
			Cancel.SetActiveMemberProperty(TopProp);
			Instance->PostEditChangeProperty(Cancel);
			return Err(Failure);
		}
	}
	else
	{
		// Preload asset references so ImportText can resolve them.
		if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			if (Value.StartsWith(TEXT("/")))
			{
				StaticLoadObject(ObjProp->PropertyClass, nullptr, *ToObjectPath(Value));
			}
		}
		if (!Prop->ImportText_Direct(*Value, ValuePtr, Instance, PPF_None))
		{
			FPropertyChangedEvent Cancel(Prop);
			Cancel.SetActiveMemberProperty(TopProp);
			Instance->PostEditChangeProperty(Cancel);
			return Err(FString::Printf(TEXT("ImportText failed for %s = %s"), *PropertyPath, *ValueText));
		}
	}

	FPropertyChangedEvent Event(Prop, EPropertyChangeType::ValueSet);
	Event.SetActiveMemberProperty(TopProp);
	Instance->PostEditChangeProperty(Event);

	Commit(BT, Graph); // UpdateAsset -> UpdateBlackboardChange -> InitializeFromAsset re-resolves key selectors
	return TEXT("OK");
}

FString UMCPInspectorBehaviorTreeToolset::RemoveBTNode(const FString& TreePath, const FString& NodeId)
{
	using namespace MCPInspectorBehaviorTreeToolset;
	FString Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* Graph = LoadTreeGraph(TreePath, BT, Error);
	if (!Graph) return Err(Error);

	UBehaviorTreeGraphNode* Owner = nullptr;
	UBehaviorTreeGraphNode* Node = FindNode(Graph, NodeId, &Owner);
	if (!Node) return Err(TEXT("node not found: ") + NodeId);
	if (Node->IsA(UBehaviorTreeGraphNode_Root::StaticClass())) return Err(TEXT("ROOT cannot be removed"));
	if (Node->bInjectedNode) return Err(TEXT("injected nodes (from a subtree) cannot be removed here"));

	const FScopedTransaction Transaction(LOCTEXT("RemoveBTNode", "Remove Behavior Tree Node"));
	BT->Modify();
	Graph->Modify();

	if (Owner)
	{
		// Same as FAIGraphEditor::DeleteSelectedNodes on a sub node: DestroyNode -> ParentNode->RemoveSubNode.
		Node->ParentNode = Owner; // transient; make sure it is set even if UpdateAsset has not run since load
		Owner->Modify();
		Node->Modify();
		Node->DestroyNode();
	}
	else
	{
		TArray<UBehaviorTreeGraphNode*> Subtree;
		CollectSubtree(Node, Subtree);
		for (int32 i = Subtree.Num() - 1; i >= 0; --i)
		{
			Subtree[i]->Modify();
			Subtree[i]->DestroyNode();
		}
	}

	Commit(BT, Graph); // also renames orphaned node instances to the transient package (RemoveOrphanedNodes)
	return TEXT("OK");
}

FString UMCPInspectorBehaviorTreeToolset::DumpBehaviorTree(const FString& TreePath)
{
	using namespace MCPInspectorBehaviorTreeToolset;
	UBehaviorTree* BT = LoadAsset<UBehaviorTree>(TreePath);
	if (!BT) return Err(TEXT("behavior tree not found: ") + TreePath);

	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(BT->BTGraph);
	if (!Graph)
	{
		// A tree without editor graph (e.g. created by script): build it the way the BT editor would on open.
		Graph = GetOrCreateGraph(BT);
		if (!Graph) return Err(TEXT("could not create the behavior tree editor graph"));
	}
	UBehaviorTreeGraphNode_Root* Root = FindRoot(Graph);
	if (!Root) return Err(TEXT("ROOT node not found"));

	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("tree"), BT->GetPathName());
	O->SetStringField(TEXT("blackboard"), BT->BlackboardAsset ? BT->BlackboardAsset->GetPathName() : FString());

	TSet<UBehaviorTreeGraphNode*> Visited;
	TSharedRef<FJsonObject> RootJson = DumpNode(Root, Visited);
	O->SetObjectField(TEXT("root"), RootJson);

	TArray<TSharedPtr<FJsonValue>> Disconnected;
	for (UEdGraphNode* EdNode : Graph->Nodes)
	{
		UBehaviorTreeGraphNode* Node = Cast<UBehaviorTreeGraphNode>(EdNode);
		if (!Node || Visited.Contains(Node))
		{
			continue;
		}
		// Only report top-most disconnected nodes (no parent link).
		UEdGraphPin* InPin = Node->GetInputPin();
		if (InPin && InPin->LinkedTo.Num() > 0)
		{
			continue;
		}
		Disconnected.Add(MakeShared<FJsonValueObject>(DumpNode(Node, Visited)));
	}
	O->SetArrayField(TEXT("disconnected"), Disconnected);
	return ToJson(O);
}

#undef LOCTEXT_NAMESPACE
