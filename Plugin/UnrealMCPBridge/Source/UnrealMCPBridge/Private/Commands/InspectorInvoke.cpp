#include "Commands/InspectorInvoke.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "JsonObjectConverter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace
{
    bool InspectorInvokeFail(MCPProtocol::FMCPError& OutError, int32 Code, const FString& Message)
    {
        OutError.Code = Code;
        OutError.Message = Message;
        return false;
    }

    // FJsonObjectConverter reads an FVector from {"X":..,"Y":..,"Z":..}; MCP clients send [x, y, z].
    bool InspectorInvokeReadVector(const TSharedPtr<FJsonValue>& Value, FProperty* Prop, void* Dest)
    {
        const FStructProperty* StructProp = CastField<FStructProperty>(Prop);
        if (!StructProp || StructProp->Struct != TBaseStructure<FVector>::Get() || Value->Type != EJson::Array)
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
        if (Arr.Num() != 3)
        {
            return false;
        }
        *static_cast<FVector*>(Dest) = FVector(Arr[0]->AsNumber(), Arr[1]->AsNumber(), Arr[2]->AsNumber());
        return true;
    }
}

FMCPCommandFunction MakeInspectorCommand(const TCHAR* ClassPath, const TCHAR* FunctionName, TArray<FString> ParamKeys)
{
    return [Path = FString(ClassPath), Name = FString(FunctionName), Keys = MoveTemp(ParamKeys)](
        const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
    {
        UClass* Class = FindObject<UClass>(nullptr, *Path);
        if (!Class)
        {
            InspectorInvokeFail(OutError, MCPProtocol::FMCPError::InternalError,
                FString::Printf(TEXT("BlueprintInspector plugin is not loaded (class %s not found)"), *Path));
            return nullptr;
        }
        UFunction* Function = Class->FindFunctionByName(*Name);
        if (!Function)
        {
            InspectorInvokeFail(OutError, MCPProtocol::FMCPError::InternalError,
                FString::Printf(TEXT("%s::%s not found (update the BlueprintInspector plugin)"), *Path, *Name));
            return nullptr;
        }

        FStructOnScope Frame(Function);
        uint8* Memory = Frame.GetStructMemory();
        FProperty* ReturnProp = Function->GetReturnProperty();

        int32 Index = 0;
        for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
        {
            FProperty* Prop = *It;
            if (Prop == ReturnProp)
            {
                continue;
            }
            if (!Keys.IsValidIndex(Index))
            {
                InspectorInvokeFail(OutError, MCPProtocol::FMCPError::InternalError,
                    FString::Printf(TEXT("%s::%s has more parameters than the command declares"), *Path, *Name));
                return nullptr;
            }
            const FString& Key = Keys[Index++];
            const TSharedPtr<FJsonValue> Value = Params.IsValid() ? Params->TryGetField(Key) : nullptr;
            if (!Value.IsValid() || Value->IsNull())
            {
                continue;
            }
            void* Dest = Prop->ContainerPtrToValuePtr<void>(Memory);
            if (!InspectorInvokeReadVector(Value, Prop, Dest) && !FJsonObjectConverter::JsonValueToUProperty(Value, Prop, Dest, 0, 0))
            {
                InspectorInvokeFail(OutError, MCPProtocol::FMCPError::InvalidParams,
                    FString::Printf(TEXT("parameter '%s' has the wrong type for %s"), *Key, *Name));
                return nullptr;
            }
        }

        Class->GetDefaultObject()->ProcessEvent(Function, Memory);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        if (!ReturnProp)
        {
            Result->SetBoolField(TEXT("ok"), true);
            return Result;
        }

        const void* Ret = ReturnProp->ContainerPtrToValuePtr<void>(Memory);
        TSharedPtr<FJsonValue> Out = FJsonObjectConverter::UPropertyToJsonValue(ReturnProp, Ret, 0, 0);
        if (const FStrProperty* StrProp = CastField<FStrProperty>(ReturnProp))
        {
            // Many BlueprintInspector functions return a JSON document as text; hand it back structured.
            const FString Text = StrProp->GetPropertyValue(Ret);
            const FString Trimmed = Text.TrimStart();
            if (Trimmed.StartsWith(TEXT("{")) || Trimmed.StartsWith(TEXT("[")))
            {
                TSharedPtr<FJsonValue> Parsed;
                TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
                if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
                {
                    Out = Parsed;
                }
            }
        }
        Result->SetField(TEXT("value"), Out.IsValid() ? Out : MakeShared<FJsonValueNull>());
        return Result;
    };
}
