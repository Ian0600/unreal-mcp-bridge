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
