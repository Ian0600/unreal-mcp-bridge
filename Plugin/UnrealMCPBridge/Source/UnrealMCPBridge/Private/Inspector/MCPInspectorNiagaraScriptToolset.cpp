#include "MCPInspectorNiagaraScriptToolset.h"

#include "AssetToolsModule.h"
#include "BPInspectorCommon.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "Factories/Factory.h"
#include "IAssetTools.h"
#include "INiagaraEditorTypeUtilities.h"
#include "Misc/FeedbackContext.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "NiagaraCommon.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEditorModule.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOp.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScratchPadContainer.h"
#include "NiagaraScript.h"
#include "NiagaraScriptFactoryNew.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSystem.h"
#include "NiagaraTypeRegistry.h"
#include "NiagaraTypes.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

// NiagaraEditor/Private (reachable through the PrivateIncludePaths entry in BlueprintInspector.Build.cs).
// Only used as types for Cast<> (UClass Z_Construct_* is exported) and for the exported
// FNiagaraStackGraphUtilities::AddNewVariableToParameterMapNode; no non-exported member is called.
#include "NiagaraNodeParameterMapBase.h"
#include "NiagaraNodeParameterMapGet.h"
#include "NiagaraNodeParameterMapSet.h"

#define LOCTEXT_NAMESPACE "MCPInspectorNiagaraScriptToolset"

namespace MCPInspectorNiagaraScriptToolset
{
	// UNiagaraNode::ReallocatePins is exported (NIAGARAEDITOR_API) but protected. Naming it through a derived
	// class yields a pointer-to-member of UNiagaraNode that may be invoked on any node (standard C++ rule).
	struct FReallocatePinsAccess : public UNiagaraNode
	{
		static bool (UNiagaraNode::*Get())(bool) { return &FReallocatePinsAccess::ReallocatePins; }
	};

	static bool CallReallocatePins(UNiagaraNode* Node)
	{
		return Node ? (Node->*FReallocatePinsAccess::Get())(true) : false;
	}

	static FString Err(const FString& Msg) { return TEXT("ERROR|") + Msg; }

	static FString IdStr(const FGuid& Guid) { return Guid.ToString(EGuidFormats::Digits); }

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

	// ---- enums / names --------------------------------------------------------------------------------

	static FString UsageName(ENiagaraScriptUsage Usage)
	{
		const UEnum* Enum = StaticEnum<ENiagaraScriptUsage>();
		return Enum ? Enum->GetNameStringByValue((int64)Usage) : FString::FromInt((int32)Usage);
	}

	/** "ParticleUpdate" / "ParticleUpdateScript" / "Module" -> enum (case-insensitive). */
	static bool ParseUsageName(const FString& InName, ENiagaraScriptUsage& Out)
	{
		const UEnum* Enum = StaticEnum<ENiagaraScriptUsage>();
		const FString Name = InName.TrimStartAndEnd();
		if (!Enum || Name.IsEmpty())
		{
			return false;
		}
		for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index) // last entry is _MAX
		{
			const FString Entry = Enum->GetNameStringByIndex(Index);
			if (Entry.Equals(Name, ESearchCase::IgnoreCase) || Entry.Equals(Name + TEXT("Script"), ESearchCase::IgnoreCase))
			{
				Out = (ENiagaraScriptUsage)Enum->GetValueByIndex(Index);
				return true;
			}
		}
		if (Name.Equals(TEXT("DI"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("Dynamic Input"), ESearchCase::IgnoreCase))
		{
			Out = ENiagaraScriptUsage::DynamicInput;
			return true;
		}
		return false;
	}

	static int32 UsageBit(ENiagaraScriptUsage Usage) { return 1 << (int32)Usage; }

	static bool ParseUsageMask(const FString& InText, int32& OutMask, FString& OutError)
	{
		const FString Text = InText.TrimStartAndEnd();
		if (Text.IsNumeric())
		{
			OutMask = FCString::Atoi(*Text);
			return true;
		}
		TArray<FString> Parts;
		Text.Replace(TEXT("|"), TEXT(",")).ParseIntoArray(Parts, TEXT(","), true);
		int32 Mask = 0;
		for (FString Part : Parts)
		{
			Part.TrimStartAndEndInline();
			if (Part.IsEmpty())
			{
				continue;
			}
			if (Part.Equals(TEXT("Particle"), ESearchCase::IgnoreCase))
			{
				Mask |= UsageBit(ENiagaraScriptUsage::ParticleSpawnScript) | UsageBit(ENiagaraScriptUsage::ParticleUpdateScript);
				continue;
			}
			if (Part.Equals(TEXT("Emitter"), ESearchCase::IgnoreCase))
			{
				Mask |= UsageBit(ENiagaraScriptUsage::EmitterSpawnScript) | UsageBit(ENiagaraScriptUsage::EmitterUpdateScript);
				continue;
			}
			if (Part.Equals(TEXT("System"), ESearchCase::IgnoreCase))
			{
				Mask |= UsageBit(ENiagaraScriptUsage::SystemSpawnScript) | UsageBit(ENiagaraScriptUsage::SystemUpdateScript);
				continue;
			}
			ENiagaraScriptUsage Usage;
			if (!ParseUsageName(Part, Usage))
			{
				OutError = TEXT("unknown script usage: ") + Part;
				return false;
			}
			Mask |= UsageBit(Usage);
		}
		if (Mask == 0)
		{
			OutError = TEXT("empty usage bitmask: ") + InText;
			return false;
		}
		OutMask = Mask;
		return true;
	}

	static TArray<TSharedPtr<FJsonValue>> UsageMaskNames(int32 Mask)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		const UEnum* Enum = StaticEnum<ENiagaraScriptUsage>();
		if (!Enum)
		{
			return Out;
		}
		for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
		{
			const int64 Value = Enum->GetValueByIndex(Index);
			if (Value >= 0 && Value < 32 && (Mask & (1 << Value)) != 0)
			{
				Out.Add(MakeShared<FJsonValueString>(Enum->GetNameStringByIndex(Index)));
			}
		}
		return Out;
	}

	static FString CompileStatusName(ENiagaraScriptCompileStatus Status)
	{
		const UEnum* Enum = StaticEnum<ENiagaraScriptCompileStatus>();
		return Enum ? Enum->GetNameStringByValue((int64)Status) : FString::FromInt((int32)Status);
	}

	static FString VisibilityName(ENiagaraScriptLibraryVisibility Visibility)
	{
		const UEnum* Enum = StaticEnum<ENiagaraScriptLibraryVisibility>();
		return Enum ? Enum->GetNameStringByValue((int64)Visibility) : FString::FromInt((int32)Visibility);
	}

	static FString DefaultModeName(ENiagaraDefaultMode Mode)
	{
		const UEnum* Enum = StaticEnum<ENiagaraDefaultMode>();
		return Enum ? Enum->GetNameStringByValue((int64)Mode) : FString::FromInt((int32)Mode);
	}

	// ---- types / values -------------------------------------------------------------------------------

	static FString TypeName(const FNiagaraTypeDefinition& Type)
	{
		return Type.IsValid() ? Type.GetName() : FString(TEXT("<invalid>"));
	}

	static bool ParseType(const FString& InText, FNiagaraTypeDefinition& Out, FString& OutError)
	{
		const FString Text = InText.TrimStartAndEnd();
		const FString L = Text.ToLower();
		if (L == TEXT("float")) { Out = FNiagaraTypeDefinition::GetFloatDef(); return true; }
		if (L == TEXT("int") || L == TEXT("int32") || L == TEXT("integer")) { Out = FNiagaraTypeDefinition::GetIntDef(); return true; }
		if (L == TEXT("bool") || L == TEXT("boolean")) { Out = FNiagaraTypeDefinition::GetBoolDef(); return true; }
		if (L == TEXT("vec2") || L == TEXT("vector2") || L == TEXT("vector2d") || L == TEXT("float2")) { Out = FNiagaraTypeDefinition::GetVec2Def(); return true; }
		if (L == TEXT("vec3") || L == TEXT("vector") || L == TEXT("vector3") || L == TEXT("float3")) { Out = FNiagaraTypeDefinition::GetVec3Def(); return true; }
		if (L == TEXT("vec4") || L == TEXT("vector4") || L == TEXT("float4")) { Out = FNiagaraTypeDefinition::GetVec4Def(); return true; }
		if (L == TEXT("color") || L == TEXT("linearcolor")) { Out = FNiagaraTypeDefinition::GetColorDef(); return true; }
		if (L == TEXT("quat") || L == TEXT("quaternion")) { Out = FNiagaraTypeDefinition::GetQuatDef(); return true; }
		if (L == TEXT("position")) { Out = FNiagaraTypeDefinition::GetPositionDef(); return true; }
		if (L == TEXT("matrix") || L == TEXT("matrix4")) { Out = FNiagaraTypeDefinition::GetMatrix4Def(); return true; }
		if (L == TEXT("map") || L == TEXT("parametermap")) { Out = FNiagaraTypeDefinition::GetParameterMapDef(); return true; }
		if (Text.IsEmpty())
		{
			OutError = TEXT("type is empty");
			return false;
		}

		UObject* Object = nullptr;
		if (Text.StartsWith(TEXT("/")))
		{
			Object = FindObject<UObject>(nullptr, *Text);
			if (!Object)
			{
				Object = LoadObject<UObject>(nullptr, *Text);
			}
		}
		else
		{
			if (TOptional<FNiagaraTypeDefinition> Registered = FNiagaraTypeRegistry::GetRegisteredTypeByName(FName(*Text)))
			{
				Out = Registered.GetValue();
				return Out.IsValid();
			}
			Object = FindFirstObject<UScriptStruct>(*Text, EFindFirstObjectOptions::NativeFirst);
			if (!Object) { Object = FindFirstObject<UEnum>(*Text, EFindFirstObjectOptions::NativeFirst); }
			if (!Object) { Object = FindFirstObject<UClass>(*Text, EFindFirstObjectOptions::NativeFirst); }
		}

		bool bNeedsRegistration = false;
		if (UEnum* Enum = Cast<UEnum>(Object))
		{
			Out = FNiagaraTypeDefinition(Enum);
			bNeedsRegistration = true;
		}
		else if (UScriptStruct* Struct = Cast<UScriptStruct>(Object))
		{
			Out = FNiagaraTypeRegistry::GetTypeForStruct(Struct);
			bNeedsRegistration = true;
		}
		else if (UClass* Class = Cast<UClass>(Object))
		{
			if (!Class->IsChildOf(UNiagaraDataInterface::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
			{
				OutError = TEXT("class is not a concrete Niagara data interface: ") + Text;
				return false;
			}
			Out = FNiagaraTypeDefinition(Class);
		}
		else
		{
			OutError = TEXT("unknown type (float, int, bool, vec2, vec3, vec4, color, quat, position, matrix, map, or an enum/struct/data interface path): ") + Text;
			return false;
		}
		if (!Out.IsValid())
		{
			OutError = TEXT("not a valid Niagara type: ") + Text;
			return false;
		}
		if (bNeedsRegistration && !FNiagaraTypeRegistry::GetRegisteredParameterTypes().Contains(Out))
		{
			OutError = TEXT("type is not registered as a Niagara parameter type (Project Settings > Niagara > Additional Parameter Types/Enums): ") + Text;
			return false;
		}
		return true;
	}

	static TSharedPtr<INiagaraEditorTypeUtilities, ESPMode::ThreadSafe> GetTypeUtilities(const FNiagaraTypeDefinition& Type)
	{
		return FNiagaraEditorModule::Get().GetTypeUtilities(Type);
	}

	/** Parse a pin-default string into an allocated variable through the type's editor utilities (same parser as the graph pins). */
	static bool ParseValue(const FNiagaraTypeDefinition& Type, const FString& Text, FNiagaraVariable& OutVar, FString& OutError)
	{
		OutVar = FNiagaraVariable(Type, FName(TEXT("Value")));
		OutVar.AllocateData();
		TSharedPtr<INiagaraEditorTypeUtilities, ESPMode::ThreadSafe> Utils = GetTypeUtilities(Type);
		if (!Utils.IsValid() || !Utils->CanHandlePinDefaults())
		{
			OutError = TEXT("type has no text default values: ") + TypeName(Type);
			return false;
		}
		if (!Utils->SetValueFromPinDefaultString(Text.TrimStartAndEnd(), OutVar))
		{
			OutError = FString::Printf(TEXT("cannot parse '%s' as %s"), *Text, *TypeName(Type));
			return false;
		}
		return true;
	}

	static FString ValueToString(const FNiagaraVariable& Var)
	{
		if (!Var.IsDataAllocated())
		{
			return FString();
		}
		TSharedPtr<INiagaraEditorTypeUtilities, ESPMode::ThreadSafe> Utils = GetTypeUtilities(Var.GetType());
		return (Utils.IsValid() && Utils->CanHandlePinDefaults()) ? Utils->GetPinDefaultStringFromValue(Var) : FString();
	}

	// ---- script / graph access ------------------------------------------------------------------------

	struct FScriptCtx
	{
		UNiagaraScript* Script = nullptr;
		UNiagaraGraph* Graph = nullptr;
	};

	static UNiagaraGraph* GetGraph(UNiagaraScript* Script)
	{
		UNiagaraScriptSource* Source = Script ? Cast<UNiagaraScriptSource>(Script->GetLatestSource()) : nullptr;
		return Source ? Source->NodeGraph.Get() : nullptr;
	}

	static bool LoadCtx(const FString& ScriptPath, FScriptCtx& Out, FString& OutError)
	{
		Out.Script = LoadAsset<UNiagaraScript>(ScriptPath);
		if (!Out.Script)
		{
			OutError = TEXT("Niagara script not found: ") + ScriptPath;
			return false;
		}
		if (!Out.Script->IsStandaloneScript())
		{
			OutError = FString::Printf(TEXT("not a module/function/dynamic input script (usage %s): %s"), *UsageName(Out.Script->GetUsage()), *ScriptPath);
			return false;
		}
		Out.Graph = GetGraph(Out.Script);
		if (!Out.Graph)
		{
			OutError = TEXT("script has no Niagara graph source: ") + ScriptPath;
			return false;
		}
		return true;
	}

	static bool IsScratchPad(const UNiagaraScript* Script)
	{
		const UObject* Outer = Script ? Script->GetOuter() : nullptr;
		return Outer && (Outer->IsA<UNiagaraSystem>() || Outer->IsA<UNiagaraScratchPadContainer>());
	}

	static FGuid LatestVersion(UNiagaraScript* Script)
	{
		const FVersionedNiagaraScriptData* Data = Script->GetLatestScriptData();
		return Data ? Data->Version.VersionGuid : FGuid();
	}

	/**
	 * Same notifications the graph editor raises after an edit: node changed (graph needs recompile), graph changed, package dirty.
	 * UNiagaraGraph::NotifyGraphNeedsRecompile is not exported and UEdGraph::NotifyGraphChanged(const FEdGraphEditAction&) is
	 * protected, so the recompile notification goes through the exported UNiagaraNode::MarkNodeRequiresSynchronization(.., true),
	 * using the output node when the edited node is gone (removed node / parameter).
	 */
	static void Touch(const FScriptCtx& Ctx, UEdGraphNode* Node = nullptr)
	{
		UNiagaraNode* NiagaraNode = Cast<UNiagaraNode>(Node);
		if (!NiagaraNode)
		{
			for (UEdGraphNode* Candidate : Ctx.Graph->Nodes)
			{
				if (UNiagaraNodeOutput* Output = Cast<UNiagaraNodeOutput>(Candidate))
				{
					NiagaraNode = Output;
					break;
				}
				if (!NiagaraNode)
				{
					NiagaraNode = Cast<UNiagaraNode>(Candidate);
				}
			}
		}
		if (NiagaraNode)
		{
			NiagaraNode->MarkNodeRequiresSynchronization(TEXT("MCPInspectorNiagaraScriptToolset edit"), true);
		}
		Ctx.Graph->NotifyGraphChanged();
		Ctx.Script->MarkPackageDirty();
	}

	static UEdGraphNode* FindNode(UNiagaraGraph* Graph, const FString& InId, FString& OutError)
	{
		const FString Id = InId.TrimStartAndEnd();
		FGuid Guid;
		const bool bIsGuid = FGuid::Parse(Id, Guid);
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && ((bIsGuid && Node->NodeGuid == Guid) || (!bIsGuid && Node->GetName() == Id)))
			{
				return Node;
			}
		}
		OutError = TEXT("node not found: ") + InId;
		return nullptr;
	}

	static bool IsAddPin(const UEdGraphPin* Pin)
	{
		// UNiagaraNodeWithDynamicPins::AddPinSubCategory ("DynamicAddPin", NiagaraNodeWithDynamicPins.cpp) is not exported.
		static const FName AddPinSubCategory(TEXT("DynamicAddPin"));
		return Pin && Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc && Pin->PinType.PinSubCategory == AddPinSubCategory;
	}

	/** Pin by PinId / PersistentGuid / name ("in:" / "out:" prefix forces the direction; otherwise PreferredDir wins ties). */
	static UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& InSpec, EEdGraphPinDirection PreferredDir, FString& OutError)
	{
		FString Spec = InSpec.TrimStartAndEnd();
		EEdGraphPinDirection ForcedDir = EGPD_MAX;
		if (Spec.StartsWith(TEXT("in:"), ESearchCase::IgnoreCase)) { ForcedDir = EGPD_Input; Spec.RightChopInline(3); }
		else if (Spec.StartsWith(TEXT("out:"), ESearchCase::IgnoreCase)) { ForcedDir = EGPD_Output; Spec.RightChopInline(4); }

		FGuid Guid;
		const bool bIsGuid = FGuid::Parse(Spec, Guid);
		TArray<UEdGraphPin*> Matches;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || IsAddPin(Pin) || (ForcedDir != EGPD_MAX && Pin->Direction != ForcedDir))
			{
				continue;
			}
			const bool bMatch = bIsGuid
				? (Pin->PinId == Guid || (Pin->PersistentGuid.IsValid() && Pin->PersistentGuid == Guid))
				: (Pin->PinName == FName(*Spec) || (!Pin->PinFriendlyName.IsEmpty() && Pin->PinFriendlyName.ToString().Equals(Spec, ESearchCase::IgnoreCase)));
			if (bMatch)
			{
				Matches.Add(Pin);
			}
		}
		if (Matches.Num() > 1)
		{
			TArray<UEdGraphPin*> Preferred = Matches.FilterByPredicate([PreferredDir](const UEdGraphPin* Pin) { return Pin->Direction == PreferredDir; });
			if (Preferred.Num() == 1)
			{
				return Preferred[0];
			}
			OutError = FString::Printf(TEXT("pin '%s' is ambiguous on node %s; use the pin id or an in:/out: prefix"), *InSpec, *IdStr(Node->NodeGuid));
			return nullptr;
		}
		if (Matches.Num() == 0)
		{
			OutError = FString::Printf(TEXT("pin '%s' not found on node %s"), *InSpec, *IdStr(Node->NodeGuid));
			return nullptr;
		}
		return Matches[0];
	}

	// ---- JSON -----------------------------------------------------------------------------------------

	static TSharedRef<FJsonObject> PinToJson(const UEdGraphPin* Pin)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), IdStr(Pin->PinId));
		O->SetStringField(TEXT("name"), Pin->PinName.ToString());
		O->SetStringField(TEXT("dir"), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));
		if (IsAddPin(Pin))
		{
			O->SetBoolField(TEXT("add"), true);
		}
		else
		{
			O->SetStringField(TEXT("type"), TypeName(UEdGraphSchema_Niagara::PinToTypeDefinition(Pin)));
			if (!Pin->bDefaultValueIsIgnored && Pin->LinkedTo.Num() == 0)
			{
				O->SetStringField(TEXT("default"), Pin->DefaultValue);
			}
		}
		if (Pin->bHidden) { O->SetBoolField(TEXT("hidden"), true); }
		if (Pin->bOrphanedPin) { O->SetBoolField(TEXT("orphaned"), true); }
		if (Pin->bDefaultValueIsReadOnly) { O->SetBoolField(TEXT("readOnlyDefault"), true); }
		TArray<TSharedPtr<FJsonValue>> Links;
		for (const UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (!Linked || !Linked->GetOwningNodeUnchecked())
			{
				continue;
			}
			TSharedRef<FJsonObject> L = MakeShared<FJsonObject>();
			L->SetStringField(TEXT("node"), IdStr(Linked->GetOwningNode()->NodeGuid));
			L->SetStringField(TEXT("pin"), IdStr(Linked->PinId));
			L->SetStringField(TEXT("pinName"), Linked->PinName.ToString());
			Links.Add(MakeShared<FJsonValueObject>(L));
		}
		O->SetArrayField(TEXT("links"), Links);
		return O;
	}

	static FString GetCustomHlslText(const UNiagaraNodeCustomHlsl* Node)
	{
		// UNiagaraNodeCustomHlsl::GetCustomHlsl() is not exported; CustomHlsl is a private UPROPERTY, read it through reflection.
		const FStrProperty* Prop = CastField<FStrProperty>(UNiagaraNodeCustomHlsl::StaticClass()->FindPropertyByName(TEXT("CustomHlsl")));
		return Prop ? Prop->GetPropertyValue_InContainer(Node) : FString();
	}

	static TSharedRef<FJsonObject> NodeToJson(const UEdGraphNode* Node)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), IdStr(Node->NodeGuid));
		O->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		O->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		O->SetNumberField(TEXT("x"), Node->NodePosX);
		O->SetNumberField(TEXT("y"), Node->NodePosY);
		if (const UNiagaraNodeOp* Op = Cast<UNiagaraNodeOp>(Node))
		{
			O->SetStringField(TEXT("op"), Op->OpName.ToString());
		}
		if (const UNiagaraNodeCustomHlsl* Hlsl = Cast<UNiagaraNodeCustomHlsl>(Node))
		{
			O->SetStringField(TEXT("hlsl"), GetCustomHlslText(Hlsl));
		}
		else if (const UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Node))
		{
			O->SetStringField(TEXT("functionScript"), Call->FunctionScript ? Call->FunctionScript->GetPathName() : FString());
			O->SetStringField(TEXT("functionName"), Call->GetFunctionName());
		}
		if (const UNiagaraNodeInput* Input = Cast<UNiagaraNodeInput>(Node))
		{
			O->SetStringField(TEXT("input"), Input->Input.GetName().ToString() + TEXT(":") + TypeName(Input->Input.GetType()));
		}
		if (const UNiagaraNodeOutput* Output = Cast<UNiagaraNodeOutput>(Node))
		{
			O->SetStringField(TEXT("outputUsage"), UsageName(Output->GetUsage()));
			TArray<TSharedPtr<FJsonValue>> Outs;
			for (const FNiagaraVariable& Var : Output->Outputs)
			{
				Outs.Add(MakeShared<FJsonValueString>(Var.GetName().ToString() + TEXT(":") + TypeName(Var.GetType())));
			}
			O->SetArrayField(TEXT("outputs"), Outs);
		}
		TArray<TSharedPtr<FJsonValue>> Pins;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin)
			{
				Pins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin)));
			}
		}
		O->SetArrayField(TEXT("pins"), Pins);
		return O;
	}

	static TSharedRef<FJsonObject> MetadataToJson(UNiagaraScript* Script)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		if (const FVersionedNiagaraScriptData* Data = Script->GetLatestScriptData())
		{
			O->SetStringField(TEXT("description"), Data->Description.ToString());
			O->SetStringField(TEXT("category"), Data->Category.ToString());
			O->SetStringField(TEXT("keywords"), Data->Keywords.ToString());
			O->SetStringField(TEXT("libraryVisibility"), VisibilityName(Data->LibraryVisibility));
			O->SetNumberField(TEXT("moduleUsageBitmask"), Data->ModuleUsageBitmask);
			O->SetArrayField(TEXT("moduleUsages"), UsageMaskNames(Data->ModuleUsageBitmask));
			O->SetBoolField(TEXT("suggested"), Data->bSuggested);
			O->SetBoolField(TEXT("deprecated"), Data->bDeprecated);
			O->SetBoolField(TEXT("experimental"), Data->bExperimental != 0);
		}
		return O;
	}

	static TSharedRef<FJsonObject> CompileToJson(UNiagaraScript* Script)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("status"), CompileStatusName(Script->GetLastCompileStatus()));
		O->SetBoolField(TEXT("synchronized"), Script->AreScriptAndSourceSynchronized(LatestVersion(Script)));
		const FNiagaraVMExecutableData& Exec = Script->GetVMExecutableData();
		O->SetStringField(TEXT("errorMessage"), Exec.ErrorMsg);
		int32 NumErrors = 0;
		int32 NumWarnings = 0;
		TArray<TSharedPtr<FJsonValue>> Events;
		for (const FNiagaraCompileEvent& Event : Exec.LastCompileEvents)
		{
			const TCHAR* Severity = TEXT("Log");
			switch (Event.Severity)
			{
			case FNiagaraCompileEventSeverity::Error: Severity = TEXT("Error"); ++NumErrors; break;
			case FNiagaraCompileEventSeverity::Warning: Severity = TEXT("Warning"); ++NumWarnings; break;
			case FNiagaraCompileEventSeverity::Display: Severity = TEXT("Display"); break;
			default: break;
			}
			TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("severity"), Severity);
			E->SetStringField(TEXT("message"), Event.Message);
			E->SetStringField(TEXT("shortDescription"), Event.ShortDescription);
			E->SetStringField(TEXT("nodeId"), Event.NodeGuid.IsValid() ? IdStr(Event.NodeGuid) : FString());
			E->SetStringField(TEXT("pinId"), Event.PinGuid.IsValid() ? IdStr(Event.PinGuid) : FString());
			Events.Add(MakeShared<FJsonValueObject>(E));
		}
		O->SetNumberField(TEXT("errors"), NumErrors);
		O->SetNumberField(TEXT("warnings"), NumWarnings);
		O->SetArrayField(TEXT("events"), Events);
		return O;
	}

	// ---- parameter map helpers ------------------------------------------------------------------------

	static UNiagaraNodeParameterMapGet* FindDefaultMapGet(UNiagaraGraph* Graph)
	{
		UNiagaraNodeParameterMapGet* First = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UNiagaraNodeParameterMapGet* Get = Cast<UNiagaraNodeParameterMapGet>(Node);
			if (!Get)
			{
				continue;
			}
			if (!First)
			{
				First = Get;
			}
			for (const UEdGraphPin* Pin : Get->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input)
				{
					for (const UEdGraphPin* Linked : Pin->LinkedTo)
					{
						if (Linked && Cast<UNiagaraNodeInput>(Linked->GetOwningNode()))
						{
							return Get; // fed by the input map
						}
					}
				}
			}
		}
		return First;
	}

	template <typename TNode>
	static TNode* FindMapNode(UNiagaraGraph* Graph, const FString& NodeId, FString& OutError)
	{
		if (NodeId.TrimStartAndEnd().IsEmpty())
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (TNode* Typed = Cast<TNode>(Node))
				{
					return Typed;
				}
			}
			OutError = FString::Printf(TEXT("graph has no %s node (add one with AddNode)"), *TNode::StaticClass()->GetName());
			return nullptr;
		}
		UEdGraphNode* Node = FindNode(Graph, NodeId, OutError);
		TNode* Typed = Cast<TNode>(Node);
		if (Node && !Typed)
		{
			OutError = FString::Printf(TEXT("node %s is a %s, not a %s"), *NodeId, *Node->GetClass()->GetName(), *TNode::StaticClass()->GetName());
		}
		return Typed;
	}

	static UEdGraphPin* FindPinByNameDir(UEdGraphNode* Node, FName Name, EEdGraphPinDirection Dir)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Dir && Pin->PinName == Name && !IsAddPin(Pin))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	/** A graph parameter with the same name but another type would make the read/write ambiguous. */
	static bool CheckParameterType(UNiagaraGraph* Graph, const FNiagaraVariable& Var, FString& OutError)
	{
		for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Pair : Graph->GetAllMetaData())
		{
			if (Pair.Key.GetName() == Var.GetName() && Pair.Key.GetType() != Var.GetType())
			{
				OutError = FString::Printf(TEXT("parameter %s already exists in this graph as %s"), *Var.GetName().ToString(), *TypeName(Pair.Key.GetType()));
				return false;
			}
		}
		return true;
	}

	/**
	 * Add a parameter pin the way the Parameters panel / "+" pin menu does: registers the graph parameter if missing and calls
	 * UNiagaraNodeWithDynamicPins::RequestNewTypedPin (through the exported FNiagaraStackGraphUtilities::AddNewVariableToParameterMapNode).
	 * Map Get output pins get their default-value input pin from UNiagaraNodeParameterMapGet::OnNewTypedPinAdded.
	 */
	static UEdGraphPin* AddParameterPin(UNiagaraNodeParameterMapBase* Node, const FNiagaraVariable& Var, EEdGraphPinDirection Dir)
	{
		FNiagaraStackGraphUtilities::AddNewVariableToParameterMapNode(Node, Dir == EGPD_Input, Var);
		return FindPinByNameDir(Node, Var.GetName(), Dir);
	}

	static FName ModuleInputName(const FString& InName)
	{
		const FString Name = InName.TrimStartAndEnd();
		return FName(*(Name.Contains(TEXT(".")) ? Name : TEXT("Module.") + Name));
	}

	static bool ApplyInputDefault(UNiagaraGraph* Graph, const FNiagaraVariable& InputVar, const FString& DefaultValue, FString& OutStored, FString& OutError)
	{
		UNiagaraScriptVariable* ScriptVar = Graph->GetScriptVariable(InputVar.GetName());
		if (!ScriptVar)
		{
			OutError = TEXT("graph parameter not found: ") + InputVar.GetName().ToString();
			return false;
		}
		FNiagaraVariable Parsed;
		if (!ParseValue(ScriptVar->Variable.GetType(), DefaultValue, Parsed, OutError))
		{
			return false;
		}
		// Same data path as the Parameters panel details (UNiagaraScriptVariable customization): mode Value + default bytes,
		// then PostEditChange -> UNiagaraGraph::ScriptVariableChanged pushes the value into the Map Get default pins.
		ScriptVar->Modify();
		ScriptVar->DefaultMode = ENiagaraDefaultMode::Value;
		ScriptVar->SetDefaultValueData(Parsed.GetData());
		ScriptVar->PostEditChange();
		OutStored = ValueToString(Parsed);
		return true;
	}

	static TArray<TSharedPtr<FJsonValue>> InputsToJson(UNiagaraGraph* Graph)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Pair : Graph->GetAllMetaData())
		{
			const FString Name = Pair.Key.GetName().ToString();
			if (!Name.StartsWith(TEXT("Module.")) || !Pair.Value)
			{
				continue;
			}
			const UNiagaraScriptVariable* ScriptVar = Pair.Value;
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("name"), Name);
			O->SetStringField(TEXT("type"), TypeName(Pair.Key.GetType()));
			O->SetStringField(TEXT("defaultMode"), DefaultModeName(ScriptVar->DefaultMode));
			FString Default;
			if (const uint8* Data = ScriptVar->GetDefaultValueData())
			{
				FNiagaraVariable Tmp(Pair.Key.GetType(), Pair.Key.GetName());
				Tmp.SetData(Data);
				Default = ValueToString(Tmp);
			}
			O->SetStringField(TEXT("default"), Default);
			O->SetStringField(TEXT("description"), ScriptVar->Metadata.Description.ToString());
			TArray<TSharedPtr<FJsonValue>> PinIds;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Cast<UNiagaraNodeParameterMapGet>(Node))
				{
					if (UEdGraphPin* Pin = FindPinByNameDir(Node, Pair.Key.GetName(), EGPD_Output))
					{
						PinIds.Add(MakeShared<FJsonValueString>(IdStr(Pin->PinId)));
					}
				}
			}
			O->SetArrayField(TEXT("pinIds"), PinIds);
			Out.Add(MakeShared<FJsonValueObject>(O));
		}
		return Out;
	}

	// ---- node creation --------------------------------------------------------------------------------

	/** Mirrors FNiagaraAction_NewNode::CreateNode (NiagaraActions.cpp) for a node already outered to the graph. */
	static bool PlaceNewNode(UNiagaraGraph* Graph, UEdGraphNode* Node, int32 PosX, int32 PosY, FString& OutError)
	{
		if (UNiagaraNode* NiagaraNode = Cast<UNiagaraNode>(Node))
		{
			FString Msg;
			if (!NiagaraNode->CanAddToGraph(Graph, Msg))
			{
				OutError = Msg.IsEmpty() ? FString(TEXT("node cannot be added to this graph")) : Msg;
				Node->MarkAsGarbage();
				return false;
			}
		}
		Graph->Modify();
		Node->SetFlags(RF_Transactional);
		Graph->AddNode(Node, /*bUserAction*/ true, /*bSelectNewNode*/ false);
		Node->CreateNewGuid();
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		Node->NodePosX = PosX;
		Node->NodePosY = PosY;
		return true;
	}

	static bool IsValidHlslIdentifier(const FString& Name)
	{
		if (Name.IsEmpty() || !(FChar::IsAlpha(Name[0]) || Name[0] == TCHAR('_')))
		{
			return false;
		}
		for (const TCHAR C : Name)
		{
			if (!(FChar::IsAlnum(C) || C == TCHAR('_')))
			{
				return false;
			}
		}
		return true;
	}

	/** Every loaded function call node that calls Script (FNiagaraEditorUtilities::GetReferencingFunctionCallNodes is not exported). */
	static TArray<UNiagaraNodeFunctionCall*> GetReferencingFunctionCalls(UNiagaraScript* Script)
	{
		TArray<UNiagaraNodeFunctionCall*> Out;
		for (TObjectIterator<UNiagaraNodeFunctionCall> It; It; ++It)
		{
			UNiagaraNodeFunctionCall* Call = *It;
			if (!IsValid(Call) || Call->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || Call->GetOutermost() == GetTransientPackage())
			{
				continue;
			}
			if (Call->FunctionScript == Script)
			{
				Out.Add(Call);
			}
		}
		return Out;
	}

	// ---- scratch pad owner ----------------------------------------------------------------------------

	struct FScratchOwner
	{
		UNiagaraSystem* System = nullptr;
		UNiagaraEmitter* Emitter = nullptr;
		UObject* Outer = nullptr;                              // UNiagaraSystem or UNiagaraScratchPadContainer
		TArray<TObjectPtr<UNiagaraScript>>* Scripts = nullptr;
	};

	static bool ResolveScratchOwner(const FString& OwnerPath, FScratchOwner& Out, FString& OutError)
	{
		UObject* Owner = LoadAsset<UObject>(OwnerPath);
		if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Owner))
		{
			// System asset edit mode (UNiagaraScratchPadViewModel GetOuterAndTargetScripts): outer = system, list = ScratchPadScripts.
			Out.System = System;
			Out.Outer = System;
			Out.Scripts = &System->ScratchPadScripts;
			return true;
		}
		if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Owner))
		{
			// Emitter asset edit mode: outer = latest emitter data's scratch pad container.
			FVersionedNiagaraEmitterData* Data = Emitter->GetLatestEmitterData();
			if (!Data || !Data->ScratchPads)
			{
				OutError = TEXT("emitter has no scratch pad container: ") + OwnerPath;
				return false;
			}
			Out.Emitter = Emitter;
			Out.Outer = Data->ScratchPads;
			Out.Scripts = &Data->ScratchPads->Scripts;
			return true;
		}
		OutError = TEXT("not a Niagara system or emitter asset: ") + OwnerPath;
		return false;
	}
}

// =====================================================================================================

FString UMCPInspectorNiagaraScriptToolset::CreateNiagaraScript(const FString& AssetPath, const FString& Usage)
{
	using namespace MCPInspectorNiagaraScriptToolset;
	const FString Path = AssetPath.TrimStartAndEnd();
	if (!FPackageName::IsValidLongPackageName(Path)) return Err(TEXT("invalid package path: ") + AssetPath);
	if (FPackageName::DoesPackageExist(Path) || FindPackage(nullptr, *Path)) return Err(TEXT("asset already exists: ") + Path);

	ENiagaraScriptUsage ParsedUsage;
	if (!ParseUsageName(Usage, ParsedUsage)) return Err(TEXT("unknown usage (Module, DynamicInput, Function): ") + Usage);

	// Same factories the content browser uses (NiagaraScriptFactoryNew.h); FactoryCreateNew picks the default script from
	// UNiagaraEditorSettings or builds the procedural default graph (UNiagaraScriptFactoryNew::InitializeScript).
	UFactory* Factory = nullptr;
	switch (ParsedUsage)
	{
	case ENiagaraScriptUsage::Module: Factory = NewObject<UNiagaraModuleScriptFactory>(); break;
	case ENiagaraScriptUsage::Function: Factory = NewObject<UNiagaraFunctionScriptFactory>(); break;
	case ENiagaraScriptUsage::DynamicInput: Factory = NewObject<UNiagaraDynamicInputScriptFactory>(); break;
	default: return Err(TEXT("only Module, DynamicInput and Function scripts can be created: ") + Usage);
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UNiagaraScript* Script = Cast<UNiagaraScript>(AssetTools.CreateAsset(
		FPackageName::GetShortName(Path), FPackageName::GetLongPackagePath(Path), UNiagaraScript::StaticClass(), Factory));
	if (!Script) return Err(TEXT("asset creation failed: ") + Path);

	UNiagaraGraph* Graph = GetGraph(Script);
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("path"), Script->GetPathName());
	O->SetStringField(TEXT("usage"), UsageName(Script->GetUsage()));
	O->SetNumberField(TEXT("nodes"), Graph ? Graph->Nodes.Num() : 0);
	O->SetStringField(TEXT("compileStatus"), CompileStatusName(Script->GetLastCompileStatus()));
	return ToJson(O);
}

FString UMCPInspectorNiagaraScriptToolset::SetScriptMetadata(const FString& ScriptPath, const FString& Description, const FString& Category, const FString& LibraryVisibility, const FString& ModuleUsageBitmask, const FString& Keywords)
{
	using namespace MCPInspectorNiagaraScriptToolset;
	FScriptCtx Ctx;
	FString Error;
	if (!LoadCtx(ScriptPath, Ctx, Error)) return Err(Error);
	FVersionedNiagaraScriptData* Data = Ctx.Script->GetLatestScriptData();
	if (!Data) return Err(TEXT("script has no version data"));

	// Validate everything before touching the asset.
	TOptional<ENiagaraScriptLibraryVisibility> NewVisibility;
	if (!LibraryVisibility.TrimStartAndEnd().IsEmpty())
	{
		const FString V = LibraryVisibility.TrimStartAndEnd();
		if (V.Equals(TEXT("Exposed"), ESearchCase::IgnoreCase) || V.Equals(TEXT("Library"), ESearchCase::IgnoreCase)) NewVisibility = ENiagaraScriptLibraryVisibility::Library;
		else if (V.Equals(TEXT("Unexposed"), ESearchCase::IgnoreCase)) NewVisibility = ENiagaraScriptLibraryVisibility::Unexposed;
		else if (V.Equals(TEXT("Hidden"), ESearchCase::IgnoreCase)) NewVisibility = ENiagaraScriptLibraryVisibility::Hidden;
		else return Err(TEXT("unknown library visibility (Exposed, Unexposed, Hidden): ") + LibraryVisibility);
	}
	TOptional<int32> NewMask;
	if (!ModuleUsageBitmask.TrimStartAndEnd().IsEmpty())
	{
		int32 Mask = 0;
		if (!ParseUsageMask(ModuleUsageBitmask, Mask, Error)) return Err(Error);
		NewMask = Mask;
	}

	const FScopedTransaction Transaction(LOCTEXT("SetScriptMetadata", "BPI: Set Niagara Script Metadata"));
	Ctx.Script->Modify();
	const FGuid Version = Data->Version.VersionGuid;
	UScriptStruct* DataStruct = FVersionedNiagaraScriptData::StaticStruct();

	// Same notification the versioned details panel sends (UNiagaraScript::PostEditChangeVersionedProperty marks the source
	// not synchronized and refreshes the asset registry tag cache).
	auto Notify = [&](FName Member)
	{
		FProperty* Prop = FindFProperty<FProperty>(DataStruct, Member);
		if (Prop)
		{
			FPropertyChangedEvent Event(Prop, EPropertyChangeType::ValueSet);
			Ctx.Script->PostEditChangeVersionedProperty(Event, Version);
		}
	};
	auto ApplyText = [&](const FString& In, FText& Field, FName Member)
	{
		if (In.IsEmpty())
		{
			return;
		}
		Field = In.Equals(TEXT("<none>")) ? FText::GetEmpty() : FText::FromString(In);
		Notify(Member);
	};
	ApplyText(Description, Data->Description, GET_MEMBER_NAME_CHECKED(FVersionedNiagaraScriptData, Description));
	ApplyText(Category, Data->Category, GET_MEMBER_NAME_CHECKED(FVersionedNiagaraScriptData, Category));
	ApplyText(Keywords, Data->Keywords, GET_MEMBER_NAME_CHECKED(FVersionedNiagaraScriptData, Keywords));
	if (NewVisibility.IsSet())
	{
		Data->LibraryVisibility = NewVisibility.GetValue();
		Notify(GET_MEMBER_NAME_CHECKED(FVersionedNiagaraScriptData, LibraryVisibility));
	}
	if (NewMask.IsSet())
	{
		Data->ModuleUsageBitmask = NewMask.GetValue();
		Notify(GET_MEMBER_NAME_CHECKED(FVersionedNiagaraScriptData, ModuleUsageBitmask));
		// The bitmask is part of the compile options (FNiagaraCompileOptions in UNiagaraScript::RequestCompile).
		Ctx.Script->MarkScriptAndSourceDesynchronized(TEXT("BPI: module usage bitmask changed"), Version);
	}
	Ctx.Script->MarkPackageDirty();
	return ToJson(MetadataToJson(Ctx.Script));
}

FString UMCPInspectorNiagaraScriptToolset::CompileNiagaraScript(const FString& ScriptPath, bool bForce, bool bRefreshReferencers)
{
	using namespace MCPInspectorNiagaraScriptToolset;
	FScriptCtx Ctx;
	FString Error;
	if (!LoadCtx(ScriptPath, Ctx, Error)) return Err(Error);
	if (!Ctx.Script->IsCompilable()) return Err(TEXT("script usage is not compilable on its own"));

	// FNiagaraScriptViewModel::CompileStandaloneScript -> UNiagaraScript::RequestCompile, which blocks on the compile job
	// (INiagaraModule::GetCompileJobResult(JobHandle, bWait=true)) or takes the DDC result.
	Ctx.Script->RequestCompile(LatestVersion(Ctx.Script), bForce);

	TSharedRef<FJsonObject> O = CompileToJson(Ctx.Script);
	TArray<TSharedPtr<FJsonValue>> Refreshed;
	if (bRefreshReferencers)
	{
		// Same follow-up as FNiagaraScratchPadScriptViewModel::ApplyChanges.
		for (UNiagaraNodeFunctionCall* Call : GetReferencingFunctionCalls(Ctx.Script))
		{
			Call->RefreshFromExternalChanges();
			Call->MarkNodeRequiresSynchronization(TEXT("BPI: called script changed"), true);
			Call->MarkPackageDirty();
			Refreshed.Add(MakeShared<FJsonValueString>(Call->GetPathName()));
		}
	}
	O->SetArrayField(TEXT("refreshedReferencers"), Refreshed);
	return ToJson(O);
}

FString UMCPInspectorNiagaraScriptToolset::DumpScriptGraph(const FString& ScriptPath)
{
	using namespace MCPInspectorNiagaraScriptToolset;
	FScriptCtx Ctx;
	FString Error;
	if (!LoadCtx(ScriptPath, Ctx, Error)) return Err(Error);

	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("path"), Ctx.Script->GetPathName());
	O->SetStringField(TEXT("usage"), UsageName(Ctx.Script->GetUsage()));
	O->SetBoolField(TEXT("isScratchPad"), IsScratchPad(Ctx.Script));
	O->SetStringField(TEXT("owner"), IsScratchPad(Ctx.Script) ? Ctx.Script->GetOutermostObject()->GetPathName() : FString());
	O->SetStringField(TEXT("versionGuid"), IdStr(LatestVersion(Ctx.Script)));
	O->SetObjectField(TEXT("metadata"), MetadataToJson(Ctx.Script));
	O->SetObjectField(TEXT("compile"), CompileToJson(Ctx.Script));
	O->SetArrayField(TEXT("inputs"), InputsToJson(Ctx.Graph));

	TArray<TSharedPtr<FJsonValue>> Writes;
	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (UEdGraphNode* Node : Ctx.Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}
		Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Node)));
		if (Cast<UNiagaraNodeParameterMapSet>(Node))
		{
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Input || IsAddPin(Pin))
				{
					continue;
				}
				const FNiagaraTypeDefinition Type = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
				if (Type == FNiagaraTypeDefinition::GetParameterMapDef())
				{
					continue;
				}
				TSharedRef<FJsonObject> W = MakeShared<FJsonObject>();
				W->SetStringField(TEXT("name"), Pin->PinName.ToString());
				W->SetStringField(TEXT("type"), TypeName(Type));
				W->SetStringField(TEXT("nodeId"), IdStr(Node->NodeGuid));
				W->SetStringField(TEXT("pinId"), IdStr(Pin->PinId));
				Writes.Add(MakeShared<FJsonValueObject>(W));
			}
		}
	}
	O->SetArrayField(TEXT("writes"), Writes);
	O->SetArrayField(TEXT("nodes"), Nodes);
	return ToJson(O);
}

FString UMCPInspectorNiagaraScriptToolset::AddScriptInput(const FString& ScriptPath, const FString& Name, const FString& Type, const FString& DefaultValue, const FString& MapGetNodeId)
{
	using namespace MCPInspectorNiagaraScriptToolset;
	FScriptCtx Ctx;
	FString Error;
	if (!LoadCtx(ScriptPath, Ctx, Error)) return Err(Error);

	const FName InputName = ModuleInputName(Name);
	if (Name.TrimStartAndEnd().IsEmpty() || !InputName.ToString().StartsWith(TEXT("Module.")))
	{
		return Err(TEXT("module inputs live in the Module namespace (use AddMapGetPin for other namespaces): ") + Name);
	}
	FNiagaraTypeDefinition ParsedType;
	if (!ParseType(Type, ParsedType, Error)) return Err(Error);
	if (ParsedType == FNiagaraTypeDefinition::GetParameterMapDef()) return Err(TEXT("a module input cannot be a parameter map"));

	UNiagaraNodeParameterMapGet* MapGet = MapGetNodeId.TrimStartAndEnd().IsEmpty()
		? FindDefaultMapGet(Ctx.Graph)
		: FindMapNode<UNiagaraNodeParameterMapGet>(Ctx.Graph, MapGetNodeId, Error);
	if (!MapGet) return Err(Error.IsEmpty() ? FString(TEXT("graph has no Map Get node (add one with AddNode ParameterMapGet)")) : Error);

	const FNiagaraVariable InputVar(ParsedType, InputName);
	if (!CheckParameterType(Ctx.Graph, InputVar, Error)) return Err(Error);
	if (FindPinByNameDir(MapGet, InputName, EGPD_Output)) return Err(TEXT("input already read on this Map Get node: ") + InputName.ToString());
	if (!DefaultValue.TrimStartAndEnd().IsEmpty())
	{
		FNiagaraVariable Probe;
		if (!ParseValue(ParsedType, DefaultValue, Probe, Error)) return Err(Error);
	}

	const FScopedTransaction Transaction(LOCTEXT("AddScriptInput", "BPI: Add Niagara Module Input"));
	Ctx.Graph->Modify();
	UEdGraphPin* Pin = AddParameterPin(MapGet, InputVar, EGPD_Output);
	if (!Pin) return Err(TEXT("failed to add the Map Get pin for ") + InputName.ToString());

	FString StoredDefault;
	if (!DefaultValue.TrimStartAndEnd().IsEmpty() && !ApplyInputDefault(Ctx.Graph, InputVar, DefaultValue, StoredDefault, Error))
	{
		return Err(Error);
	}
	Touch(Ctx, MapGet);

	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("name"), Pin->PinName.ToString());
	O->SetStringField(TEXT("type"), TypeName(ParsedType));
	O->SetStringField(TEXT("default"), StoredDefault);
	O->SetStringField(TEXT("nodeId"), IdStr(MapGet->NodeGuid));
	O->SetStringField(TEXT("pinId"), IdStr(Pin->PinId));
	return ToJson(O);
}

FString UMCPInspectorNiagaraScriptToolset::RemoveScriptInput(const FString& ScriptPath, const FString& Name)
{
	using namespace MCPInspectorNiagaraScriptToolset;
	FScriptCtx Ctx;
	FString Error;
	if (!LoadCtx(ScriptPath, Ctx, Error)) return Err(Error);
	const FName InputName = ModuleInputName(Name);

	TArray<TPair<UEdGraphNode*, UEdGraphPin*>> Reads;
	for (UEdGraphNode* Node : Ctx.Graph->Nodes)
	{
		if (Cast<UNiagaraNodeParameterMapGet>(Node))
		{
			if (UEdGraphPin* Pin = FindPinByNameDir(Node, InputName, EGPD_Output))
			{
				Reads.Emplace(Node, Pin);
			}
		}
	}
	TArray<FNiagaraVariable> Params;
	for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Pair : Ctx.Graph->GetAllMetaData())
	{
		if (Pair.Key.GetName() == InputName)
		{
			Params.Add(Pair.Key);
		}
	}
	if (Reads.Num() == 0 && Params.Num() == 0) return Err(TEXT("module input not found: ") + InputName.ToString());

	const FScopedTransaction Transaction(LOCTEXT("RemoveScriptInput", "BPI: Remove Niagara Module Input"));
	Ctx.Graph->Modify();
	const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
	for (const TPair<UEdGraphNode*, UEdGraphPin*>& Read : Reads)
	{
		// UEdGraphNode::RemovePin -> UNiagaraNodeParameterMapGet::OnPinRemoved also removes the paired default-value pin.
		Read.Key->Modify();
		Schema->BreakPinLinks(*Read.Value, true);
		Read.Key->RemovePin(Read.Value);
		if (UNiagaraNode* NiagaraNode = Cast<UNiagaraNode>(Read.Key))
		{
			NiagaraNode->MarkNodeRequiresSynchronization(TEXT("BPI: module input removed"), true);
		}
	}
	// UNiagaraGraph::RemoveParameter is not exported; remove the script variable entry from the exported metadata map
	// (the parameter reference cache is rebuilt by NotifyGraphChanged -> InvalidateCachedParameterData).
	for (const FNiagaraVariable& Param : Params)
	{
		Ctx.Graph->GetAllMetaData().Remove(Param);
	}
	Touch(Ctx);

	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("removedPins"), Reads.Num());
	O->SetBoolField(TEXT("removedParameter"), Params.Num() > 0);
	return ToJson(O);
}

FString UMCPInspectorNiagaraScriptToolset::SetScriptInputDefault(const FString& ScriptPath, const FString& Name, const FString& DefaultValue)
{
	using namespace MCPInspectorNiagaraScriptToolset;
	FScriptCtx Ctx;
	FString Error;
	if (!LoadCtx(ScriptPath, Ctx, Error)) return Err(Error);
	const FName InputName = ModuleInputName(Name);
	UNiagaraScriptVariable* ScriptVar = Ctx.Graph->GetScriptVariable(InputName);
	if (!ScriptVar) return Err(TEXT("module input not found: ") + InputName.ToString());
	FNiagaraVariable Probe;
	if (!ParseValue(ScriptVar->Variable.GetType(), DefaultValue, Probe, Error)) return Err(Error);

	const FScopedTransaction Transaction(LOCTEXT("SetScriptInputDefault", "BPI: Set Niagara Module Input Default"));
	Ctx.Graph->Modify();
	FString Stored;
	if (!ApplyInputDefault(Ctx.Graph, ScriptVar->Variable, DefaultValue, Stored, Error)) return Err(Error);
	Touch(Ctx);

	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("name"), InputName.ToString());
	O->SetStringField(TEXT("type"), TypeName(ScriptVar->Variable.GetType()));
	O->SetStringField(TEXT("default"), Stored);
	return ToJson(O);
}

static FString BPINiagara_AddMapPin(const FString& ScriptPath, const FString& NodeId, const FString& ParameterName, const FString& Type, bool bWrite)
{
	using namespace MCPInspectorNiagaraScriptToolset;
	FScriptCtx Ctx;
	FString Error;
	if (!LoadCtx(ScriptPath, Ctx, Error)) return Err(Error);

	const FString Name = ParameterName.TrimStartAndEnd();
	if (!Name.Contains(TEXT(".")) || Name.StartsWith(TEXT(".")) || Name.EndsWith(TEXT(".")))
	{
		return Err(TEXT("parameter name needs a namespace, e.g. Particles.Color, Module.Tint, Engine.DeltaTime: ") + ParameterName);
	}
	FNiagaraTypeDefinition ParsedType;
	if (!ParseType(Type, ParsedType, Error)) return Err(Error);
	if (ParsedType == FNiagaraTypeDefinition::GetParameterMapDef()) return Err(TEXT("a parameter cannot be a parameter map"));

	UNiagaraNodeParameterMapBase* MapNode = bWrite
		? static_cast<UNiagaraNodeParameterMapBase*>(FindMapNode<UNiagaraNodeParameterMapSet>(Ctx.Graph, NodeId, Error))
		: static_cast<UNiagaraNodeParameterMapBase*>(FindMapNode<UNiagaraNodeParameterMapGet>(Ctx.Graph, NodeId, Error));
	if (!MapNode) return Err(Error);

	const FNiagaraVariable Var(ParsedType, FName(*Name));
	if (!CheckParameterType(Ctx.Graph, Var, Error)) return Err(Error);
	const EEdGraphPinDirection Dir = bWrite ? EGPD_Input : EGPD_Output;
	if (FindPinByNameDir(MapNode, Var.GetName(), Dir)) return Err(FString::Printf(TEXT("%s already has a pin %s"), *IdStr(MapNode->NodeGuid), *Name));

	const FScopedTransaction Transaction(bWrite ? LOCTEXT("AddMapSetPin", "BPI: Add Niagara Map Set Pin") : LOCTEXT("AddMapGetPin", "BPI: Add Niagara Map Get Pin"));
	Ctx.Graph->Modify();
	UEdGraphPin* Pin = AddParameterPin(MapNode, Var, Dir);
	if (!Pin) return Err(TEXT("failed to add pin ") + Name);
	Touch(Ctx, MapNode);

	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("nodeId"), IdStr(MapNode->NodeGuid));
	O->SetStringField(TEXT("pinId"), IdStr(Pin->PinId));
	O->SetStringField(TEXT("name"), Pin->PinName.ToString());
	O->SetStringField(TEXT("type"), TypeName(ParsedType));
	return ToJson(O);
}

FString UMCPInspectorNiagaraScriptToolset::AddMapGetPin(const FString& ScriptPath, const FString& NodeId, const FString& ParameterName, const FString& Type)
{
	return BPINiagara_AddMapPin(ScriptPath, NodeId, ParameterName, Type, false);
}

FString UMCPInspectorNiagaraScriptToolset::AddMapSetPin(const FString& ScriptPath, const FString& NodeId, const FString& ParameterName, const FString& Type)
{
	return BPINiagara_AddMapPin(ScriptPath, NodeId, ParameterName, Type, true);
}

FString UMCPInspectorNiagaraScriptToolset::AddNode(const FString& ScriptPath, const FString& NodeKind, const FString& Arg, int32 PosX, int32 PosY)
{
	using namespace MCPInspectorNiagaraScriptToolset;
	FScriptCtx Ctx;
	FString Error;
	if (!LoadCtx(ScriptPath, Ctx, Error)) return Err(Error);
	const FString Kind = NodeKind.TrimStartAndEnd();

	// Validate arguments before opening the transaction.
	UNiagaraScript* FunctionScript = nullptr;
	UClass* RerouteClass = nullptr;
	if (Kind.Equals(TEXT("Op"), ESearchCase::IgnoreCase))
	{
		if (Arg.TrimStartAndEnd().IsEmpty()) return Err(TEXT("Op needs an op name, e.g. Numeric::Add"));
	}
	else if (Kind.Equals(TEXT("FunctionCall"), ESearchCase::IgnoreCase))
	{
		FunctionScript = LoadAsset<UNiagaraScript>(Arg);
		if (!FunctionScript) return Err(TEXT("function script not found: ") + Arg);
		const ENiagaraScriptUsage CalledUsage = FunctionScript->GetUsage();
		if (CalledUsage != ENiagaraScriptUsage::Function && CalledUsage != ENiagaraScriptUsage::DynamicInput)
		{
			return Err(FString::Printf(TEXT("only Function and DynamicInput scripts can be called inside a script graph (got %s); add modules to a stack with the official AddModule"), *UsageName(CalledUsage)));
		}
		if (FunctionScript == Ctx.Script) return Err(TEXT("a script cannot call itself"));
	}
	else if (Kind.Equals(TEXT("Reroute"), ESearchCase::IgnoreCase))
	{
		// UNiagaraNodeReroute lives in NiagaraEditor/Private; resolve the class by path instead of including the header.
		RerouteClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeReroute"));
		if (!RerouteClass) return Err(TEXT("NiagaraNodeReroute class not found"));
	}
	else if (!Kind.Equals(TEXT("CustomHlsl"), ESearchCase::IgnoreCase)
		&& !Kind.Equals(TEXT("ParameterMapGet"), ESearchCase::IgnoreCase)
		&& !Kind.Equals(TEXT("ParameterMapSet"), ESearchCase::IgnoreCase))
	{
		return Err(TEXT("unknown node kind (Op, FunctionCall, CustomHlsl, ParameterMapGet, ParameterMapSet, Reroute): ") + NodeKind);
	}

	FScopedTransaction Transaction(LOCTEXT("AddNode", "BPI: Add Niagara Node"));
	UEdGraphNode* NewNode = nullptr;
	if (Kind.Equals(TEXT("Op"), ESearchCase::IgnoreCase))
	{
		// Same as UE::Niagara::Wizard::Utilities::CreateOpNode / the schema's op menu template.
		UNiagaraNodeOp* Op = NewObject<UNiagaraNodeOp>(Ctx.Graph, NAME_None, RF_Transactional);
		Op->OpName = FName(*Arg.TrimStartAndEnd());
		NewNode = Op;
	}
	else if (FunctionScript)
	{
		// Same as UE::Niagara::Wizard::Utilities::CreateFunctionCallNode.
		UNiagaraNodeFunctionCall* Call = NewObject<UNiagaraNodeFunctionCall>(Ctx.Graph, NAME_None, RF_Transactional);
		Call->FunctionScript = FunctionScript;
		Call->SelectedScriptVersion = FunctionScript->GetExposedVersion().VersionGuid;
		NewNode = Call;
	}
	else if (Kind.Equals(TEXT("CustomHlsl"), ESearchCase::IgnoreCase))
	{
		UNiagaraNodeCustomHlsl* Hlsl = NewObject<UNiagaraNodeCustomHlsl>(Ctx.Graph, NAME_None, RF_Transactional);
		// The schema template calls SetCustomHlsl (not exported) before placing the node; set the private UPROPERTY directly.
		if (FStrProperty* Prop = CastField<FStrProperty>(UNiagaraNodeCustomHlsl::StaticClass()->FindPropertyByName(TEXT("CustomHlsl"))))
		{
			const FString Code = Arg.TrimStartAndEnd().IsEmpty()
				? FString(TEXT("// Insert the body of the function here and add any inputs\r\n// and outputs by name using the add pins above.\r\n// Currently, complicated branches, for loops, switches, etc are not advised."))
				: Arg;
			Prop->SetPropertyValue_InContainer(Hlsl, Code);
		}
		NewNode = Hlsl;
	}
	else if (Kind.Equals(TEXT("ParameterMapGet"), ESearchCase::IgnoreCase))
	{
		NewNode = NewObject<UNiagaraNodeParameterMapGet>(Ctx.Graph, NAME_None, RF_Transactional);
	}
	else if (Kind.Equals(TEXT("ParameterMapSet"), ESearchCase::IgnoreCase))
	{
		NewNode = NewObject<UNiagaraNodeParameterMapSet>(Ctx.Graph, NAME_None, RF_Transactional);
	}
	else
	{
		NewNode = NewObject<UEdGraphNode>(Ctx.Graph, RerouteClass, NAME_None, RF_Transactional);
	}

	if (!PlaceNewNode(Ctx.Graph, NewNode, PosX, PosY, Error))
	{
		Transaction.Cancel();
		return Err(Error);
	}
	// UNiagaraNodeOp::AllocateDefaultPins creates nothing for an unknown op (FNiagaraOpInfo::GetOpInfo is not exported).
	if (Cast<UNiagaraNodeOp>(NewNode) && NewNode->Pins.Num() == 0)
	{
		NewNode->DestroyNode();
		Transaction.Cancel();
		return Err(TEXT("unknown op name (format Category::Name, e.g. Numeric::Add, Numeric::Mul, Numeric::Normalize, Vector3::Cross, Boolean::LogicAnd): ") + Arg);
	}
	Touch(Ctx, NewNode);
	return ToJson(NodeToJson(NewNode));
}

FString UMCPInspectorNiagaraScriptToolset::RemoveNode(const FString& ScriptPath, const FString& NodeId)
{
	using namespace MCPInspectorNiagaraScriptToolset;
	FScriptCtx Ctx;
	FString Error;
	if (!LoadCtx(ScriptPath, Ctx, Error)) return Err(Error);
	UEdGraphNode* Node = FindNode(Ctx.Graph, NodeId, Error);
	if (!Node) return Err(Error);
	if (!Node->CanUserDeleteNode()) return Err(FString::Printf(TEXT("%s cannot be deleted"), *Node->GetClass()->GetName()));

	// Same as FNiagaraScriptGraphViewModel::DeleteSelectedNodes.
	const FScopedTransaction Transaction(LOCTEXT("RemoveNode", "BPI: Remove Niagara Node"));
	Ctx.Graph->Modify();
	Node->Modify();
	Node->DestroyNode();
	Touch(Ctx);
	return TEXT("OK");
}

FString UMCPInspectorNiagaraScriptToolset::SetCustomHlsl(const FString& ScriptPath, const FString& NodeId, const FString& Code)
{
	using namespace MCPInspectorNiagaraScriptToolset;
	FScriptCtx Ctx;
	FString Error;
	if (!LoadCtx(ScriptPath, Ctx, Error)) return Err(Error);
	UNiagaraNodeCustomHlsl* Node = Cast<UNiagaraNodeCustomHlsl>(FindNode(Ctx.Graph, NodeId, Error));
	if (!Node) return Err(Error.IsEmpty() ? TEXT("not a Custom HLSL node: ") + NodeId : Error);
	FStrProperty* Prop = CastField<FStrProperty>(UNiagaraNodeCustomHlsl::StaticClass()->FindPropertyByName(TEXT("CustomHlsl")));
	if (!Prop) return Err(TEXT("UNiagaraNodeCustomHlsl::CustomHlsl property not found"));

	// Details-panel path: PostEditChangeProperty(CustomHlsl) -> RefreshFromExternalChanges + NotifyGraphNeedsRecompile.
	const FScopedTransaction Transaction(LOCTEXT("SetCustomHlsl", "BPI: Set Custom HLSL"));
	Node->Modify();
	Node->PreEditChange(Prop);
	Prop->SetPropertyValue_InContainer(Node, Code);
	FPropertyChangedEvent Event(Prop, EPropertyChangeType::ValueSet);
	Node->PostEditChangeProperty(Event);
	Touch(Ctx, Node);
	return ToJson(NodeToJson(Node));
}

FString UMCPInspectorNiagaraScriptToolset::AddCustomHlslPin(const FString& ScriptPath, const FString& NodeId, const FString& Name, const FString& Type, bool bIsOutput)
{
	using namespace MCPInspectorNiagaraScriptToolset;
	FScriptCtx Ctx;
	FString Error;
	if (!LoadCtx(ScriptPath, Ctx, Error)) return Err(Error);
	UNiagaraNodeCustomHlsl* Node = Cast<UNiagaraNodeCustomHlsl>(FindNode(Ctx.Graph, NodeId, Error));
	if (!Node) return Err(Error.IsEmpty() ? TEXT("not a Custom HLSL node: ") + NodeId : Error);

	const FString PinName = Name.TrimStartAndEnd();
	if (!IsValidHlslIdentifier(PinName)) return Err(TEXT("pin name must be an HLSL identifier ([A-Za-z_][A-Za-z0-9_]*): ") + Name);
	FNiagaraTypeDefinition ParsedType;
	if (!ParseType(Type, ParsedType, Error)) return Err(Error);
	const EEdGraphPinDirection Dir = bIsOutput ? EGPD_Output : EGPD_Input;
	if (!Node->AllowNiagaraTypeForAddPin(ParsedType, Dir)) return Err(FString::Printf(TEXT("type %s is not allowed on a Custom HLSL %s pin"), *TypeName(ParsedType), bIsOutput ? TEXT("output") : TEXT("input")));
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && !IsAddPin(Pin) && Pin->PinName == FName(*PinName))
		{
			return Err(TEXT("pin name already used on this node: ") + PinName);
		}
	}

	// UNiagaraNodeWithDynamicPins::RequestNewTypedPin / UNiagaraNodeCustomHlsl::RebuildSignatureFromPins are not exported.
	// Equivalent result: extend the node's FunctionSignature (public UPROPERTY) and rebuild the pins with the exported
	// UNiagaraNode::ReallocatePins (UNiagaraNodeFunctionCall::AllocateDefaultPins builds pins from Signature; existing pins
	// keep their ids, defaults and links by name).
	const FScopedTransaction Transaction(LOCTEXT("AddCustomHlslPin", "BPI: Add Custom HLSL Pin"));
	Node->Modify();
	FNiagaraVariable Var(ParsedType, FName(*PinName));
	if (bIsOutput)
	{
		Node->Signature.Outputs.Add(Var);
	}
	else
	{
		if (ParsedType != FNiagaraTypeDefinition::GetParameterMapDef() && !ParsedType.IsDataInterface() && !ParsedType.IsUObject())
		{
			FNiagaraEditorUtilities::ResetVariableToDefaultValue(Var);
		}
		Node->Signature.Inputs.Add(Var);
	}
	MCPInspectorNiagaraScriptToolset::CallReallocatePins(Node);
	Touch(Ctx, Node);
	return ToJson(NodeToJson(Node));
}

FString UMCPInspectorNiagaraScriptToolset::RemoveCustomHlslPin(const FString& ScriptPath, const FString& NodeId, const FString& Pin)
{
	using namespace MCPInspectorNiagaraScriptToolset;
	FScriptCtx Ctx;
	FString Error;
	if (!LoadCtx(ScriptPath, Ctx, Error)) return Err(Error);
	UNiagaraNodeCustomHlsl* Node = Cast<UNiagaraNodeCustomHlsl>(FindNode(Ctx.Graph, NodeId, Error));
	if (!Node) return Err(Error.IsEmpty() ? TEXT("not a Custom HLSL node: ") + NodeId : Error);
	UEdGraphPin* Target = FindPin(Node, Pin, EGPD_Input, Error);
	if (!Target) return Err(Error);

	const FName TargetName = Target->PinName;
	const bool bOutput = Target->Direction == EGPD_Output;
	const int32 Index = bOutput
		? Node->Signature.Outputs.IndexOfByPredicate([TargetName](const FNiagaraVariableBase& V) { return V.GetName() == TargetName; })
		: Node->Signature.Inputs.IndexOfByPredicate([TargetName](const FNiagaraVariable& V) { return V.GetName() == TargetName; });
	if (Index == INDEX_NONE) return Err(TEXT("pin is not part of the Custom HLSL signature: ") + TargetName.ToString());

	const FScopedTransaction Transaction(LOCTEXT("RemoveCustomHlslPin", "BPI: Remove Custom HLSL Pin"));
	Node->Modify();
	// Unlinked pin with an untouched default -> ReallocatePins drops it instead of keeping it as an orphan.
	GetDefault<UEdGraphSchema_Niagara>()->BreakPinLinks(*Target, true);
	Target->DefaultValue = Target->AutogeneratedDefaultValue;
	if (bOutput)
	{
		Node->Signature.Outputs.RemoveAt(Index);
	}
	else
	{
		Node->Signature.Inputs.RemoveAt(Index);
	}
	MCPInspectorNiagaraScriptToolset::CallReallocatePins(Node);
	Touch(Ctx, Node);
	return ToJson(NodeToJson(Node));
}

FString UMCPInspectorNiagaraScriptToolset::ConnectPins(const FString& ScriptPath, const FString& FromNodeId, const FString& FromPin, const FString& ToNodeId, const FString& ToPin)
{
	using namespace MCPInspectorNiagaraScriptToolset;
	FScriptCtx Ctx;
	FString Error;
	if (!LoadCtx(ScriptPath, Ctx, Error)) return Err(Error);
	UEdGraphNode* FromNode = FindNode(Ctx.Graph, FromNodeId, Error);
	if (!FromNode) return Err(Error);
	UEdGraphNode* ToNode = FindNode(Ctx.Graph, ToNodeId, Error);
	if (!ToNode) return Err(Error);
	UEdGraphPin* A = FindPin(FromNode, FromPin, EGPD_Output, Error);
	if (!A) return Err(Error);
	UEdGraphPin* B = FindPin(ToNode, ToPin, EGPD_Input, Error);
	if (!B) return Err(Error);

	const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
	const FPinConnectionResponse Response = Schema->CanCreateConnection(A, B);
	if (Response.Response == CONNECT_RESPONSE_DISALLOW)
	{
		return Err(Response.Message.IsEmpty() ? FString(TEXT("connection not allowed")) : Response.Message.ToString());
	}
	const FScopedTransaction Transaction(LOCTEXT("ConnectPins", "BPI: Connect Niagara Pins"));
	Ctx.Graph->Modify();
	if (!Schema->TryCreateConnection(A, B))
	{
		return Err(Response.Message.IsEmpty() ? FString(TEXT("connection failed")) : Response.Message.ToString());
	}
	Touch(Ctx, ToNode);

	const UEnum* ResponseEnum = StaticEnum<ECanCreateConnectionResponse>();
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("response"), ResponseEnum ? ResponseEnum->GetNameStringByValue((int64)Response.Response.GetValue()) : FString::FromInt((int32)Response.Response.GetValue()));
	O->SetStringField(TEXT("message"), Response.Message.ToString());
	return ToJson(O);
}

FString UMCPInspectorNiagaraScriptToolset::DisconnectPin(const FString& ScriptPath, const FString& NodeId, const FString& Pin, const FString& OtherNodeId, const FString& OtherPin)
{
	using namespace MCPInspectorNiagaraScriptToolset;
	FScriptCtx Ctx;
	FString Error;
	if (!LoadCtx(ScriptPath, Ctx, Error)) return Err(Error);
	UEdGraphNode* Node = FindNode(Ctx.Graph, NodeId, Error);
	if (!Node) return Err(Error);
	UEdGraphPin* Target = FindPin(Node, Pin, EGPD_Input, Error);
	if (!Target) return Err(Error);

	UEdGraphPin* Other = nullptr;
	if (!OtherNodeId.TrimStartAndEnd().IsEmpty() || !OtherPin.TrimStartAndEnd().IsEmpty())
	{
		UEdGraphNode* OtherNode = FindNode(Ctx.Graph, OtherNodeId, Error);
		if (!OtherNode) return Err(Error);
		Other = FindPin(OtherNode, OtherPin, Target->Direction == EGPD_Input ? EGPD_Output : EGPD_Input, Error);
		if (!Other) return Err(Error);
		if (!Target->LinkedTo.Contains(Other)) return Err(TEXT("the pins are not linked"));
	}
	const int32 Count = Other ? 1 : Target->LinkedTo.Num();
	if (Count == 0) return Err(TEXT("pin has no links"));

	const FScopedTransaction Transaction(LOCTEXT("DisconnectPin", "BPI: Disconnect Niagara Pin"));
	Ctx.Graph->Modify();
	const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
	if (Other)
	{
		Schema->BreakSinglePinLink(Target, Other);
	}
	else
	{
		Schema->BreakPinLinks(*Target, true);
	}
	Touch(Ctx, Node);

	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("brokenLinks"), Count);
	return ToJson(O);
}

FString UMCPInspectorNiagaraScriptToolset::SetPinDefault(const FString& ScriptPath, const FString& NodeId, const FString& Pin, const FString& ValueText)
{
	using namespace MCPInspectorNiagaraScriptToolset;
	FScriptCtx Ctx;
	FString Error;
	if (!LoadCtx(ScriptPath, Ctx, Error)) return Err(Error);
	UEdGraphNode* Node = FindNode(Ctx.Graph, NodeId, Error);
	if (!Node) return Err(Error);
	UEdGraphPin* Target = FindPin(Node, Pin, EGPD_Input, Error);
	if (!Target) return Err(Error);
	if (Target->Direction != EGPD_Input) return Err(TEXT("only input pins have defaults"));
	if (Target->LinkedTo.Num() > 0) return Err(TEXT("pin is linked; its default is ignored (DisconnectPin first)"));
	if (Target->bDefaultValueIsIgnored) return Err(TEXT("this pin has no inline default"));
	if (Target->bDefaultValueIsReadOnly)
	{
		return Err(Cast<UNiagaraNodeParameterMapGet>(Node)
			? FString(TEXT("Map Get default pins follow the parameter default; use SetScriptInputDefault"))
			: FString(TEXT("pin default is read-only")));
	}

	const FNiagaraTypeDefinition Type = UEdGraphSchema_Niagara::PinToTypeDefinition(Target);
	FString Value = ValueText.TrimStartAndEnd();
	TSharedPtr<INiagaraEditorTypeUtilities, ESPMode::ThreadSafe> Utils;
	if (Type.IsValid())
	{
		Utils = GetTypeUtilities(Type);
	}
	if (Utils.IsValid() && Utils->CanHandlePinDefaults())
	{
		// Normalize through the type's pin-default parser (rejects text the compiler would not understand).
		FNiagaraVariable Parsed;
		if (!ParseValue(Type, Value, Parsed, Error)) return Err(Error);
		Value = ValueToString(Parsed);
	}

	const FScopedTransaction Transaction(LOCTEXT("SetPinDefault", "BPI: Set Niagara Pin Default"));
	Node->Modify();
	Target->Modify();
	GetDefault<UEdGraphSchema_Niagara>()->TrySetDefaultValue(*Target, Value, true);
	Touch(Ctx, Node);

	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("pin"), IdStr(Target->PinId));
	O->SetStringField(TEXT("default"), Target->DefaultValue);
	return ToJson(O);
}

FString UMCPInspectorNiagaraScriptToolset::CreateScratchPadScript(const FString& OwnerPath, const FString& Name, const FString& Usage, const FString& TargetStackUsage)
{
	using namespace MCPInspectorNiagaraScriptToolset;
	FScratchOwner Owner;
	FString Error;
	if (!ResolveScratchOwner(OwnerPath, Owner, Error)) return Err(Error);

	ENiagaraScriptUsage ParsedUsage;
	if (!ParseUsageName(Usage, ParsedUsage) || (ParsedUsage != ENiagaraScriptUsage::Module && ParsedUsage != ENiagaraScriptUsage::DynamicInput))
	{
		return Err(TEXT("scratch pad usage must be Module or DynamicInput: ") + Usage);
	}
	TOptional<ENiagaraScriptUsage> TargetUsage;
	if (!TargetStackUsage.TrimStartAndEnd().IsEmpty())
	{
		ENiagaraScriptUsage Parsed;
		if (!ParseUsageName(TargetStackUsage, Parsed)) return Err(TEXT("unknown target stack usage: ") + TargetStackUsage);
		TargetUsage = Parsed;
	}

	const FString BaseName = Name.TrimStartAndEnd().IsEmpty()
		? FString(ParsedUsage == ENiagaraScriptUsage::Module ? TEXT("ScratchModule") : TEXT("ScratchDynamicInput"))
		: Name.TrimStartAndEnd();
	const FName UniqueName = FNiagaraEditorUtilities::GetUniqueObjectName(Owner.Outer, UNiagaraScript::StaticClass(), BaseName);

	// UNiagaraScratchPadViewModel::CreateNewScript: transactional outer, duplicate of the default module / dynamic input
	// script (or the procedural default graph), non-public, appended to the owner's list, usage bit of the target stack.
	// The editor's factory does the same default-script duplication, so it is used here for the creation step.
	const FScopedTransaction Transaction(LOCTEXT("CreateScratchPadScript", "BPI: Create Niagara Scratch Pad Script"));
	if (!Owner.Outer->HasAnyFlags(RF_Transactional))
	{
		Owner.Outer->SetFlags(RF_Transactional);
	}
	Owner.Outer->Modify();
	UFactory* Factory = ParsedUsage == ENiagaraScriptUsage::Module
		? static_cast<UFactory*>(NewObject<UNiagaraModuleScriptFactory>())
		: static_cast<UFactory*>(NewObject<UNiagaraDynamicInputScriptFactory>());
	UNiagaraScript* NewScript = Cast<UNiagaraScript>(Factory->FactoryCreateNew(UNiagaraScript::StaticClass(), Owner.Outer, UniqueName, RF_Transactional, nullptr, GWarn));
	if (!NewScript) return Err(TEXT("scratch pad script creation failed"));
	NewScript->ClearFlags(RF_Public | RF_Standalone);
	if (TargetUsage.IsSet())
	{
		if (FVersionedNiagaraScriptData* Data = NewScript->GetLatestScriptData())
		{
			Data->ModuleUsageBitmask |= UsageBit(TargetUsage.GetValue());
		}
	}
	Owner.Scripts->Add(NewScript);
	if (Owner.Emitter)
	{
		Owner.Emitter->NotifyScratchPadScriptsChanged(); // UpdateChangeId in UNiagaraScratchPadViewModel (emitter asset mode)
	}
	Owner.Outer->MarkPackageDirty();

	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("path"), NewScript->GetPathName());
	O->SetStringField(TEXT("name"), NewScript->GetName());
	O->SetStringField(TEXT("usage"), UsageName(NewScript->GetUsage()));
	O->SetStringField(TEXT("owner"), Owner.System ? Owner.System->GetPathName() : Owner.Emitter->GetPathName());
	const FVersionedNiagaraScriptData* Data = NewScript->GetLatestScriptData();
	O->SetArrayField(TEXT("moduleUsages"), UsageMaskNames(Data ? Data->ModuleUsageBitmask : 0));
	return ToJson(O);
}

FString UMCPInspectorNiagaraScriptToolset::ListScratchPadScripts(const FString& OwnerPath)
{
	using namespace MCPInspectorNiagaraScriptToolset;
	FScratchOwner Owner;
	FString Error;
	if (!ResolveScratchOwner(OwnerPath, Owner, Error)) return Err(Error);

	TArray<TSharedPtr<FJsonValue>> Scripts;
	for (const TObjectPtr<UNiagaraScript>& Script : *Owner.Scripts)
	{
		if (!Script)
		{
			continue;
		}
		TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetStringField(TEXT("path"), Script->GetPathName());
		S->SetStringField(TEXT("name"), Script->GetName());
		S->SetStringField(TEXT("usage"), UsageName(Script->GetUsage()));
		const FVersionedNiagaraScriptData* Data = Script->GetLatestScriptData();
		S->SetArrayField(TEXT("moduleUsages"), UsageMaskNames(Data ? Data->ModuleUsageBitmask : 0));
		S->SetStringField(TEXT("compileStatus"), CompileStatusName(Script->GetLastCompileStatus()));
		Scripts.Add(MakeShared<FJsonValueObject>(S));
	}
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("owner"), Owner.System ? Owner.System->GetPathName() : Owner.Emitter->GetPathName());
	O->SetArrayField(TEXT("scripts"), Scripts);
	return ToJson(O);
}

#undef LOCTEXT_NAMESPACE
