#include "Commands/NiagaraCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"
#include "Misc/StringOutputDevice.h"

#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraTypes.h"
#include "NiagaraClipboard.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraRendererProperties.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackFunctionInput.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackRendererItem.h"
#include "ViewModels/Stack/NiagaraStackScriptItemGroup.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

// Named rather than anonymous namespace on purpose: UBT's unity build merges command .cpp
// files into one translation unit, where same-named helpers in two anonymous namespaces
// collide. See the note in CommandJsonHelpers.h.
namespace UnrealMCPBridge::NiagaraCmd
{
    using UnrealMCPBridge::Json::RequireString;

    void Fail(MCPProtocol::FMCPError& OutError, int32 Code, const FString& Message)
    {
        OutError.Code = Code;
        OutError.Message = Message;
    }

    UNiagaraSystem* LoadSystem(const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        FString Path;
        if (!RequireString(Params, TEXT("system_path"), Path, OutError))
        {
            return nullptr;
        }
        UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *Path);
        if (!System)
        {
            Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                FString::Printf(TEXT("Could not load NiagaraSystem: %s"), *Path));
            return nullptr;
        }
        System->EnsureFullyLoaded();
        return System;
    }

    /**
     * Builds a throwaway system view model in data-processing mode — no preview world, no
     * sequencer, no undo registration. Same options Epic's audit commandlet and the emitter
     * version-upgrade path use, so this is a supported headless configuration.
     *
     * Deliberately not cached between commands: the view model is an FTickableEditorObject
     * that registers delegates on the system, and a stale one left alive across an asset
     * reload or an editor open would fight the real toolkit. Initialize() is cheap in this
     * mode because it skips the preview component and the compile.
     */
    TSharedPtr<FNiagaraSystemViewModel> MakeDataOnlyViewModel(UNiagaraSystem& System)
    {
        TSharedPtr<FNiagaraSystemViewModel> ViewModel = MakeShared<FNiagaraSystemViewModel>();
        FNiagaraSystemViewModelOptions Options;
        Options.bCanModifyEmittersFromTimeline = false;
        Options.bCanSimulate = false;
        Options.bCanAutoCompile = false;
        Options.bIsForDataProcessingOnly = true;
        Options.MessageLogGuid = System.GetAssetGuid();
        ViewModel->Initialize(System, Options);
        return ViewModel;
    }

    TSharedPtr<FNiagaraEmitterHandleViewModel> FindEmitterHandle(FNiagaraSystemViewModel& ViewModel, const FString& EmitterName)
    {
        for (const TSharedRef<FNiagaraEmitterHandleViewModel>& Candidate : ViewModel.GetEmitterHandleViewModels())
        {
            if (Candidate->GetName().ToString().Equals(EmitterName, ESearchCase::IgnoreCase))
            {
                return Candidate;
            }
        }
        return nullptr;
    }

    // Accepts both the short form used in the Niagara UI ("ParticleSpawn") and the enum name
    // ("ParticleSpawnScript").
    bool ParseUsage(const FString& In, ENiagaraScriptUsage& Out)
    {
        const FString Key = In.Replace(TEXT("Script"), TEXT("")).Replace(TEXT("_"), TEXT("")).TrimStartAndEnd();
        if (Key.Equals(TEXT("SystemSpawn"), ESearchCase::IgnoreCase))    { Out = ENiagaraScriptUsage::SystemSpawnScript;    return true; }
        if (Key.Equals(TEXT("SystemUpdate"), ESearchCase::IgnoreCase))   { Out = ENiagaraScriptUsage::SystemUpdateScript;   return true; }
        if (Key.Equals(TEXT("EmitterSpawn"), ESearchCase::IgnoreCase))   { Out = ENiagaraScriptUsage::EmitterSpawnScript;   return true; }
        if (Key.Equals(TEXT("EmitterUpdate"), ESearchCase::IgnoreCase))  { Out = ENiagaraScriptUsage::EmitterUpdateScript;  return true; }
        if (Key.Equals(TEXT("ParticleSpawn"), ESearchCase::IgnoreCase))  { Out = ENiagaraScriptUsage::ParticleSpawnScript;  return true; }
        if (Key.Equals(TEXT("ParticleUpdate"), ESearchCase::IgnoreCase)) { Out = ENiagaraScriptUsage::ParticleUpdateScript; return true; }
        if (Key.Equals(TEXT("ParticleEvent"), ESearchCase::IgnoreCase))  { Out = ENiagaraScriptUsage::ParticleEventScript;  return true; }
        return false;
    }

    bool IsSystemLevelUsage(ENiagaraScriptUsage Usage)
    {
        return Usage == ENiagaraScriptUsage::SystemSpawnScript || Usage == ENiagaraScriptUsage::SystemUpdateScript;
    }

    FString UsageToString(ENiagaraScriptUsage Usage)
    {
        switch (Usage)
        {
        case ENiagaraScriptUsage::SystemSpawnScript:    return TEXT("SystemSpawn");
        case ENiagaraScriptUsage::SystemUpdateScript:   return TEXT("SystemUpdate");
        case ENiagaraScriptUsage::EmitterSpawnScript:   return TEXT("EmitterSpawn");
        case ENiagaraScriptUsage::EmitterUpdateScript:  return TEXT("EmitterUpdate");
        case ENiagaraScriptUsage::ParticleSpawnScript:  return TEXT("ParticleSpawn");
        case ENiagaraScriptUsage::ParticleUpdateScript: return TEXT("ParticleUpdate");
        case ENiagaraScriptUsage::ParticleEventScript:  return TEXT("ParticleEvent");
        default:                                        return TEXT("Other");
        }
    }

    void VisitStack(UNiagaraStackEntry* Entry, TFunctionRef<void(UNiagaraStackEntry&)> Visitor)
    {
        if (!Entry)
        {
            return;
        }
        Visitor(*Entry);

        TArray<UNiagaraStackEntry*> Children;
        Entry->GetUnfilteredChildren(Children);
        for (UNiagaraStackEntry* Child : Children)
        {
            VisitStack(Child, Visitor);
        }
    }

    /** A resolved (system, emitter, script group) triple. Holds the view model alive for the caller. */
    struct FStackTarget
    {
        UNiagaraSystem* System = nullptr;
        TSharedPtr<FNiagaraSystemViewModel> ViewModel;
        UNiagaraStackScriptItemGroup* Group = nullptr;
    };

    bool ResolveStack(const TSharedPtr<FJsonObject>& Params, FStackTarget& Out, MCPProtocol::FMCPError& OutError)
    {
        Out.System = LoadSystem(Params, OutError);
        if (!Out.System)
        {
            return false;
        }

        FString StackName;
        if (!RequireString(Params, TEXT("stack"), StackName, OutError))
        {
            return false;
        }
        ENiagaraScriptUsage Usage;
        if (!ParseUsage(StackName, Usage))
        {
            Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                FString::Printf(TEXT("Unknown stack '%s'. Expected one of SystemSpawn, SystemUpdate, EmitterSpawn, EmitterUpdate, ParticleSpawn, ParticleUpdate, ParticleEvent."), *StackName));
            return false;
        }

        FString EmitterName;
        Params->TryGetStringField(TEXT("emitter_name"), EmitterName);

        Out.ViewModel = MakeDataOnlyViewModel(*Out.System);

        UNiagaraStackViewModel* StackViewModel = nullptr;
        if (IsSystemLevelUsage(Usage))
        {
            if (!EmitterName.IsEmpty())
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                    FString::Printf(TEXT("stack '%s' is system-level; leave emitter_name empty."), *StackName));
                return false;
            }
            StackViewModel = Out.ViewModel->GetSystemStackViewModel();
        }
        else
        {
            if (EmitterName.IsEmpty())
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                    FString::Printf(TEXT("stack '%s' is emitter-level; emitter_name is required."), *StackName));
                return false;
            }

            TSharedPtr<FNiagaraEmitterHandleViewModel> HandleViewModel = FindEmitterHandle(*Out.ViewModel, EmitterName);
            if (!HandleViewModel.IsValid())
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                    FString::Printf(TEXT("Emitter '%s' not found in %s."), *EmitterName, *Out.System->GetPathName()));
                return false;
            }
            StackViewModel = HandleViewModel->GetEmitterStackViewModel();
        }

        if (!StackViewModel)
        {
            Fail(OutError, MCPProtocol::FMCPError::InternalError, TEXT("Stack view model unavailable."));
            return false;
        }

        VisitStack(StackViewModel->GetRootEntry(), [&Out, Usage](UNiagaraStackEntry& Entry)
        {
            if (Out.Group)
            {
                return;
            }
            if (UNiagaraStackScriptItemGroup* Candidate = Cast<UNiagaraStackScriptItemGroup>(&Entry))
            {
                const UNiagaraNodeOutput* OutputNode = Candidate->GetScriptOutputNode();
                if (OutputNode && OutputNode->GetUsage() == Usage)
                {
                    Out.Group = Candidate;
                }
            }
        });

        if (!Out.Group)
        {
            Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                FString::Printf(TEXT("Script group '%s' not found on the requested stack."), *StackName));
            return false;
        }
        return true;
    }

    /** A resolved (system, emitter) pair — what the renderer and emitter commands work on. */
    struct FEmitterTarget
    {
        UNiagaraSystem* System = nullptr;
        TSharedPtr<FNiagaraSystemViewModel> ViewModel;
        TSharedPtr<FNiagaraEmitterHandleViewModel> Emitter;
    };

    bool ResolveEmitter(const TSharedPtr<FJsonObject>& Params, FEmitterTarget& Out, MCPProtocol::FMCPError& OutError)
    {
        Out.System = LoadSystem(Params, OutError);
        if (!Out.System)
        {
            return false;
        }

        FString EmitterName;
        if (!RequireString(Params, TEXT("emitter_name"), EmitterName, OutError))
        {
            return false;
        }

        Out.ViewModel = MakeDataOnlyViewModel(*Out.System);
        Out.Emitter = FindEmitterHandle(*Out.ViewModel, EmitterName);
        if (!Out.Emitter.IsValid())
        {
            Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                FString::Printf(TEXT("Emitter '%s' not found in %s. Call niagara.get_info to list them."),
                    *EmitterName, *Out.System->GetPathName()));
            return false;
        }
        return true;
    }

    void CollectRenderers(FNiagaraEmitterHandleViewModel& Emitter, TArray<UNiagaraStackRendererItem*>& Out)
    {
        TArray<UNiagaraStackEntry*> Entries;
        Emitter.GetRendererEntries(Entries);
        for (UNiagaraStackEntry* Entry : Entries)
        {
            if (UNiagaraStackRendererItem* Item = Cast<UNiagaraStackRendererItem>(Entry))
            {
                Out.Add(Item);
            }
        }
    }

    /** Renderers are addressed by their position in niagara.renderer_list — they have no stable name. */
    UNiagaraStackRendererItem* FindRenderer(FNiagaraEmitterHandleViewModel& Emitter, const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        double IndexValue = 0.0;
        if (!Params->TryGetNumberField(TEXT("renderer_index"), IndexValue))
        {
            Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                TEXT("renderer_index is required; call niagara.renderer_list to see the indices"));
            return nullptr;
        }

        TArray<UNiagaraStackRendererItem*> Renderers;
        CollectRenderers(Emitter, Renderers);

        const int32 Index = static_cast<int32>(IndexValue);
        if (!Renderers.IsValidIndex(Index))
        {
            Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                FString::Printf(TEXT("renderer_index %d is out of range; this emitter has %d renderer(s)."), Index, Renderers.Num()));
            return nullptr;
        }
        return Renderers[Index];
    }

    void CollectModules(UNiagaraStackScriptItemGroup& Group, TArray<UNiagaraStackModuleItem*>& Out)
    {
        VisitStack(&Group, [&Out](UNiagaraStackEntry& Entry)
        {
            if (UNiagaraStackModuleItem* Module = Cast<UNiagaraStackModuleItem>(&Entry))
            {
                Out.Add(Module);
            }
        });
        Out.Sort([](const UNiagaraStackModuleItem& A, const UNiagaraStackModuleItem& B)
        {
            return A.GetModuleIndex() < B.GetModuleIndex();
        });
    }

    /** Matches on the module's function name first, then its display name. */
    UNiagaraStackModuleItem* FindModule(UNiagaraStackScriptItemGroup& Group, const FString& Name)
    {
        TArray<UNiagaraStackModuleItem*> Modules;
        CollectModules(Group, Modules);
        for (UNiagaraStackModuleItem* Module : Modules)
        {
            if (Module->GetModuleNode().GetFunctionName().Equals(Name, ESearchCase::IgnoreCase))
            {
                return Module;
            }
        }
        for (UNiagaraStackModuleItem* Module : Modules)
        {
            if (Module->GetDisplayName().ToString().Equals(Name, ESearchCase::IgnoreCase))
            {
                return Module;
            }
        }
        return nullptr;
    }

    void CollectInputs(UNiagaraStackModuleItem& Module, TArray<UNiagaraStackFunctionInput*>& Out)
    {
        VisitStack(&Module, [&Out](UNiagaraStackEntry& Entry)
        {
            if (UNiagaraStackFunctionInput* Input = Cast<UNiagaraStackFunctionInput>(&Entry))
            {
                Out.Add(Input);
            }
        });
    }

    UNiagaraStackFunctionInput* FindInput(UNiagaraStackModuleItem& Module, const FString& Name)
    {
        TArray<UNiagaraStackFunctionInput*> Inputs;
        CollectInputs(Module, Inputs);
        for (UNiagaraStackFunctionInput* Input : Inputs)
        {
            const FNiagaraParameterHandle& Handle = Input->GetInputParameterHandle();
            if (Handle.GetName().ToString().Equals(Name, ESearchCase::IgnoreCase) ||
                Handle.GetParameterHandleString().ToString().Equals(Name, ESearchCase::IgnoreCase))
            {
                return Input;
            }
        }
        return nullptr;
    }

    FString ValueModeToString(UNiagaraStackFunctionInput::EValueMode Mode)
    {
        switch (Mode)
        {
        case UNiagaraStackFunctionInput::EValueMode::Local:              return TEXT("Local");
        case UNiagaraStackFunctionInput::EValueMode::Linked:             return TEXT("Linked");
        case UNiagaraStackFunctionInput::EValueMode::LinkedShared:       return TEXT("LinkedShared");
        case UNiagaraStackFunctionInput::EValueMode::Dynamic:            return TEXT("Dynamic");
        case UNiagaraStackFunctionInput::EValueMode::Data:               return TEXT("Data");
        case UNiagaraStackFunctionInput::EValueMode::ObjectAsset:        return TEXT("ObjectAsset");
        case UNiagaraStackFunctionInput::EValueMode::Expression:         return TEXT("Expression");
        case UNiagaraStackFunctionInput::EValueMode::DefaultFunction:    return TEXT("DefaultFunction");
        case UNiagaraStackFunctionInput::EValueMode::InvalidOverride:    return TEXT("InvalidOverride");
        case UNiagaraStackFunctionInput::EValueMode::UnsupportedDefault: return TEXT("UnsupportedDefault");
        default:                                                        return TEXT("None");
        }
    }

    /**
     * Renders a local value in the same comma form input_set_local accepts, so a value read
     * out of input_list can be written straight back.
     *
     * Not UScriptStruct::ExportText: that omits every field still at its default, so a false
     * bool comes out as "()" and a vector of (1,0,0) loses its zero components. Walking the
     * numeric leaves keeps every component and matches BuildLocalValueData's field order.
     * Anything with a non-numeric leaf (enums, nested structs) falls back to import text.
     */
    FString FormatLocalValue(const UScriptStruct& Struct, const uint8* Data)
    {
        if (&Struct == FNiagaraBool::StaticStruct())
        {
            return reinterpret_cast<const FNiagaraBool*>(Data)->GetValue() ? TEXT("true") : TEXT("false");
        }

        TArray<FString> Parts;
        for (TFieldIterator<FProperty> It(&Struct); It; ++It)
        {
            const FProperty* Property = *It;
            const void* Address = Property->ContainerPtrToValuePtr<const void>(Data);

            if (const FFloatProperty* AsFloat = CastField<FFloatProperty>(Property))
            {
                Parts.Add(FString::SanitizeFloat(AsFloat->GetPropertyValue(Address)));
            }
            else if (const FDoubleProperty* AsDouble = CastField<FDoubleProperty>(Property))
            {
                Parts.Add(FString::SanitizeFloat(AsDouble->GetPropertyValue(Address)));
            }
            else if (const FIntProperty* AsInt = CastField<FIntProperty>(Property))
            {
                Parts.Add(FString::FromInt(AsInt->GetPropertyValue(Address)));
            }
            else if (const FBoolProperty* AsBool = CastField<FBoolProperty>(Property))
            {
                Parts.Add(AsBool->GetPropertyValue(Address) ? TEXT("true") : TEXT("false"));
            }
            else
            {
                Parts.Reset();
                break;
            }
        }

        if (Parts.Num() > 0)
        {
            return FString::Join(Parts, TEXT(","));
        }

        FString Text;
        const_cast<UScriptStruct&>(Struct).ExportText(Text, Data, nullptr, nullptr, PPF_None, nullptr);
        return Text;
    }

    /** Human-readable current value, in the same shape input_set_local accepts back. */
    FString DescribeInputValue(UNiagaraStackFunctionInput& Input)
    {
        switch (Input.GetValueMode())
        {
        case UNiagaraStackFunctionInput::EValueMode::Local:
        {
            TSharedPtr<const FStructOnScope> ValueStruct = Input.GetLocalValueStruct();
            if (!ValueStruct.IsValid() || !ValueStruct->GetStructMemory())
            {
                return FString();
            }
            const UScriptStruct* Struct = Cast<const UScriptStruct>(ValueStruct->GetStruct());
            if (!Struct)
            {
                return FString();
            }
            return FormatLocalValue(*Struct, ValueStruct->GetStructMemory());
        }
        case UNiagaraStackFunctionInput::EValueMode::Linked:
        case UNiagaraStackFunctionInput::EValueMode::LinkedShared:
            return Input.GetLinkedParameterValue().GetName().ToString();
        case UNiagaraStackFunctionInput::EValueMode::Dynamic:
        {
            UNiagaraNodeFunctionCall* DynamicNode = Input.GetDynamicInputNode();
            return DynamicNode ? DynamicNode->GetFunctionName() : FString();
        }
        case UNiagaraStackFunctionInput::EValueMode::Data:
        {
            UNiagaraDataInterface* Data = Input.GetDataValueObject();
            return Data ? Data->GetClass()->GetName() : FString();
        }
        case UNiagaraStackFunctionInput::EValueMode::ObjectAsset:
        {
            UObject* Asset = Input.GetObjectAssetValue();
            return Asset ? Asset->GetPathName() : FString();
        }
        default:
            return FString();
        }
    }

    /**
     * Fills a raw Niagara value buffer from text.
     *
     * Two accepted forms, so callers can use whichever is natural:
     *   - Unreal import text, when it starts with '(' — "(X=1.0,Y=2.0,Z=0.0)". Handles every
     *     type the reflection system knows, including enums and nested structs.
     *   - A bare scalar or comma-separated list — "8", "true", "1,0,0.5". Written into the
     *     struct's numeric/bool leaf properties in declaration order, which covers float,
     *     int, bool, Vector2/3/4, Position, Quat and LinearColor.
     */
    bool BuildLocalValueData(const FNiagaraTypeDefinition& Type, const FString& ValueText, TArray<uint8>& OutData, FString& OutErrorText)
    {
        UScriptStruct* Struct = Type.GetScriptStruct();
        if (!Struct)
        {
            OutErrorText = TEXT("input type has no script struct; use input_set_linked or input_set_dynamic instead");
            return false;
        }

        const int32 Size = FMath::Max(Type.GetSize(), Struct->GetStructureSize());
        OutData.SetNumZeroed(Size);
        Struct->InitializeStruct(OutData.GetData());

        const FString Trimmed = ValueText.TrimStartAndEnd();
        if (Trimmed.StartsWith(TEXT("(")))
        {
            FStringOutputDevice ImportErrors;
            const TCHAR* Consumed = Struct->ImportText(*Trimmed, OutData.GetData(), nullptr, PPF_None, &ImportErrors, Struct->GetName());
            if (Consumed == nullptr)
            {
                OutErrorText = ImportErrors.IsEmpty() ? TEXT("could not parse import text") : FString(ImportErrors);
                return false;
            }
            return true;
        }

        TArray<FString> Tokens;
        Trimmed.ParseIntoArray(Tokens, TEXT(","), true);
        if (Tokens.Num() == 0)
        {
            OutErrorText = TEXT("value is empty");
            return false;
        }

        // FNiagaraBool wraps an int32 that the VM requires to be exactly 0 or -1, so the generic
        // numeric loop below gets it wrong twice over: Atoi("true") is 0 — the opposite of what
        // was asked — and "1" is neither of the two legal values.
        if (Struct == FNiagaraBool::StaticStruct())
        {
            reinterpret_cast<FNiagaraBool*>(OutData.GetData())->SetValue(Tokens[0].TrimStartAndEnd().ToBool());
            return true;
        }

        int32 TokenIndex = 0;
        for (TFieldIterator<FProperty> It(Struct); It && TokenIndex < Tokens.Num(); ++It)
        {
            FProperty* Property = *It;
            void* Address = Property->ContainerPtrToValuePtr<void>(OutData.GetData());
            const FString Token = Tokens[TokenIndex].TrimStartAndEnd();

            if (FFloatProperty* AsFloat = CastField<FFloatProperty>(Property))
            {
                AsFloat->SetPropertyValue(Address, FCString::Atof(*Token));
            }
            else if (FDoubleProperty* AsDouble = CastField<FDoubleProperty>(Property))
            {
                AsDouble->SetPropertyValue(Address, FCString::Atod(*Token));
            }
            else if (FIntProperty* AsInt = CastField<FIntProperty>(Property))
            {
                AsInt->SetPropertyValue(Address, FCString::Atoi(*Token));
            }
            else if (FBoolProperty* AsBool = CastField<FBoolProperty>(Property))
            {
                AsBool->SetPropertyValue(Address, Token.ToBool());
            }
            else
            {
                OutErrorText = FString::Printf(
                    TEXT("property '%s' of type %s cannot be set from a plain value; pass Unreal import text starting with '(' instead"),
                    *Property->GetName(), *Type.GetName());
                return false;
            }
            ++TokenIndex;
        }

        if (TokenIndex == 0)
        {
            OutErrorText = FString::Printf(TEXT("type %s has no settable numeric fields"), *Type.GetName());
            return false;
        }
        return true;
    }

    TSharedPtr<FJsonObject> MakeOkResult()
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("ok"), true);
        return Result;
    }

    /** Shared prologue for every input.* command: resolve stack, module and input. */
    bool ResolveInput(const TSharedPtr<FJsonObject>& Params, FStackTarget& Target, UNiagaraStackFunctionInput*& OutInput, MCPProtocol::FMCPError& OutError)
    {
        if (!ResolveStack(Params, Target, OutError))
        {
            return false;
        }

        FString ModuleName;
        FString InputName;
        if (!RequireString(Params, TEXT("module_name"), ModuleName, OutError)) return false;
        if (!RequireString(Params, TEXT("input_name"), InputName, OutError))   return false;

        UNiagaraStackModuleItem* Module = FindModule(*Target.Group, ModuleName);
        if (!Module)
        {
            Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                FString::Printf(TEXT("Module '%s' not found. Call niagara.module_list to see what is on this stack."), *ModuleName));
            return false;
        }

        OutInput = FindInput(*Module, InputName);
        if (!OutInput)
        {
            Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                FString::Printf(TEXT("Input '%s' not found on module '%s'. Call niagara.input_list to see available inputs."), *InputName, *ModuleName));
            return false;
        }
        return true;
    }
}

void FNiagaraCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    using namespace UnrealMCPBridge::NiagaraCmd;

    // niagara.get_info — emitters and the script stacks available on each. Params: system_path.
    Registry.Register(TEXT("niagara.get_info"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UNiagaraSystem* System = LoadSystem(Params, OutError);
            if (!System) return nullptr;

            TSharedPtr<FNiagaraSystemViewModel> ViewModel = MakeDataOnlyViewModel(*System);

            // Collects the script groups reachable from one stack view model.
            auto GatherStacks = [](UNiagaraStackViewModel* StackViewModel)
            {
                TArray<TSharedPtr<FJsonValue>> Stacks;
                if (StackViewModel)
                {
                    VisitStack(StackViewModel->GetRootEntry(), [&Stacks](UNiagaraStackEntry& Entry)
                    {
                        UNiagaraStackScriptItemGroup* Group = Cast<UNiagaraStackScriptItemGroup>(&Entry);
                        const UNiagaraNodeOutput* OutputNode = Group ? Group->GetScriptOutputNode() : nullptr;
                        if (OutputNode)
                        {
                            TArray<UNiagaraStackModuleItem*> Modules;
                            CollectModules(*Group, Modules);

                            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
                            Obj->SetStringField(TEXT("stack"), UsageToString(OutputNode->GetUsage()));
                            Obj->SetNumberField(TEXT("module_count"), Modules.Num());
                            Stacks.Add(MakeShared<FJsonValueObject>(Obj));
                        }
                    });
                }
                return Stacks;
            };

            TArray<TSharedPtr<FJsonValue>> Emitters;
            for (const TSharedRef<FNiagaraEmitterHandleViewModel>& Handle : ViewModel->GetEmitterHandleViewModels())
            {
                TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
                Obj->SetStringField(TEXT("name"), Handle->GetName().ToString());
                Obj->SetStringField(TEXT("id"), Handle->GetId().ToString());
                Obj->SetBoolField(TEXT("enabled"), Handle->GetIsEnabled());
                Obj->SetArrayField(TEXT("stacks"), GatherStacks(Handle->GetEmitterStackViewModel()));
                Emitters.Add(MakeShared<FJsonValueObject>(Obj));
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("system"), System->GetPathName());
            Result->SetArrayField(TEXT("system_stacks"), GatherStacks(ViewModel->GetSystemStackViewModel()));
            Result->SetArrayField(TEXT("emitters"), Emitters);
            return Result;
        });

    // niagara.module_list — modules on one script stack, in execution order.
    // Params: system_path, stack, emitter_name (emitter/particle stacks only).
    Registry.Register(TEXT("niagara.module_list"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FStackTarget Target;
            if (!ResolveStack(Params, Target, OutError)) return nullptr;

            TArray<UNiagaraStackModuleItem*> Modules;
            CollectModules(*Target.Group, Modules);

            TArray<TSharedPtr<FJsonValue>> Entries;
            for (UNiagaraStackModuleItem* Module : Modules)
            {
                const UNiagaraNodeFunctionCall& Node = Module->GetModuleNode();

                TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
                Obj->SetStringField(TEXT("name"), Node.GetFunctionName());
                Obj->SetStringField(TEXT("display_name"), Module->GetDisplayName().ToString());
                Obj->SetNumberField(TEXT("index"), Module->GetModuleIndex());
                Obj->SetBoolField(TEXT("enabled"), Module->GetIsEnabled());
                Obj->SetBoolField(TEXT("inherited"), Module->GetIsInherited());
                Obj->SetBoolField(TEXT("can_move_and_delete"), Module->CanMoveAndDelete());
                Obj->SetStringField(TEXT("script"), Node.FunctionScript ? Node.FunctionScript->GetPathName() : FString());
                Entries.Add(MakeShared<FJsonValueObject>(Obj));
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("modules"), Entries);
            return Result;
        });

    // niagara.module_add — inserts a module script onto a stack.
    // Params: system_path, stack, emitter_name, module_script_path,
    //         target_index (optional, -1 = append), suggested_name (optional).
    Registry.Register(TEXT("niagara.module_add"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FStackTarget Target;
            if (!ResolveStack(Params, Target, OutError)) return nullptr;

            FString ScriptPath;
            if (!RequireString(Params, TEXT("module_script_path"), ScriptPath, OutError)) return nullptr;

            UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, *ScriptPath);
            if (!ModuleScript)
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                    FString::Printf(TEXT("Could not load NiagaraScript: %s"), *ScriptPath));
                return nullptr;
            }
            if (!ModuleScript->IsModuleScript())
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                    FString::Printf(TEXT("%s is not a module script (usage is %d). Dynamic inputs go through niagara.input_set_dynamic."),
                        *ScriptPath, static_cast<int32>(ModuleScript->GetUsage())));
                return nullptr;
            }

            int32 TargetIndex = INDEX_NONE;
            double IndexValue = 0.0;
            if (Params->TryGetNumberField(TEXT("target_index"), IndexValue))
            {
                TargetIndex = static_cast<int32>(IndexValue);
            }
            FString SuggestedName;
            Params->TryGetStringField(TEXT("suggested_name"), SuggestedName);

            UNiagaraNodeOutput* OutputNode = Target.Group->GetScriptOutputNode();
            if (!OutputNode)
            {
                Fail(OutError, MCPProtocol::FMCPError::InternalError, TEXT("Script group has no output node."));
                return nullptr;
            }

            UNiagaraNodeFunctionCall* AddedNode =
                FNiagaraStackGraphUtilities::AddScriptModuleToStack(ModuleScript, *OutputNode, TargetIndex, SuggestedName);
            if (!AddedNode)
            {
                Fail(OutError, MCPProtocol::FMCPError::InternalError,
                    FString::Printf(TEXT("Niagara refused to add %s to this stack."), *ScriptPath));
                return nullptr;
            }

            // The stack UI's post-add step (UNiagaraStackScriptItemGroup::ItemAdded) is private;
            // its only non-selection work is this refresh, which rebuilds the group's children so
            // the new module item exists and can be looked up below.
            Target.Group->RefreshChildren();
            Target.System->MarkPackageDirty();

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("module_name"), AddedNode->GetFunctionName());
            if (UNiagaraStackModuleItem* Added = FindModule(*Target.Group, AddedNode->GetFunctionName()))
            {
                Result->SetNumberField(TEXT("index"), Added->GetModuleIndex());
            }
            return Result;
        });

    // niagara.module_remove — deletes a module from a stack.
    // Params: system_path, stack, emitter_name, module_name.
    Registry.Register(TEXT("niagara.module_remove"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FStackTarget Target;
            if (!ResolveStack(Params, Target, OutError)) return nullptr;

            FString ModuleName;
            if (!RequireString(Params, TEXT("module_name"), ModuleName, OutError)) return nullptr;

            UNiagaraStackModuleItem* Module = FindModule(*Target.Group, ModuleName);
            if (!Module)
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                    FString::Printf(TEXT("Module '%s' not found on this stack."), *ModuleName));
                return nullptr;
            }
            // Delete() assert-checks this internally, so screen it here rather than crash the editor.
            if (!Module->CanMoveAndDelete())
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                    FString::Printf(TEXT("Module '%s' cannot be deleted (it is required by the stack or inherited from a parent emitter)."), *ModuleName));
                return nullptr;
            }

            Module->Delete();
            Target.System->MarkPackageDirty();
            return MakeOkResult();
        });

    // niagara.module_set_enabled — toggles a module without removing it.
    // Params: system_path, stack, emitter_name, module_name, enabled.
    Registry.Register(TEXT("niagara.module_set_enabled"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FStackTarget Target;
            if (!ResolveStack(Params, Target, OutError)) return nullptr;

            FString ModuleName;
            if (!RequireString(Params, TEXT("module_name"), ModuleName, OutError)) return nullptr;

            bool bEnabled = true;
            if (!Params->TryGetBoolField(TEXT("enabled"), bEnabled))
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams, TEXT("enabled is required and must be a boolean"));
                return nullptr;
            }

            UNiagaraStackModuleItem* Module = FindModule(*Target.Group, ModuleName);
            if (!Module)
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                    FString::Printf(TEXT("Module '%s' not found on this stack."), *ModuleName));
                return nullptr;
            }

            Module->SetEnabled(bEnabled);
            Target.System->MarkPackageDirty();
            return MakeOkResult();
        });

    // niagara.input_list — inputs of one module with their current values.
    // Params: system_path, stack, emitter_name, module_name.
    Registry.Register(TEXT("niagara.input_list"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FStackTarget Target;
            if (!ResolveStack(Params, Target, OutError)) return nullptr;

            FString ModuleName;
            if (!RequireString(Params, TEXT("module_name"), ModuleName, OutError)) return nullptr;

            UNiagaraStackModuleItem* Module = FindModule(*Target.Group, ModuleName);
            if (!Module)
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                    FString::Printf(TEXT("Module '%s' not found on this stack."), *ModuleName));
                return nullptr;
            }

            TArray<UNiagaraStackFunctionInput*> Inputs;
            CollectInputs(*Module, Inputs);

            TArray<TSharedPtr<FJsonValue>> Entries;
            for (UNiagaraStackFunctionInput* Input : Inputs)
            {
                TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
                Obj->SetStringField(TEXT("name"), Input->GetInputParameterHandle().GetName().ToString());
                Obj->SetStringField(TEXT("handle"), Input->GetInputParameterHandle().GetParameterHandleString().ToString());
                Obj->SetStringField(TEXT("type"), Input->GetInputType().GetName());
                Obj->SetStringField(TEXT("value_mode"), ValueModeToString(Input->GetValueMode()));
                Obj->SetStringField(TEXT("value"), DescribeInputValue(*Input));
                Obj->SetBoolField(TEXT("can_reset"), Input->CanReset());
                Entries.Add(MakeShared<FJsonValueObject>(Obj));
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("inputs"), Entries);
            return Result;
        });

    // niagara.input_set_local — sets a module input to a constant.
    // Params: system_path, stack, emitter_name, module_name, input_name, value.
    // value accepts "8", "true", "1,0,0.5" or Unreal import text "(X=1,Y=2,Z=0)".
    Registry.Register(TEXT("niagara.input_set_local"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FStackTarget Target;
            UNiagaraStackFunctionInput* Input = nullptr;
            if (!ResolveInput(Params, Target, Input, OutError)) return nullptr;

            FString ValueText;
            if (!Params->TryGetStringField(TEXT("value"), ValueText))
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams, TEXT("value is required"));
                return nullptr;
            }

            const FNiagaraTypeDefinition& Type = Input->GetInputType();
            TArray<uint8> ValueData;
            FString ParseError;
            if (!BuildLocalValueData(Type, ValueText, ValueData, ParseError))
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                    FString::Printf(TEXT("Could not read value '%s' as %s: %s"), *ValueText, *Type.GetName(), *ParseError));
                return nullptr;
            }

            const UNiagaraClipboardFunctionInput* ClipboardInput = UNiagaraClipboardFunctionInput::CreateLocalValue(
                GetTransientPackage(), Input->GetInputParameterHandle().GetName(), Type, TOptional<bool>(), ValueData);
            if (!ClipboardInput)
            {
                Fail(OutError, MCPProtocol::FMCPError::InternalError, TEXT("Could not build a clipboard value for this input."));
                return nullptr;
            }

            Input->SetValueFromClipboardFunctionInput(*ClipboardInput);
            Target.System->MarkPackageDirty();
            return MakeOkResult();
        });

    // niagara.input_set_linked — points a module input at another parameter.
    // Params: system_path, stack, emitter_name, module_name, input_name, parameter.
    Registry.Register(TEXT("niagara.input_set_linked"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FStackTarget Target;
            UNiagaraStackFunctionInput* Input = nullptr;
            if (!ResolveInput(Params, Target, Input, OutError)) return nullptr;

            FString ParameterName;
            if (!RequireString(Params, TEXT("parameter"), ParameterName, OutError)) return nullptr;

            // The linked parameter must carry the input's own type, otherwise the stack rejects it.
            const FNiagaraVariableBase LinkedParameter(Input->GetInputType(), FName(*ParameterName));
            Input->SetLinkedParameterValue(LinkedParameter);
            Target.System->MarkPackageDirty();
            return MakeOkResult();
        });

    // niagara.input_set_dynamic — drives a module input with a dynamic input script.
    // Params: system_path, stack, emitter_name, module_name, input_name,
    //         dynamic_input_script_path, suggested_name (optional).
    Registry.Register(TEXT("niagara.input_set_dynamic"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FStackTarget Target;
            UNiagaraStackFunctionInput* Input = nullptr;
            if (!ResolveInput(Params, Target, Input, OutError)) return nullptr;

            FString ScriptPath;
            if (!RequireString(Params, TEXT("dynamic_input_script_path"), ScriptPath, OutError)) return nullptr;

            UNiagaraScript* DynamicInput = LoadObject<UNiagaraScript>(nullptr, *ScriptPath);
            if (!DynamicInput)
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                    FString::Printf(TEXT("Could not load NiagaraScript: %s"), *ScriptPath));
                return nullptr;
            }
            if (!DynamicInput->IsDynamicInputScript())
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                    FString::Printf(TEXT("%s is not a dynamic input script."), *ScriptPath));
                return nullptr;
            }

            FString SuggestedName;
            Params->TryGetStringField(TEXT("suggested_name"), SuggestedName);

            Input->SetDynamicInput(DynamicInput, SuggestedName);
            Target.System->MarkPackageDirty();
            return MakeOkResult();
        });

    // niagara.input_reset — drops a module input override back to its default.
    // Params: system_path, stack, emitter_name, module_name, input_name.
    Registry.Register(TEXT("niagara.input_reset"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FStackTarget Target;
            UNiagaraStackFunctionInput* Input = nullptr;
            if (!ResolveInput(Params, Target, Input, OutError)) return nullptr;

            if (!Input->CanReset())
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams, TEXT("This input is already at its default value."));
                return nullptr;
            }

            Input->Reset();
            Target.System->MarkPackageDirty();
            return MakeOkResult();
        });

    // niagara.renderer_list — an emitter's renderers, in draw order.
    // Params: system_path, emitter_name, include_properties (optional; dumps each renderer's
    // editable properties and their current values).
    Registry.Register(TEXT("niagara.renderer_list"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FEmitterTarget Target;
            if (!ResolveEmitter(Params, Target, OutError)) return nullptr;

            bool bIncludeProperties = false;
            Params->TryGetBoolField(TEXT("include_properties"), bIncludeProperties);

            TArray<UNiagaraStackRendererItem*> Renderers;
            CollectRenderers(*Target.Emitter, Renderers);

            TArray<TSharedPtr<FJsonValue>> Entries;
            for (int32 Index = 0; Index < Renderers.Num(); ++Index)
            {
                UNiagaraRendererProperties* Properties = Renderers[Index]->GetRendererProperties();

                TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
                Obj->SetNumberField(TEXT("index"), Index);
                Obj->SetStringField(TEXT("class"), Properties ? Properties->GetClass()->GetName() : FString());
                Obj->SetStringField(TEXT("display_name"), Renderers[Index]->GetDisplayName().ToString());
                Obj->SetBoolField(TEXT("enabled"), Renderers[Index]->GetIsEnabled());
                Obj->SetBoolField(TEXT("inherited"), Renderers[Index]->GetIsInherited());

                // Opt-in because a sprite renderer alone carries dozens of properties. Without
                // it renderer_set_property would be write-only — there would be no way to see
                // what a renderer exposes, or to confirm a write landed.
                if (bIncludeProperties && Properties)
                {
                    TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
                    for (TFieldIterator<FProperty> It(Properties->GetClass()); It; ++It)
                    {
                        FProperty* Property = *It;
                        if (!Property->HasAnyPropertyFlags(CPF_Edit))
                        {
                            continue;
                        }
                        FString Text;
                        Property->ExportText_InContainer(0, Text, Properties, Properties, Properties, PPF_None);
                        PropObj->SetStringField(Property->GetName(), Text);
                    }
                    Obj->SetObjectField(TEXT("properties"), PropObj);
                }

                Entries.Add(MakeShared<FJsonValueObject>(Obj));
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("renderers"), Entries);
            return Result;
        });

    // niagara.renderer_set_property — reflection write on a renderer's properties object.
    // Params: system_path, emitter_name, renderer_index, property, value (Unreal import text).
    Registry.Register(TEXT("niagara.renderer_set_property"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FEmitterTarget Target;
            if (!ResolveEmitter(Params, Target, OutError)) return nullptr;

            UNiagaraStackRendererItem* Item = FindRenderer(*Target.Emitter, Params, OutError);
            if (!Item) return nullptr;

            FString PropertyName;
            FString Value;
            if (!RequireString(Params, TEXT("property"), PropertyName, OutError)) return nullptr;
            if (!Params->TryGetStringField(TEXT("value"), Value))
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams, TEXT("value is required"));
                return nullptr;
            }

            UNiagaraRendererProperties* Properties = Item->GetRendererProperties();
            if (!Properties)
            {
                Fail(OutError, MCPProtocol::FMCPError::InternalError, TEXT("Renderer has no properties object."));
                return nullptr;
            }

            FProperty* Property = Properties->GetClass()->FindPropertyByName(FName(*PropertyName));
            if (!Property)
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                    FString::Printf(TEXT("Property '%s' not found on %s."), *PropertyName, *Properties->GetClass()->GetName()));
                return nullptr;
            }

            void* Address = Property->ContainerPtrToValuePtr<void>(Properties);
            Properties->Modify();
            if (Property->ImportText_Direct(*Value, Address, Properties, PPF_None) == nullptr)
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                    FString::Printf(TEXT("Could not read '%s' as a value for %s."), *Value, *PropertyName));
                return nullptr;
            }

            // Renderers cache derived state (bindings, material lists) that only rebuilds on
            // the property-change notification, so a raw write alone would not take effect.
            FPropertyChangedEvent ChangeEvent(Property);
            Properties->PostEditChangeProperty(ChangeEvent);
            Target.System->MarkPackageDirty();
            return MakeOkResult();
        });

    // niagara.renderer_set_enabled — toggles one renderer.
    // Params: system_path, emitter_name, renderer_index, enabled.
    Registry.Register(TEXT("niagara.renderer_set_enabled"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FEmitterTarget Target;
            if (!ResolveEmitter(Params, Target, OutError)) return nullptr;

            UNiagaraStackRendererItem* Item = FindRenderer(*Target.Emitter, Params, OutError);
            if (!Item) return nullptr;

            bool bEnabled = true;
            if (!Params->TryGetBoolField(TEXT("enabled"), bEnabled))
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams, TEXT("enabled is required and must be a boolean"));
                return nullptr;
            }

            Item->SetIsEnabled(bEnabled);
            Target.System->MarkPackageDirty();
            return MakeOkResult();
        });

    // niagara.emitter_add — adds an emitter asset to a system.
    // Params: system_path, emitter_asset_path, emitter_name (optional rename).
    Registry.Register(TEXT("niagara.emitter_add"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UNiagaraSystem* System = LoadSystem(Params, OutError);
            if (!System) return nullptr;

            FString EmitterPath;
            if (!RequireString(Params, TEXT("emitter_asset_path"), EmitterPath, OutError)) return nullptr;

            UNiagaraEmitter* Emitter = LoadObject<UNiagaraEmitter>(nullptr, *EmitterPath);
            if (!Emitter)
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams,
                    FString::Printf(TEXT("Could not load NiagaraEmitter: %s"), *EmitterPath));
                return nullptr;
            }

            TSharedPtr<FNiagaraSystemViewModel> ViewModel = MakeDataOnlyViewModel(*System);
            TSharedPtr<FNiagaraEmitterHandleViewModel> Handle =
                ViewModel->AddEmitter(*Emitter, Emitter->GetExposedVersion().VersionGuid);
            if (!Handle.IsValid())
            {
                Fail(OutError, MCPProtocol::FMCPError::InternalError,
                    FString::Printf(TEXT("Niagara refused to add %s to this system."), *EmitterPath));
                return nullptr;
            }

            FString NewName;
            if (Params->TryGetStringField(TEXT("emitter_name"), NewName) && !NewName.IsEmpty())
            {
                Handle->SetName(FName(*NewName));
            }
            System->MarkPackageDirty();

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("name"), Handle->GetName().ToString());
            Result->SetStringField(TEXT("id"), Handle->GetId().ToString());
            return Result;
        });

    // niagara.emitter_remove — removes an emitter from a system.
    // Params: system_path, emitter_name.
    Registry.Register(TEXT("niagara.emitter_remove"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FEmitterTarget Target;
            if (!ResolveEmitter(Params, Target, OutError)) return nullptr;

            Target.ViewModel->DeleteEmitters({ Target.Emitter->GetId() });
            Target.System->MarkPackageDirty();
            return MakeOkResult();
        });

    // niagara.compile — recompiles a system after stack edits. Params: system_path,
    // wait (optional, default true — blocks until the CPU scripts finish).
    Registry.Register(TEXT("niagara.compile"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UNiagaraSystem* System = LoadSystem(Params, OutError);
            if (!System) return nullptr;

            bool bWait = true;
            Params->TryGetBoolField(TEXT("wait"), bWait);

            const bool bCompiling = System->RequestCompile(false);
            if (bWait)
            {
                System->WaitForCompilationComplete(false, false);
            }
            System->MarkPackageDirty();

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("requested"), bCompiling);
            Result->SetBoolField(TEXT("waited"), bWait);
            return Result;
        });
}
