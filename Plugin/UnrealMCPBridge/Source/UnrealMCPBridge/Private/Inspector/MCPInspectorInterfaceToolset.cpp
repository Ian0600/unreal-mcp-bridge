#include "MCPInspectorInterfaceToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Interface.h"
#include "UObject/Package.h"

namespace MCPInspectorInterfaceToolset
{
	static FString Err(const FString& Msg) { return TEXT("ERROR|") + Msg; }

	// "/Game/A/B" -> "/Game/A/B.B"
	static FString ToObjectPath(const FString& Path)
	{
		return Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPackageName::GetShortName(Path);
	}

	static UBlueprint* LoadBlueprint(const FString& Path)
	{
		return LoadObject<UBlueprint>(nullptr, *ToObjectPath(Path));
	}

	// Resolve a Blueprint Interface asset path or a native interface class path to the interface UClass.
	static UClass* ResolveInterfaceClass(const FString& Path)
	{
		if (Path.StartsWith(TEXT("/Script/")))
		{
			UClass* Native = LoadObject<UClass>(nullptr, *Path);
			return (Native && Native->HasAnyClassFlags(CLASS_Interface)) ? Native : nullptr;
		}
		if (UBlueprint* BP = LoadBlueprint(Path))
		{
			return (BP->BlueprintType == BPTYPE_Interface) ? BP->GeneratedClass.Get() : nullptr;
		}
		return nullptr;
	}

	static bool ParsePinType(FString TypeStr, FEdGraphPinType& Out, FString& OutError)
	{
		Out = FEdGraphPinType();
		TypeStr.TrimStartAndEndInline();
		if (TypeStr.StartsWith(TEXT("array:"), ESearchCase::IgnoreCase))
		{
			if (!ParsePinType(TypeStr.RightChop(6), Out, OutError)) return false;
			Out.ContainerType = EPinContainerType::Array;
			return true;
		}
		FString Kind = TypeStr, Ref;
		TypeStr.Split(TEXT(":"), &Kind, &Ref);
		Kind = Kind.ToLower();

		auto SetStruct = [&Out](UScriptStruct* S) { Out.PinCategory = UEdGraphSchema_K2::PC_Struct; Out.PinSubCategoryObject = S; };
		if (Kind == TEXT("bool")) Out.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		else if (Kind == TEXT("byte")) Out.PinCategory = UEdGraphSchema_K2::PC_Byte;
		else if (Kind == TEXT("int")) Out.PinCategory = UEdGraphSchema_K2::PC_Int;
		else if (Kind == TEXT("int64")) Out.PinCategory = UEdGraphSchema_K2::PC_Int64;
		else if (Kind == TEXT("float")) { Out.PinCategory = UEdGraphSchema_K2::PC_Real; Out.PinSubCategory = UEdGraphSchema_K2::PC_Float; }
		else if (Kind == TEXT("double")) { Out.PinCategory = UEdGraphSchema_K2::PC_Real; Out.PinSubCategory = UEdGraphSchema_K2::PC_Double; }
		else if (Kind == TEXT("string")) Out.PinCategory = UEdGraphSchema_K2::PC_String;
		else if (Kind == TEXT("name")) Out.PinCategory = UEdGraphSchema_K2::PC_Name;
		else if (Kind == TEXT("text")) Out.PinCategory = UEdGraphSchema_K2::PC_Text;
		else if (Kind == TEXT("vector")) SetStruct(TBaseStructure<FVector>::Get());
		else if (Kind == TEXT("rotator")) SetStruct(TBaseStructure<FRotator>::Get());
		else if (Kind == TEXT("transform")) SetStruct(TBaseStructure<FTransform>::Get());
		else if (Kind == TEXT("linearcolor")) SetStruct(TBaseStructure<FLinearColor>::Get());
		else if (Kind == TEXT("object") || Kind == TEXT("class") || Kind == TEXT("softobject"))
		{
			UClass* Class = LoadObject<UClass>(nullptr, *Ref);
			if (!Class)
			{
				if (UBlueprint* BP = LoadBlueprint(Ref)) Class = BP->GeneratedClass;
			}
			if (!Class) { OutError = FString::Printf(TEXT("class not found: %s"), *Ref); return false; }
			Out.PinCategory = Kind == TEXT("object") ? UEdGraphSchema_K2::PC_Object
				: Kind == TEXT("class") ? UEdGraphSchema_K2::PC_Class : UEdGraphSchema_K2::PC_SoftObject;
			Out.PinSubCategoryObject = Class;
		}
		else if (Kind == TEXT("struct"))
		{
			UScriptStruct* S = LoadObject<UScriptStruct>(nullptr, *ToObjectPath(Ref));
			if (!S) { OutError = FString::Printf(TEXT("struct not found: %s"), *Ref); return false; }
			SetStruct(S);
		}
		else if (Kind == TEXT("enum"))
		{
			UEnum* E = LoadObject<UEnum>(nullptr, *ToObjectPath(Ref));
			if (!E) { OutError = FString::Printf(TEXT("enum not found: %s"), *Ref); return false; }
			Out.PinCategory = UEdGraphSchema_K2::PC_Byte;
			Out.PinSubCategoryObject = E;
		}
		else { OutError = FString::Printf(TEXT("unknown type: %s"), *TypeStr); return false; }
		return true;
	}

	// "A:int,B:object:/Script/Engine.Actor" -> [(A,int),(B,object:/Script/Engine.Actor)]
	static bool ParseParams(const FString& Spec, TArray<TPair<FName, FEdGraphPinType>>& Out, FString& OutError)
	{
		TArray<FString> Items;
		Spec.ParseIntoArray(Items, TEXT(","), true);
		for (FString Item : Items)
		{
			Item.TrimStartAndEndInline();
			FString Name, Type;
			if (!Item.Split(TEXT(":"), &Name, &Type)) { OutError = FString::Printf(TEXT("bad parameter '%s' (expected Name:Type)"), *Item); return false; }
			FEdGraphPinType PinType;
			if (!ParsePinType(Type, PinType, OutError)) return false;
			Out.Emplace(FName(*Name.TrimStartAndEnd()), PinType);
		}
		return true;
	}

	static FString ToJson(const TArray<TSharedPtr<FJsonValue>>& Arr)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Arr, Writer);
		return Out;
	}
}

FString UMCPInspectorInterfaceToolset::CreateBlueprintInterface(const FString& AssetPath)
{
	using namespace MCPInspectorInterfaceToolset;
	if (!FPackageName::IsValidLongPackageName(AssetPath)) return Err(TEXT("invalid package path: ") + AssetPath);
	if (FPackageName::DoesPackageExist(AssetPath) || FindPackage(nullptr, *AssetPath)) return Err(TEXT("asset already exists: ") + AssetPath);

	UPackage* Package = CreatePackage(*AssetPath);
	const FName AssetName(*FPackageName::GetShortName(AssetPath));
	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(UInterface::StaticClass(), Package, AssetName, BPTYPE_Interface,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), FName(TEXT("BlueprintInspector")));
	if (!BP) return Err(TEXT("CreateBlueprint failed"));

	FAssetRegistryModule::AssetCreated(BP);
	Package->MarkPackageDirty();
	return BP->GetPathName();
}

FString UMCPInspectorInterfaceToolset::AddInterfaceFunction(const FString& InterfacePath, const FString& FunctionName, const FString& Inputs, const FString& Outputs)
{
	using namespace MCPInspectorInterfaceToolset;
	UBlueprint* BP = LoadBlueprint(InterfacePath);
	if (!BP || BP->BlueprintType != BPTYPE_Interface) return Err(TEXT("not a Blueprint Interface: ") + InterfacePath);

	const FName FuncName(*FunctionName);
	for (UEdGraph* Existing : BP->FunctionGraphs)
	{
		if (Existing && Existing->GetFName() == FuncName) return Err(TEXT("function already exists: ") + FunctionName);
	}

	TArray<TPair<FName, FEdGraphPinType>> InParams, OutParams;
	FString ParseError;
	if (!ParseParams(Inputs, InParams, ParseError) || !ParseParams(Outputs, OutParams, ParseError)) return Err(ParseError);

	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(BP, FuncName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, Graph, /*bIsUserCreated*/ true, nullptr);

	TArray<UK2Node_FunctionEntry*> Entries;
	Graph->GetNodesOfClass(Entries);
	if (Entries.Num() == 0) return Err(TEXT("function entry node not created"));
	UK2Node_FunctionEntry* Entry = Entries[0];

	for (const TPair<FName, FEdGraphPinType>& P : InParams)
	{
		Entry->CreateUserDefinedPin(P.Key, P.Value, EGPD_Output);
	}
	if (OutParams.Num() > 0)
	{
		UK2Node_FunctionResult* Result = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entry);
		if (!Result) return Err(TEXT("function result node not created"));
		for (const TPair<FName, FEdGraphPinType>& P : OutParams)
		{
			Result->CreateUserDefinedPin(P.Key, P.Value, EGPD_Input);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP);
	return TEXT("OK");
}

FString UMCPInspectorInterfaceToolset::ImplementInterface(const FString& BlueprintPath, const FString& InterfacePath)
{
	using namespace MCPInspectorInterfaceToolset;
	UBlueprint* BP = LoadBlueprint(BlueprintPath);
	if (!BP) return Err(TEXT("blueprint not found: ") + BlueprintPath);
	UClass* Interface = ResolveInterfaceClass(InterfacePath);
	if (!Interface) return Err(TEXT("interface not found: ") + InterfacePath);

	if (!FBlueprintEditorUtils::ImplementNewInterface(BP, Interface->GetClassPathName()))
	{
		return Err(TEXT("ImplementNewInterface failed (already implemented or incompatible)"));
	}
	FKismetEditorUtilities::CompileBlueprint(BP);
	return TEXT("OK");
}

FString UMCPInspectorInterfaceToolset::RemoveInterface(const FString& BlueprintPath, const FString& InterfacePath, bool bPreserveFunctions)
{
	using namespace MCPInspectorInterfaceToolset;
	UBlueprint* BP = LoadBlueprint(BlueprintPath);
	if (!BP) return Err(TEXT("blueprint not found: ") + BlueprintPath);
	UClass* Interface = ResolveInterfaceClass(InterfacePath);
	if (!Interface) return Err(TEXT("interface not found: ") + InterfacePath);

	const bool bImplemented = BP->ImplementedInterfaces.ContainsByPredicate(
		[Interface](const FBPInterfaceDescription& D) { return D.Interface == Interface; });
	if (!bImplemented) return Err(TEXT("interface is not implemented directly by this blueprint"));

	FBlueprintEditorUtils::RemoveInterface(BP, Interface->GetClassPathName(), bPreserveFunctions);
	FKismetEditorUtilities::CompileBlueprint(BP);
	return TEXT("OK");
}

FString UMCPInspectorInterfaceToolset::ListImplementedInterfaces(const FString& BlueprintPath)
{
	using namespace MCPInspectorInterfaceToolset;
	UBlueprint* BP = LoadBlueprint(BlueprintPath);
	if (!BP) return Err(TEXT("blueprint not found: ") + BlueprintPath);

	TArray<TSharedPtr<FJsonValue>> Arr;
	TSet<UClass*> Direct;
	for (const FBPInterfaceDescription& D : BP->ImplementedInterfaces)
	{
		if (!D.Interface) continue;
		Direct.Add(D.Interface);
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("interface"), D.Interface->GetPathName());
		O->SetBoolField(TEXT("inherited"), false);
		TArray<TSharedPtr<FJsonValue>> Graphs;
		for (UEdGraph* G : D.Graphs) if (G) Graphs.Add(MakeShared<FJsonValueString>(G->GetName()));
		O->SetArrayField(TEXT("graphs"), Graphs);
		Arr.Add(MakeShared<FJsonValueObject>(O));
	}
	TArray<UClass*> All;
	FBlueprintEditorUtils::FindImplementedInterfaces(BP, /*bGetAllInterfaces*/ true, All);
	for (UClass* C : All)
	{
		if (!C || Direct.Contains(C)) continue;
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("interface"), C->GetPathName());
		O->SetBoolField(TEXT("inherited"), true);
		O->SetArrayField(TEXT("graphs"), {});
		Arr.Add(MakeShared<FJsonValueObject>(O));
	}
	return ToJson(Arr);
}
