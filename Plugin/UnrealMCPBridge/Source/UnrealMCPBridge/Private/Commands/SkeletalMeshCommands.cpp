#include "Commands/SkeletalMeshCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "Engine/SkeletalMesh.h"
#include "Animation/PreviewAssetAttachComponent.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/StaticMesh.h"
#include "ReferenceSkeleton.h"

namespace
{
    using namespace UnrealMCPBridge::Json;

    USkeletalMesh* LoadMesh(const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        FString Path;
        if (!RequireString(Params, TEXT("mesh_path"), Path, OutError))
        {
            return nullptr;
        }
        USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *Path);
        if (!Mesh)
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("Could not load skeletal mesh: %s"), *Path);
        }
        return Mesh;
    }

    // [x, y, z] — absent or malformed leaves the default, so a caller that only cares about the
    // bone can omit the transform entirely.
    FVector ReadVector(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, const FVector& Default)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Params.IsValid() && Params->TryGetArrayField(Field, Arr) && Arr && Arr->Num() == 3)
        {
            return FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
        }
        return Default;
    }

    // [pitch, yaw, roll] — same order the editor's details panel shows, not FRotator's constructor.
    FRotator ReadRotator(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Params.IsValid() && Params->TryGetArrayField(Field, Arr) && Arr && Arr->Num() == 3)
        {
            return FRotator((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
        }
        return FRotator::ZeroRotator;
    }

    /* 소켓 자리에 띄워 두는 **프리뷰 애셋**. 스켈레톤 에디터에서 잡는 각도를 눈으로 맞추는 도구이고
       게임에는 아무 영향이 없다.

       ★ 소켓이 들고 있지 않다 - UE5 에서는 **스켈레톤(또는 메시)의 PreviewAttachedAssetContainer**
         가 "어느 붙임점에 무엇을 띄울지" 를 이름으로 들고 있다. 소켓을 지웠다 다시 만들어도 그
         컨테이너는 남으므로, 같은 이름으로 다시 만들면 프리뷰가 따라온다.

       ★ 파이썬에서는 이 컨테이너도 소켓 프로퍼티도 열려 있지 않아 여기서 받는다. */
    void ApplyPreviewAsset(USkeletalMesh* Mesh, FName SocketName, const TSharedPtr<FJsonObject>& Params)
    {
#if WITH_EDITORONLY_DATA
        FString PreviewPath;
        if (!Params.IsValid() || !Params->TryGetStringField(TEXT("preview_mesh"), PreviewPath))
        {
            return;   // 안 주면 건드리지 않는다 - 이미 맞춰 둔 프리뷰를 지우지 않기 위해서다.
        }

        /* 스켈레톤 쪽에 담는다. 소켓 자체가 스켈레톤에 있으면 당연하고, 메시 소켓이더라도 프리뷰는
           스켈레톤 에디터에서 보는 것이라 그쪽이 자연스럽다. */
        USkeleton* Skeleton = Mesh ? Mesh->GetSkeleton() : nullptr;
        if (!Skeleton)
        {
            return;
        }

        Skeleton->Modify();
        if (UObject* Existing = Skeleton->PreviewAttachedAssetContainer.GetAttachedObjectByAttachName(SocketName))
        {
            Skeleton->PreviewAttachedAssetContainer.RemoveAttachedObject(Existing, SocketName);
        }
        if (!PreviewPath.IsEmpty())
        {
            if (UObject* Asset = LoadObject<UObject>(nullptr, *PreviewPath))
            {
                Skeleton->PreviewAttachedAssetContainer.AddUniqueAttachedObject(Asset, SocketName);
            }
        }
        Skeleton->MarkPackageDirty();
#endif
    }

    TSharedPtr<FJsonObject> MakeSocketJson(const USkeletalMeshSocket* Socket)
    {
        auto Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Socket->SocketName.ToString());
        Obj->SetStringField(TEXT("bone"), Socket->BoneName.ToString());
        Obj->SetField(TEXT("location"), MakeVector3JsonArray(Socket->RelativeLocation));

        TArray<TSharedPtr<FJsonValue>> Rot;
        Rot.Add(MakeShared<FJsonValueNumber>(Socket->RelativeRotation.Pitch));
        Rot.Add(MakeShared<FJsonValueNumber>(Socket->RelativeRotation.Yaw));
        Rot.Add(MakeShared<FJsonValueNumber>(Socket->RelativeRotation.Roll));
        Obj->SetArrayField(TEXT("rotation"), Rot);

        Obj->SetField(TEXT("scale"), MakeVector3JsonArray(Socket->RelativeScale));
        return Obj;
    }
}

void FSkeletalMeshCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    // skeletalmesh.list_sockets — every socket the mesh answers with, its own and its skeleton's.
    // Params: mesh_path. Returns { mesh_sockets: [...], skeleton_sockets: [...] }.
    //
    // The two lists are kept apart because they behave differently: a skeleton socket is shared by
    // every mesh on that skeleton, a mesh socket is not. Which one a name resolves to at runtime is
    // decided by the mesh first, so a duplicate name silently shadows the skeleton's.
    Registry.Register(TEXT("skeletalmesh.list_sockets"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            USkeletalMesh* Mesh = LoadMesh(Params, OutError); if (!Mesh) return nullptr;

            TArray<TSharedPtr<FJsonValue>> MeshOut;
            for (const USkeletalMeshSocket* Socket : Mesh->GetMeshOnlySocketList())
            {
                if (Socket) { MeshOut.Add(MakeShared<FJsonValueObject>(MakeSocketJson(Socket))); }
            }

            TArray<TSharedPtr<FJsonValue>> SkelOut;
            if (const USkeleton* Skeleton = Mesh->GetSkeleton())
            {
                for (const USkeletalMeshSocket* Socket : Skeleton->Sockets)
                {
                    if (Socket) { SkelOut.Add(MakeShared<FJsonValueObject>(MakeSocketJson(Socket))); }
                }
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("mesh_sockets"), MeshOut);
            Result->SetArrayField(TEXT("skeleton_sockets"), SkelOut);
            return Result;
        });

    // skeletalmesh.add_socket — creates an attachment socket. Params: mesh_path, socket_name,
    // bone_name, location [x,y,z], rotation [pitch,yaw,roll], scale [x,y,z], to_skeleton (bool),
    // replace (bool).
    //
    // to_skeleton puts it on the USkeleton instead of this mesh, so every mesh sharing that skeleton
    // gets it — that is what you want for a character whose body is swapped (Rita's body and her
    // outfit meshes are separate assets on one skeleton). Default is the mesh, the narrower choice.
    Registry.Register(TEXT("skeletalmesh.add_socket"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            USkeletalMesh* Mesh = LoadMesh(Params, OutError); if (!Mesh) return nullptr;

            FString SocketName, BoneName;
            if (!RequireString(Params, TEXT("socket_name"), SocketName, OutError)) { return nullptr; }
            if (!RequireString(Params, TEXT("bone_name"), BoneName, OutError))     { return nullptr; }

            USkeleton* Skeleton = Mesh->GetSkeleton();
            const bool bToSkeleton = Params->HasField(TEXT("to_skeleton")) && Params->GetBoolField(TEXT("to_skeleton"));
            if (bToSkeleton && !Skeleton)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("to_skeleton was requested but the mesh has no skeleton");
                return nullptr;
            }

            /* The bone has to exist, and this is the only place that can say so — attaching to a
               missing bone is not an error at runtime, the component just sits at the mesh origin
               and nobody can tell why the prop is on the floor. */
            if (Mesh->GetRefSkeleton().FindBoneIndex(FName(*BoneName)) == INDEX_NONE)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("bone '%s' does not exist on %s"), *BoneName, *Mesh->GetName());
                return nullptr;
            }

            const bool bReplace = Params->HasField(TEXT("replace")) && Params->GetBoolField(TEXT("replace"));
            if (USkeletalMeshSocket* Existing = Mesh->FindSocket(FName(*SocketName)))
            {
                if (!bReplace)
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = FString::Printf(
                        TEXT("socket '%s' already exists; pass replace=true to overwrite it"), *SocketName);
                    return nullptr;
                }
                /* Overwrite in place rather than remove + re-add: anything already attached to this
                   socket keeps pointing at the same object, and the asset's socket order stays put. */
                Existing->Modify();
                Existing->BoneName         = FName(*BoneName);
                Existing->RelativeLocation = ReadVector(Params, TEXT("location"), FVector::ZeroVector);
                Existing->RelativeRotation = ReadRotator(Params, TEXT("rotation"));
                Existing->RelativeScale    = ReadVector(Params, TEXT("scale"), FVector::OneVector);
                ApplyPreviewAsset(Mesh, FName(*SocketName), Params);

                UObject* Owner = Existing->GetOuter();
                if (Owner) { Owner->MarkPackageDirty(); }

                auto Result = MakeShared<FJsonObject>();
                Result->SetBoolField(TEXT("ok"), true);
                Result->SetBoolField(TEXT("replaced"), true);
                Result->SetStringField(TEXT("owner"), Owner ? Owner->GetName() : TEXT("(none)"));
                Result->SetField(TEXT("socket"), MakeShared<FJsonValueObject>(MakeSocketJson(Existing)));
                return Result;
            }

            UObject* Outer = bToSkeleton ? static_cast<UObject*>(Skeleton) : static_cast<UObject*>(Mesh);
            USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(Outer);
            Socket->SocketName      = FName(*SocketName);
            Socket->BoneName        = FName(*BoneName);
            Socket->RelativeLocation = ReadVector(Params, TEXT("location"), FVector::ZeroVector);
            Socket->RelativeRotation = ReadRotator(Params, TEXT("rotation"));
            Socket->RelativeScale    = ReadVector(Params, TEXT("scale"), FVector::OneVector);
            ApplyPreviewAsset(Mesh, FName(*SocketName), Params);

            if (bToSkeleton)
            {
                Skeleton->Modify();
                Skeleton->Sockets.Add(Socket);
                Skeleton->MarkPackageDirty();
            }
            else
            {
                Mesh->Modify();
                Mesh->AddSocket(Socket, /*bAddToSkeleton*/ false);
                Mesh->MarkPackageDirty();
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            Result->SetBoolField(TEXT("replaced"), false);
            Result->SetStringField(TEXT("owner"), Outer->GetName());
            Result->SetField(TEXT("socket"), MakeShared<FJsonValueObject>(MakeSocketJson(Socket)));
            return Result;
        });

    // skeletalmesh.remove_socket — Params: mesh_path, socket_name. Removes from whichever of the two
    // lists holds it. Returns { ok, removed_from }.
    Registry.Register(TEXT("skeletalmesh.remove_socket"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            USkeletalMesh* Mesh = LoadMesh(Params, OutError); if (!Mesh) return nullptr;

            FString SocketName;
            if (!RequireString(Params, TEXT("socket_name"), SocketName, OutError)) { return nullptr; }
            const FName Name(*SocketName);

            FString RemovedFrom;
            TArray<TObjectPtr<USkeletalMeshSocket>>& MeshSockets = Mesh->GetMeshOnlySocketList();
            const int32 MeshIndex = MeshSockets.IndexOfByPredicate(
                [Name](const TObjectPtr<USkeletalMeshSocket>& S) { return S && S->SocketName == Name; });
            if (MeshIndex != INDEX_NONE)
            {
                Mesh->Modify();
                MeshSockets.RemoveAt(MeshIndex);
                Mesh->MarkPackageDirty();
                RemovedFrom = TEXT("mesh");
            }
            else if (USkeleton* Skeleton = Mesh->GetSkeleton())
            {
                const int32 SkelIndex = Skeleton->Sockets.IndexOfByPredicate(
                    [Name](const TObjectPtr<USkeletalMeshSocket>& S) { return S && S->SocketName == Name; });
                if (SkelIndex != INDEX_NONE)
                {
                    Skeleton->Modify();
                    Skeleton->Sockets.RemoveAt(SkelIndex);
                    Skeleton->MarkPackageDirty();
                    RemovedFrom = TEXT("skeleton");
                }
            }

            if (RemovedFrom.IsEmpty())
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("socket '%s' was not found"), *SocketName);
                return nullptr;
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            Result->SetStringField(TEXT("removed_from"), RemovedFrom);
            return Result;
        });
}
