#include "Commands/PhysicsAssetCommands.h"
#include "Commands/CommandJsonHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "Engine/SkeletalMesh.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "ReferenceSkeleton.h"

namespace
{
    using namespace UnrealMCPBridge::Json;

    /* ★ 이름 앞에 Phys 를 붙인 이유: 유니티 빌드가 Commands/*.cpp 를 한 번역 단위로 합치므로,
       익명 네임스페이스에 두어도 다른 커맨드 파일의 같은 이름(SkeletalMeshCommands.cpp 의
       ReadVector/ReadRotator 등)과 재정의로 충돌한다. AnimNotify_EffectActor 의 FMessageLog 채널이
       같은 이유로 파일 스코프 상수를 못 쓰는 것과 같은 사정이다. */

    UPhysicsAsset* PhysLoadAsset(const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        FString Path;
        if (!RequireString(Params, TEXT("asset_path"), Path, OutError))
        {
            return nullptr;
        }
        UPhysicsAsset* Asset = LoadObject<UPhysicsAsset>(nullptr, *Path);
        if (!Asset)
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("Could not load physics asset: %s"), *Path);
        }
        return Asset;
    }

    // [x, y, z] — 없거나 형식이 틀리면 기본값. 도형만 바꾸고 위치는 그대로 두고 싶은 호출을 위해서다.
    FVector PhysReadVector(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, const FVector& Default)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Params.IsValid() && Params->TryGetArrayField(Field, Arr) && Arr && Arr->Num() == 3)
        {
            return FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
        }
        return Default;
    }

    // [pitch, yaw, roll] — 디테일 패널이 보여 주는 순서다(FRotator 생성자 순서가 아니다).
    FRotator PhysReadRotator(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Params.IsValid() && Params->TryGetArrayField(Field, Arr) && Arr && Arr->Num() == 3)
        {
            return FRotator((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
        }
        return FRotator::ZeroRotator;
    }

    double PhysReadNumber(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, double Default)
    {
        double Out = Default;
        return (Params.IsValid() && Params->TryGetNumberField(Field, Out)) ? Out : Default;
    }

    bool PhysReadBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, bool bDefault)
    {
        bool bOut = bDefault;
        return (Params.IsValid() && Params->TryGetBoolField(Field, bOut)) ? bOut : bDefault;
    }

    const TCHAR* PhysTypeToString(TEnumAsByte<EPhysicsType> Type)
    {
        switch (Type.GetValue())
        {
        case PhysType_Kinematic: return TEXT("kinematic");
        case PhysType_Simulated: return TEXT("simulated");
        default:                 return TEXT("default");
        }
    }

    /* 문자열 -> 저작 타입. 못 알아보면 false 를 내고 호출부가 에러를 만든다 - 오타를 조용히
       `Default` 로 떨어뜨리면 "왜 안 흔들리지"로 시간을 버리게 된다. */
    bool PhysTypeFromString(const FString& In, TEnumAsByte<EPhysicsType>& Out)
    {
        if (In.Equals(TEXT("kinematic"), ESearchCase::IgnoreCase)) { Out = PhysType_Kinematic; return true; }
        if (In.Equals(TEXT("simulated"), ESearchCase::IgnoreCase)) { Out = PhysType_Simulated; return true; }
        if (In.Equals(TEXT("default"),   ESearchCase::IgnoreCase)) { Out = PhysType_Default;   return true; }
        return false;
    }

    /* 바디 하나를 JSON 으로. 도형 파라미터까지 실어 보내는 것이 요점이다 - 다른 PA 에서 값을 읽어
       그대로 옮기는 것이 이 커맨드의 주 용도이고, 그러려면 읽는 쪽이 완전해야 한다. */
    TSharedPtr<FJsonObject> MakePhysBodyJson(const USkeletalBodySetup* Body, int32 Index)
    {
        auto Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("index"), Index);
        Obj->SetStringField(TEXT("bone_name"), Body->BoneName.ToString());
        Obj->SetStringField(TEXT("physics_type"), PhysTypeToString(Body->PhysicsType));
        Obj->SetBoolField(TEXT("consider_for_bounds"), Body->bConsiderForBounds != 0);

        TArray<TSharedPtr<FJsonValue>> Shapes;
        for (const FKSphereElem& E : Body->AggGeom.SphereElems)
        {
            auto S = MakeShared<FJsonObject>();
            S->SetStringField(TEXT("shape"), TEXT("sphere"));
            S->SetNumberField(TEXT("radius"), E.Radius);
            S->SetField(TEXT("center"), MakeVector3JsonArray(E.Center));
            Shapes.Add(MakeShared<FJsonValueObject>(S));
        }
        for (const FKSphylElem& E : Body->AggGeom.SphylElems)
        {
            auto S = MakeShared<FJsonObject>();
            S->SetStringField(TEXT("shape"), TEXT("capsule"));
            S->SetNumberField(TEXT("radius"), E.Radius);
            S->SetNumberField(TEXT("length"), E.Length);
            S->SetField(TEXT("center"), MakeVector3JsonArray(E.Center));
            TArray<TSharedPtr<FJsonValue>> Rot;
            Rot.Add(MakeShared<FJsonValueNumber>(E.Rotation.Pitch));
            Rot.Add(MakeShared<FJsonValueNumber>(E.Rotation.Yaw));
            Rot.Add(MakeShared<FJsonValueNumber>(E.Rotation.Roll));
            S->SetArrayField(TEXT("rotation"), Rot);
            Shapes.Add(MakeShared<FJsonValueObject>(S));
        }
        for (const FKBoxElem& E : Body->AggGeom.BoxElems)
        {
            auto S = MakeShared<FJsonObject>();
            S->SetStringField(TEXT("shape"), TEXT("box"));
            S->SetField(TEXT("extents"), MakeVector3JsonArray(FVector(E.X, E.Y, E.Z)));
            S->SetField(TEXT("center"), MakeVector3JsonArray(E.Center));
            TArray<TSharedPtr<FJsonValue>> Rot;
            Rot.Add(MakeShared<FJsonValueNumber>(E.Rotation.Pitch));
            Rot.Add(MakeShared<FJsonValueNumber>(E.Rotation.Yaw));
            Rot.Add(MakeShared<FJsonValueNumber>(E.Rotation.Roll));
            S->SetArrayField(TEXT("rotation"), Rot);
            Shapes.Add(MakeShared<FJsonValueObject>(S));
        }
        Obj->SetNumberField(TEXT("convex_count"), Body->AggGeom.ConvexElems.Num());
        Obj->SetArrayField(TEXT("shapes"), Shapes);
        return Obj;
    }

    USkeletalBodySetup* FindPhysBody(UPhysicsAsset* Asset, FName BoneName, int32& OutIndex)
    {
        for (int32 i = 0; i < Asset->SkeletalBodySetups.Num(); ++i)
        {
            USkeletalBodySetup* Body = Asset->SkeletalBodySetups[i];
            if (Body && Body->BoneName == BoneName)
            {
                OutIndex = i;
                return Body;
            }
        }
        OutIndex = INDEX_NONE;
        return nullptr;
    }

    /* PA 는 프리뷰 메시로만 스켈레톤을 안다. 안 걸려 있으면 본 검증을 건너뛰고 결과에 그 사실을
       실어 보낸다 - **없는 본에 바디를 만들면 에러 없이 원점에 놓인다.** 조용히 틀리는 종류라
       검증했는지 여부까지 알려 줘야 한다. */
    bool ValidateBoneOnPreview(UPhysicsAsset* Asset, FName BoneName, bool& bOutChecked)
    {
        bOutChecked = false;
#if WITH_EDITORONLY_DATA
        if (USkeletalMesh* Preview = Asset->PreviewSkeletalMesh.LoadSynchronous())
        {
            bOutChecked = true;
            return Preview->GetRefSkeleton().FindBoneIndex(BoneName) != INDEX_NONE;
        }
#endif
        return true;
    }
}

void FPhysicsAssetCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    /* physicsasset.list_bodies — Params: asset_path.
       Returns { asset, body_count, bodies:[{index, bone_name, physics_type, shapes:[...]}] }.

       파이썬으로도 바디 목록은 겨우 읽을 수 있지만(서브오브젝트 이름을 0..N 으로 훑는 우회),
       번호가 연속이 아니라 범위를 넉넉히 돌아야 하고 도형 파라미터를 꺼내는 것이 번거롭다.
       옮겨 심을 값을 읽는 것이 이 커맨드 짝의 절반이라 여기서 완전하게 낸다. */
    Registry.Register(TEXT("physicsasset.list_bodies"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UPhysicsAsset* Asset = PhysLoadAsset(Params, OutError);
            if (!Asset) { return nullptr; }

            TArray<TSharedPtr<FJsonValue>> Bodies;
            for (int32 i = 0; i < Asset->SkeletalBodySetups.Num(); ++i)
            {
                if (const USkeletalBodySetup* Body = Asset->SkeletalBodySetups[i])
                {
                    Bodies.Add(MakeShared<FJsonValueObject>(MakePhysBodyJson(Body, i)));
                }
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("asset"), Asset->GetPathName());
            Result->SetNumberField(TEXT("body_count"), Bodies.Num());
            Result->SetNumberField(TEXT("constraint_count"), Asset->ConstraintSetup.Num());
            Result->SetArrayField(TEXT("bodies"), Bodies);
            return Result;
        });

    /* physicsasset.add_body — Params:
         asset_path            (필수)
         bone_name             (필수)
         shape                 "capsule" | "sphere" | "box"   (기본 capsule)
         radius, length        capsule/sphere 용
         extents [x,y,z]       box 용 (반이 아니라 전체 크기 - FKBoxElem 의 X/Y/Z 와 같다)
         center  [x,y,z]       본 로컬 오프셋
         rotation [p,y,r]      본 로컬 회전
         physics_type          "kinematic" | "simulated" | "default"   (기본 kinematic)
         consider_for_bounds   기본 false
         mode                  "create" | "replace" | "append"          (기본 create)
       Returns { ok, bone_name, body_index, physics_type, shape_count, bone_validated }.

       **기본이 kinematic 인 이유**: 이 커맨드로 넣는 것은 대개 콜라이더다(클로스가 부딪힐 몸,
       래그돌이 뚫지 말아야 할 벽). `Default` 로 두면 컴포넌트가 `SetSimulatePhysics(true)` 를
       부르는 순간 같이 넘어가 의도치 않게 흔들린다.

       **컨스트레인트를 안 만든다**는 점을 잊지 말 것 - `simulated` 로 넣으면 부모에 물리지 않은
       자유 바디가 되어 그 자리에서 떨어져 나간다. 시뮬레이션할 바디는 에디터에서 조인트까지 잡아야
       한다(헤더 주석). */
    Registry.Register(TEXT("physicsasset.add_body"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UPhysicsAsset* Asset = PhysLoadAsset(Params, OutError);
            if (!Asset) { return nullptr; }

            FString BoneNameStr;
            if (!RequireString(Params, TEXT("bone_name"), BoneNameStr, OutError)) { return nullptr; }
            const FName BoneName(*BoneNameStr);

            bool bBoneChecked = false;
            if (!ValidateBoneOnPreview(Asset, BoneName, bBoneChecked))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(
                    TEXT("bone '%s' does not exist on %s's preview mesh"), *BoneNameStr, *Asset->GetName());
                return nullptr;
            }

            TEnumAsByte<EPhysicsType> BodyType = PhysType_Kinematic;
            FString TypeStr;
            if (Params->TryGetStringField(TEXT("physics_type"), TypeStr) && !PhysTypeFromString(TypeStr, BodyType))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(
                    TEXT("physics_type '%s' is not one of default/kinematic/simulated"), *TypeStr);
                return nullptr;
            }

            FString Mode = TEXT("create");
            Params->TryGetStringField(TEXT("mode"), Mode);

            int32 ExistingIndex = INDEX_NONE;
            USkeletalBodySetup* Body = FindPhysBody(Asset, BoneName, ExistingIndex);
            if (Body && Mode.Equals(TEXT("create"), ESearchCase::IgnoreCase))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(
                    TEXT("a body for bone '%s' already exists (index %d); pass mode=replace or mode=append"),
                    *BoneNameStr, ExistingIndex);
                return nullptr;
            }

            Asset->Modify();

            const bool bCreated = (Body == nullptr);
            if (bCreated)
            {
                /* RF_Transactional 이 있어야 에디터의 실행 취소가 이 바디를 되돌린다. 없으면 Ctrl+Z 가
                   PA 는 되돌리는데 바디만 남아 목록이 어긋난다. */
                Body = NewObject<USkeletalBodySetup>(Asset, NAME_None, RF_Transactional);
                Body->BoneName = BoneName;
                ExistingIndex = Asset->SkeletalBodySetups.Add(Body);
            }
            else
            {
                Body->Modify();
                if (Mode.Equals(TEXT("replace"), ESearchCase::IgnoreCase))
                {
                    Body->AggGeom.EmptyElements();
                }
            }

            Body->PhysicsType = BodyType;
            Body->bConsiderForBounds = PhysReadBool(Params, TEXT("consider_for_bounds"), false) ? 1 : 0;

            const FVector  Center   = PhysReadVector(Params, TEXT("center"), FVector::ZeroVector);
            const FRotator Rotation = PhysReadRotator(Params, TEXT("rotation"));

            FString Shape = TEXT("capsule");
            Params->TryGetStringField(TEXT("shape"), Shape);

            if (Shape.Equals(TEXT("sphere"), ESearchCase::IgnoreCase))
            {
                FKSphereElem Elem;
                Elem.Center = Center;
                Elem.Radius = static_cast<float>(PhysReadNumber(Params, TEXT("radius"), 5.0));
                Body->AggGeom.SphereElems.Add(Elem);
            }
            else if (Shape.Equals(TEXT("box"), ESearchCase::IgnoreCase))
            {
                const FVector Extents = PhysReadVector(Params, TEXT("extents"), FVector(10.0, 10.0, 10.0));
                FKBoxElem Elem;
                Elem.Center   = Center;
                Elem.Rotation = Rotation;
                Elem.X = static_cast<float>(Extents.X);
                Elem.Y = static_cast<float>(Extents.Y);
                Elem.Z = static_cast<float>(Extents.Z);
                Body->AggGeom.BoxElems.Add(Elem);
            }
            else if (Shape.Equals(TEXT("capsule"), ESearchCase::IgnoreCase))
            {
                FKSphylElem Elem;
                Elem.Center   = Center;
                Elem.Rotation = Rotation;
                Elem.Radius   = static_cast<float>(PhysReadNumber(Params, TEXT("radius"), 5.0));
                /* Length 는 **양 끝 반구를 뺀 직선 구간**이다(FKSphylElem 주석). 디테일 패널이 보여
                   주는 값과 같으므로 다른 PA 에서 읽은 값을 그대로 넣으면 된다. */
                Elem.Length   = static_cast<float>(PhysReadNumber(Params, TEXT("length"), 10.0));
                Body->AggGeom.SphylElems.Add(Elem);
            }
            else
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(
                    TEXT("shape '%s' is not one of capsule/sphere/box"), *Shape);
                return nullptr;
            }

            /* ★ 이 둘을 안 부르면 조용히 어긋난다. 인덱스 맵은 본 이름으로 바디를 찾는 경로가
               전부 쓰고(런타임 포함), 바운즈 배열을 갱신 안 하면 새 바디가 컬링 계산에서 빠진다. */
            Asset->UpdateBodySetupIndexMap();
            Asset->UpdateBoundsBodiesArray();
            Asset->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            Result->SetBoolField(TEXT("created"), bCreated);
            Result->SetStringField(TEXT("bone_name"), BoneNameStr);
            Result->SetNumberField(TEXT("body_index"), ExistingIndex);
            Result->SetStringField(TEXT("physics_type"), PhysTypeToString(Body->PhysicsType));
            Result->SetNumberField(TEXT("shape_count"), Body->AggGeom.GetElementCount());
            Result->SetBoolField(TEXT("bone_validated"), bBoneChecked);
            Result->SetField(TEXT("body"), MakeShared<FJsonValueObject>(MakePhysBodyJson(Body, ExistingIndex)));
            return Result;
        });

    /* physicsasset.remove_body — Params: asset_path, bone_name. Returns { ok, removed_index }.

       **그 바디를 물고 있는 컨스트레인트도 같이 지운다.** 남겨 두면 없는 바디를 가리키는 조인트가
       되어 PA 를 열 때 경고가 뜨고, 래그돌에서 그 관절이 조용히 빠진다. */
    Registry.Register(TEXT("physicsasset.remove_body"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UPhysicsAsset* Asset = PhysLoadAsset(Params, OutError);
            if (!Asset) { return nullptr; }

            FString BoneNameStr;
            if (!RequireString(Params, TEXT("bone_name"), BoneNameStr, OutError)) { return nullptr; }
            const FName BoneName(*BoneNameStr);

            int32 Index = INDEX_NONE;
            if (!FindPhysBody(Asset, BoneName, Index))
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = FString::Printf(TEXT("no body for bone '%s' in %s"),
                    *BoneNameStr, *Asset->GetName());
                return nullptr;
            }

            Asset->Modify();

            int32 RemovedConstraints = 0;
            for (int32 i = Asset->ConstraintSetup.Num() - 1; i >= 0; --i)
            {
                const UPhysicsConstraintTemplate* Setup = Asset->ConstraintSetup[i];
                if (!Setup) { continue; }
                const FConstraintInstance& CI = Setup->DefaultInstance;
                if (CI.ConstraintBone1 == BoneName || CI.ConstraintBone2 == BoneName)
                {
                    Asset->ConstraintSetup.RemoveAt(i);
                    ++RemovedConstraints;
                }
            }

            Asset->SkeletalBodySetups.RemoveAt(Index);
            Asset->UpdateBodySetupIndexMap();
            Asset->UpdateBoundsBodiesArray();
            Asset->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            Result->SetStringField(TEXT("bone_name"), BoneNameStr);
            Result->SetNumberField(TEXT("removed_index"), Index);
            Result->SetNumberField(TEXT("removed_constraints"), RemovedConstraints);
            Result->SetNumberField(TEXT("body_count"), Asset->SkeletalBodySetups.Num());
            return Result;
        });
}
