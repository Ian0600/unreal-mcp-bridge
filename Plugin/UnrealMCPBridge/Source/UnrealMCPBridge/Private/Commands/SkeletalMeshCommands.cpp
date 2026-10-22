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
#include "ScopedTransaction.h"

namespace
{
    using namespace UnrealMCPBridge::Json;

    /* 소켓 명령이 다루는 애셋. 스켈레탈 메시를 주면 메시 소켓과 그 스켈레톤 소켓을 둘 다 보고,
       스켈레톤을 주면 스켈레톤 소켓만 본다(2026-10-16 애니 팀 요청 — 촉수 SKEL_MLPlebaddon_Tentacle 처럼
       메시 없이 스켈레톤에 바로 다는 경우). */
    struct FSocketTarget
    {
        USkeletalMesh* Mesh = nullptr;     // 스켈레톤 애셋을 받았으면 null
        USkeleton* Skeleton = nullptr;

        const FReferenceSkeleton* RefSkeleton() const
        {
            return Mesh ? &Mesh->GetRefSkeleton() : (Skeleton ? &Skeleton->GetReferenceSkeleton() : nullptr);
        }
        FString Name() const
        {
            return Mesh ? Mesh->GetName() : (Skeleton ? Skeleton->GetName() : FString(TEXT("(none)")));
        }
    };

    // mesh_path(예전 인자) · skeleton_path · asset_path 중 하나. asset_path 는 메시든 스켈레톤이든 받는다.
    bool LoadSocketTarget(const TSharedPtr<FJsonObject>& Params, FSocketTarget& Out, MCPProtocol::FMCPError& OutError)
    {
        FString Path;
        const TCHAR* Field = nullptr;
        for (const TCHAR* Candidate : { TEXT("mesh_path"), TEXT("skeleton_path"), TEXT("asset_path") })
        {
            if (Params.IsValid() && Params->TryGetStringField(Candidate, Path) && !Path.IsEmpty())
            {
                Field = Candidate;
                break;
            }
        }
        if (!Field)
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = TEXT("mesh_path, skeleton_path or asset_path is required (a SkeletalMesh or a Skeleton)");
            return false;
        }

        UObject* Asset = LoadObject<UObject>(nullptr, *Path);
        if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset))
        {
            Out.Mesh = Mesh;
            Out.Skeleton = Mesh->GetSkeleton();
            return true;
        }
        if (USkeleton* Skeleton = Cast<USkeleton>(Asset))
        {
            Out.Skeleton = Skeleton;
            return true;
        }
        OutError.Code = MCPProtocol::FMCPError::InvalidParams;
        OutError.Message = Asset
            ? FString::Printf(TEXT("%s is a %s, not a SkeletalMesh or Skeleton"), *Path, *Asset->GetClass()->GetName())
            : FString::Printf(TEXT("Could not load %s"), *Path);
        return false;
    }

    int32 FindSocketIndex(const TArray<TObjectPtr<USkeletalMeshSocket>>& Sockets, FName Name)
    {
        return Sockets.IndexOfByPredicate(
            [Name](const TObjectPtr<USkeletalMeshSocket>& S) { return S && S->SocketName == Name; });
    }

    // 이름이 걸리는 소켓 — 메시가 먼저(런타임도 메시가 스켈레톤 소켓을 가린다), 없으면 스켈레톤.
    USkeletalMeshSocket* FindTargetSocket(const FSocketTarget& Target, FName Name)
    {
        if (Target.Mesh)
        {
            TArray<TObjectPtr<USkeletalMeshSocket>>& MeshSockets = Target.Mesh->GetMeshOnlySocketList();
            const int32 Index = FindSocketIndex(MeshSockets, Name);
            if (Index != INDEX_NONE) { return MeshSockets[Index]; }
        }
        if (Target.Skeleton)
        {
            const int32 Index = FindSocketIndex(Target.Skeleton->Sockets, Name);
            if (Index != INDEX_NONE) { return Target.Skeleton->Sockets[Index]; }
        }
        return nullptr;
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
    FRotator ReadRotator(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, const FRotator& Default = FRotator::ZeroRotator)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Params.IsValid() && Params->TryGetArrayField(Field, Arr) && Arr && Arr->Num() == 3)
        {
            return FRotator((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
        }
        return Default;
    }

    bool CheckBone(const FSocketTarget& Target, const FString& BoneName, MCPProtocol::FMCPError& OutError)
    {
        /* The bone has to exist, and this is the only place that can say so — attaching to a
           missing bone is not an error at runtime, the component just sits at the mesh origin
           and nobody can tell why the prop is on the floor. */
        const FReferenceSkeleton* Ref = Target.RefSkeleton();
        if (!Ref || Ref->FindBoneIndex(FName(*BoneName)) == INDEX_NONE)
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("bone '%s' does not exist on %s"), *BoneName, *Target.Name());
            return false;
        }
        return true;
    }

    /* 소켓 자리에 띄워 두는 **프리뷰 애셋**. 스켈레톤 에디터에서 잡는 각도를 눈으로 맞추는 도구이고
       게임에는 아무 영향이 없다.

       ★ 소켓이 들고 있지 않다 - UE5 에서는 **스켈레톤(또는 메시)의 PreviewAttachedAssetContainer**
         가 "어느 붙임점에 무엇을 띄울지" 를 이름으로 들고 있다. 소켓을 지웠다 다시 만들어도 그
         컨테이너는 남으므로, 같은 이름으로 다시 만들면 프리뷰가 따라온다.

       ★ 파이썬에서는 이 컨테이너도 소켓 프로퍼티도 열려 있지 않아 여기서 받는다. */
    void ApplyPreviewAsset(USkeleton* Skeleton, FName SocketName, const TSharedPtr<FJsonObject>& Params)
    {
#if WITH_EDITORONLY_DATA
        FString PreviewPath;
        if (!Params.IsValid() || !Params->TryGetStringField(TEXT("preview_mesh"), PreviewPath))
        {
            return;   // 안 주면 건드리지 않는다 - 이미 맞춰 둔 프리뷰를 지우지 않기 위해서다.
        }

        /* 스켈레톤 쪽에 담는다. 소켓 자체가 스켈레톤에 있으면 당연하고, 메시 소켓이더라도 프리뷰는
           스켈레톤 에디터에서 보는 것이라 그쪽이 자연스럽다. */
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

    // 이름을 바꾸면 프리뷰 애셋도 새 이름으로 옮긴다 — 컨테이너가 이름으로 묶여 있어 안 옮기면 프리뷰가 사라진다.
    void RenamePreviewAsset(USkeleton* Skeleton, FName OldName, FName NewName)
    {
#if WITH_EDITORONLY_DATA
        if (!Skeleton)
        {
            return;
        }
        if (UObject* Existing = Skeleton->PreviewAttachedAssetContainer.GetAttachedObjectByAttachName(OldName))
        {
            Skeleton->Modify();
            Skeleton->PreviewAttachedAssetContainer.RemoveAttachedObject(Existing, OldName);
            Skeleton->PreviewAttachedAssetContainer.AddUniqueAttachedObject(Existing, NewName);
            Skeleton->MarkPackageDirty();
        }
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
    // skeletalmesh.list_sockets — every socket the asset answers with, its own and its skeleton's.
    // Params: mesh_path | skeleton_path | asset_path. Returns { mesh_sockets: [...], skeleton_sockets: [...] }
    // (mesh_sockets is empty when a Skeleton was given).
    //
    // The two lists are kept apart because they behave differently: a skeleton socket is shared by
    // every mesh on that skeleton, a mesh socket is not. Which one a name resolves to at runtime is
    // decided by the mesh first, so a duplicate name silently shadows the skeleton's.
    Registry.Register(TEXT("skeletalmesh.list_sockets"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FSocketTarget Target;
            if (!LoadSocketTarget(Params, Target, OutError)) { return nullptr; }

            TArray<TSharedPtr<FJsonValue>> MeshOut;
            if (Target.Mesh)
            {
                for (const USkeletalMeshSocket* Socket : Target.Mesh->GetMeshOnlySocketList())
                {
                    if (Socket) { MeshOut.Add(MakeShared<FJsonValueObject>(MakeSocketJson(Socket))); }
                }
            }

            TArray<TSharedPtr<FJsonValue>> SkelOut;
            if (Target.Skeleton)
            {
                for (const USkeletalMeshSocket* Socket : Target.Skeleton->Sockets)
                {
                    if (Socket) { SkelOut.Add(MakeShared<FJsonValueObject>(MakeSocketJson(Socket))); }
                }
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("mesh_sockets"), MeshOut);
            Result->SetArrayField(TEXT("skeleton_sockets"), SkelOut);
            return Result;
        });

    // skeletalmesh.add_socket — creates an attachment socket. Params: mesh_path | skeleton_path |
    // asset_path, socket_name, bone_name, location [x,y,z], rotation [pitch,yaw,roll], scale [x,y,z],
    // to_skeleton (bool), replace (bool), preview_mesh (optional). Does not save — use asset.save.
    //
    // to_skeleton puts it on the USkeleton instead of this mesh, so every mesh sharing that skeleton
    // gets it — that is what you want for a character whose body is swapped (Rita's body and her
    // outfit meshes are separate assets on one skeleton). Default is the mesh, the narrower choice.
    // A Skeleton asset always takes it on the skeleton.
    Registry.Register(TEXT("skeletalmesh.add_socket"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FSocketTarget Target;
            if (!LoadSocketTarget(Params, Target, OutError)) { return nullptr; }

            FString SocketName, BoneName;
            if (!RequireString(Params, TEXT("socket_name"), SocketName, OutError)) { return nullptr; }
            if (!RequireString(Params, TEXT("bone_name"), BoneName, OutError))     { return nullptr; }

            USkeleton* Skeleton = Target.Skeleton;
            const bool bToSkeleton = !Target.Mesh
                || (Params->HasField(TEXT("to_skeleton")) && Params->GetBoolField(TEXT("to_skeleton")));
            if (bToSkeleton && !Skeleton)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("to_skeleton was requested but the mesh has no skeleton");
                return nullptr;
            }
            if (!CheckBone(Target, BoneName, OutError)) { return nullptr; }

            const FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("MCP: Add Socket %s"), *SocketName)));

            const bool bReplace = Params->HasField(TEXT("replace")) && Params->GetBoolField(TEXT("replace"));
            if (USkeletalMeshSocket* Existing = FindTargetSocket(Target, FName(*SocketName)))
            {
                if (!bReplace)
                {
                    OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                    OutError.Message = FString::Printf(
                        TEXT("socket '%s' already exists; pass replace=true to overwrite it, or use skeletalmesh.update_socket"), *SocketName);
                    return nullptr;
                }
                /* Overwrite in place rather than remove + re-add: anything already attached to this
                   socket keeps pointing at the same object, and the asset's socket order stays put. */
                Existing->SetFlags(RF_Transactional);
                Existing->Modify();
                Existing->BoneName         = FName(*BoneName);
                Existing->RelativeLocation = ReadVector(Params, TEXT("location"), FVector::ZeroVector);
                Existing->RelativeRotation = ReadRotator(Params, TEXT("rotation"));
                Existing->RelativeScale    = ReadVector(Params, TEXT("scale"), FVector::OneVector);
                ApplyPreviewAsset(Skeleton, FName(*SocketName), Params);

                UObject* Owner = Existing->GetOuter();
                if (Owner) { Owner->MarkPackageDirty(); }

                auto Result = MakeShared<FJsonObject>();
                Result->SetBoolField(TEXT("ok"), true);
                Result->SetBoolField(TEXT("replaced"), true);
                Result->SetStringField(TEXT("owner"), Owner ? Owner->GetName() : TEXT("(none)"));
                Result->SetField(TEXT("socket"), MakeShared<FJsonValueObject>(MakeSocketJson(Existing)));
                return Result;
            }

            UObject* Outer = bToSkeleton ? static_cast<UObject*>(Skeleton) : static_cast<UObject*>(Target.Mesh);
            Outer->Modify();
            USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(Outer, NAME_None, RF_Transactional);
            Socket->SocketName      = FName(*SocketName);
            Socket->BoneName        = FName(*BoneName);
            Socket->RelativeLocation = ReadVector(Params, TEXT("location"), FVector::ZeroVector);
            Socket->RelativeRotation = ReadRotator(Params, TEXT("rotation"));
            Socket->RelativeScale    = ReadVector(Params, TEXT("scale"), FVector::OneVector);
            ApplyPreviewAsset(Skeleton, FName(*SocketName), Params);

            if (bToSkeleton)
            {
                Skeleton->Sockets.Add(Socket);
                Skeleton->MarkPackageDirty();
            }
            else
            {
                Target.Mesh->AddSocket(Socket, /*bAddToSkeleton*/ false);
                Target.Mesh->MarkPackageDirty();
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            Result->SetBoolField(TEXT("replaced"), false);
            Result->SetStringField(TEXT("owner"), Outer->GetName());
            Result->SetField(TEXT("socket"), MakeShared<FJsonValueObject>(MakeSocketJson(Socket)));
            return Result;
        });

    // skeletalmesh.update_socket — changes only the fields given. Params: mesh_path | skeleton_path |
    // asset_path, socket_name, and any of new_name, bone_name, location, rotation, scale, preview_mesh.
    // Returns { ok, owner, changed: [...], socket }. Does not save — use asset.save.
    //
    // This is the rename Python cannot do: unreal.SkeletalMeshSocket.socket_name is read-only and
    // rename_socket looks the socket up by a name that a Python-created socket never got.
    Registry.Register(TEXT("skeletalmesh.update_socket"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FSocketTarget Target;
            if (!LoadSocketTarget(Params, Target, OutError)) { return nullptr; }

            FString SocketName;
            if (!RequireString(Params, TEXT("socket_name"), SocketName, OutError)) { return nullptr; }
            USkeletalMeshSocket* Socket = FindTargetSocket(Target, FName(*SocketName));
            if (!Socket)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("socket '%s' was not found on %s"), *SocketName, *Target.Name());
                return nullptr;
            }

            FString NewName, BoneName;
            const bool bRename = Params->TryGetStringField(TEXT("new_name"), NewName) && !NewName.IsEmpty() && NewName != SocketName;
            const bool bBone = Params->TryGetStringField(TEXT("bone_name"), BoneName) && !BoneName.IsEmpty();
            if (bRename && FindTargetSocket(Target, FName(*NewName)))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("a socket named '%s' already exists"), *NewName);
                return nullptr;
            }
            if (bBone && !CheckBone(Target, BoneName, OutError)) { return nullptr; }

            const FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("MCP: Update Socket %s"), *SocketName)));
            Socket->SetFlags(RF_Transactional);
            Socket->Modify();

            TArray<TSharedPtr<FJsonValue>> Changed;
            if (bRename)
            {
                Socket->SocketName = FName(*NewName);
                RenamePreviewAsset(Target.Skeleton, FName(*SocketName), FName(*NewName));
                Changed.Add(MakeShared<FJsonValueString>(TEXT("name")));
            }
            if (bBone)
            {
                Socket->BoneName = FName(*BoneName);
                Changed.Add(MakeShared<FJsonValueString>(TEXT("bone")));
            }
            if (Params->HasField(TEXT("location")))
            {
                Socket->RelativeLocation = ReadVector(Params, TEXT("location"), Socket->RelativeLocation);
                Changed.Add(MakeShared<FJsonValueString>(TEXT("location")));
            }
            if (Params->HasField(TEXT("rotation")))
            {
                Socket->RelativeRotation = ReadRotator(Params, TEXT("rotation"), Socket->RelativeRotation);
                Changed.Add(MakeShared<FJsonValueString>(TEXT("rotation")));
            }
            if (Params->HasField(TEXT("scale")))
            {
                Socket->RelativeScale = ReadVector(Params, TEXT("scale"), Socket->RelativeScale);
                Changed.Add(MakeShared<FJsonValueString>(TEXT("scale")));
            }
            if (Params->HasField(TEXT("preview_mesh")))
            {
                ApplyPreviewAsset(Target.Skeleton, Socket->SocketName, Params);
                Changed.Add(MakeShared<FJsonValueString>(TEXT("preview_mesh")));
            }

            UObject* Owner = Socket->GetOuter();
            if (Owner) { Owner->MarkPackageDirty(); }

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            Result->SetStringField(TEXT("owner"), Owner ? Owner->GetName() : TEXT("(none)"));
            Result->SetArrayField(TEXT("changed"), Changed);
            Result->SetField(TEXT("socket"), MakeShared<FJsonValueObject>(MakeSocketJson(Socket)));
            return Result;
        });

    // skeletalmesh.remove_socket — Params: mesh_path | skeleton_path | asset_path, socket_name.
    // Removes from whichever of the two lists holds it (mesh first). Returns { ok, removed_from }.
    Registry.Register(TEXT("skeletalmesh.remove_socket"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            FSocketTarget Target;
            if (!LoadSocketTarget(Params, Target, OutError)) { return nullptr; }

            FString SocketName;
            if (!RequireString(Params, TEXT("socket_name"), SocketName, OutError)) { return nullptr; }
            const FName Name(*SocketName);

            const FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("MCP: Remove Socket %s"), *SocketName)));

            FString RemovedFrom;
            if (Target.Mesh)
            {
                TArray<TObjectPtr<USkeletalMeshSocket>>& MeshSockets = Target.Mesh->GetMeshOnlySocketList();
                const int32 MeshIndex = FindSocketIndex(MeshSockets, Name);
                if (MeshIndex != INDEX_NONE)
                {
                    Target.Mesh->Modify();
                    MeshSockets.RemoveAt(MeshIndex);
                    Target.Mesh->MarkPackageDirty();
                    RemovedFrom = TEXT("mesh");
                }
            }
            if (RemovedFrom.IsEmpty() && Target.Skeleton)
            {
                const int32 SkelIndex = FindSocketIndex(Target.Skeleton->Sockets, Name);
                if (SkelIndex != INDEX_NONE)
                {
                    Target.Skeleton->Modify();
                    Target.Skeleton->Sockets.RemoveAt(SkelIndex);
                    Target.Skeleton->MarkPackageDirty();
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
