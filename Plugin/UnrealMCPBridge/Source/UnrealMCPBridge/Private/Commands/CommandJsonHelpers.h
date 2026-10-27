#pragma once

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "AssetRegistry/AssetData.h"
#include "MCPProtocol.h"

// Forward declaration — FBPVariableDescription is a Kismet struct, include it where needed.
struct FBPVariableDescription;

namespace UnrealMCPBridge::Json
{
    /**
     * Reads a required non-empty string field, filling OutError and returning false if it is
     * missing, empty, or the params object itself is null.
     *
     * Lives here rather than in each command file's anonymous namespace: every handler needs the
     * same check, and UBT's unity build merges those files into one translation unit, where five
     * identical definitions become a redefinition error. Renaming per file (MtlRequireString,
     * RigRequireString, ...) dodged that twice; one shared definition removes the hazard instead.
     */
    inline bool RequireString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FString& Out, MCPProtocol::FMCPError& OutError)
    {
        if (!Params.IsValid() || !Params->TryGetStringField(Field, Out) || Out.IsEmpty())
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("%s is required and must be non-empty"), Field);
            return false;
        }
        return true;
    }

    /**
     * Reads an optional [x, y, z] number array. Leaves Out untouched when the field is absent;
     * returns false (with OutError) only when it is present but malformed.
     */
    inline bool ReadVector3(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FVector& Out, MCPProtocol::FMCPError& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!Params.IsValid() || !Params->TryGetArrayField(Field, Arr) || !Arr)
        {
            return true;
        }

        if (Arr->Num() != 3)
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("%s must be a 3-element number array"), Field);
            return false;
        }

        Out.X = (*Arr)[0]->AsNumber();
        Out.Y = (*Arr)[1]->AsNumber();
        Out.Z = (*Arr)[2]->AsNumber();
        return true;
    }

    /** Reads an optional [pitch, yaw, roll] array — Unreal's FRotator ordering, as the protocol documents. */
    inline bool ReadRotator(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FRotator& Out, MCPProtocol::FMCPError& OutError)
    {
        FVector AsVector(Out.Pitch, Out.Yaw, Out.Roll);
        if (!ReadVector3(Params, Field, AsVector, OutError))
        {
            return false;
        }
        Out.Pitch = AsVector.X;
        Out.Yaw   = AsVector.Y;
        Out.Roll  = AsVector.Z;
        return true;
    }

    /** [pitch, yaw, roll] JSON array, the inverse of ReadRotator. */
    inline TSharedPtr<FJsonValue> MakeRotatorJsonArray(const FRotator& R)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(R.Pitch));
        Arr.Add(MakeShared<FJsonValueNumber>(R.Yaw));
        Arr.Add(MakeShared<FJsonValueNumber>(R.Roll));
        return MakeShared<FJsonValueArray>(Arr);
    }

    /**
     * Builds a JSON object representing a single actor.
     * Fields: "name" (string), "class" (string), "location" ([x,y,z] number array).
     */
    inline TSharedPtr<FJsonObject> MakeActorJson(AActor* Actor)
    {
        check(Actor);
        const FVector Loc = Actor->GetActorLocation();

        TArray<TSharedPtr<FJsonValue>> LocArray;
        LocArray.Add(MakeShared<FJsonValueNumber>(Loc.X));
        LocArray.Add(MakeShared<FJsonValueNumber>(Loc.Y));
        LocArray.Add(MakeShared<FJsonValueNumber>(Loc.Z));

        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"),     Actor->GetName());
        Obj->SetStringField(TEXT("class"),    Actor->GetClass()->GetName());
        Obj->SetArrayField(TEXT("location"),  LocArray);
        return Obj;
    }

    /**
     * Builds a [x, y, z] JSON array from an FVector.
     */
    inline TSharedPtr<FJsonValue> MakeVector3JsonArray(const FVector& V)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(V.X));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
        return MakeShared<FJsonValueArray>(Arr);
    }

    /**
     * Builds a JSON object representing a single asset.
     * Fields: "name" (string), "class" (string), "object_path" (string).
     */
    inline TSharedPtr<FJsonObject> MakeAssetJson(const FAssetData& AssetData)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"),        AssetData.AssetName.ToString());
        Obj->SetStringField(TEXT("class"),        AssetData.AssetClassPath.GetAssetName().ToString());
        Obj->SetStringField(TEXT("object_path"),  AssetData.GetObjectPathString());
        return Obj;
    }

    /**
     * Builds a JSON object representing a single Blueprint variable.
     * Fields: "name" (string), "type" (string), "category" (string).
     * TypeStr must be pre-resolved by the caller from FEdGraphPinType.
     */
    inline TSharedPtr<FJsonObject> MakeVariableJson(const FString& Name, const FString& TypeStr, const FString& Category)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"),     Name);
        Obj->SetStringField(TEXT("type"),     TypeStr);
        Obj->SetStringField(TEXT("category"), Category);
        return Obj;
    }

} // namespace UnrealMCPBridge::Json
