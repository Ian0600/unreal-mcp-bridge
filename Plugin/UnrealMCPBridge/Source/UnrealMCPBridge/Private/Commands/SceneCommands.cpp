#include "Commands/SceneCommands.h"
#include "Commands/SceneTools.h"
#include "Commands/CommandJsonHelpers.h"
#include "Commands/CommandReflectionHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "MCPSettings.h"
#include "UnrealMCPBridgeModule.h"
#include "Dom/JsonObject.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "EngineUtils.h"
#include "CollisionQueryParams.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Volume.h"
#include "Components/BrushComponent.h"
#include "Components/ShapeComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/LightComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Animation/SkeletalMeshActor.h"
#include "Engine/Light.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Engine/RectLight.h"
#include "Engine/DirectionalLight.h"
#include "Camera/CameraActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "Animation/AnimationAsset.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/CameraTypes.h"
#include "SceneView.h"
#include "ImageUtils.h"
#include "ImageCore.h"
#include "ContentStreaming.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Containers/Ticker.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/StrongObjectPtr.h"

// ---------------------------------------------------------------------------------------------
// Shared with the level data commands (SceneTools.h)
// ---------------------------------------------------------------------------------------------

namespace UnrealMCPBridge::Scene
{
    const FName TempActorTag(TEXT("MCPTemp"));

    UWorld* GetEditorWorld(MCPProtocol::FMCPError& OutError)
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World || !World->PersistentLevel)
        {
            OutError.Code = MCPProtocol::FMCPError::InternalError;
            OutError.Message = TEXT("Editor world is not available");
            return nullptr;
        }
        return World;
    }

    UWorld* ResolveWorld(const TSharedPtr<FJsonObject>& Params, EDefaultWorld Default, bool& bOutIsPie, MCPProtocol::FMCPError& OutError)
    {
        FString Which;
        if (Params.IsValid())
        {
            Params->TryGetStringField(TEXT("world"), Which);
        }
        UWorld* PlayWorld = GEditor ? GEditor->PlayWorld.Get() : nullptr;
        if (Which.IsEmpty())
        {
            bOutIsPie = Default == EDefaultWorld::PieIfRunning && PlayWorld != nullptr;
        }
        else if (Which.Equals(TEXT("pie"), ESearchCase::IgnoreCase))
        {
            bOutIsPie = true;
        }
        else if (Which.Equals(TEXT("editor"), ESearchCase::IgnoreCase))
        {
            bOutIsPie = false;
        }
        else
        {
            OutError.Code = MCPProtocol::FMCPError::InvalidParams;
            OutError.Message = FString::Printf(TEXT("world must be \"pie\" or \"editor\", not \"%s\""), *Which);
            return nullptr;
        }
        if (bOutIsPie)
        {
            if (!PlayWorld)
            {
                OutError.Code = MCPProtocol::FMCPError::InvalidParams;
                OutError.Message = TEXT("world=pie but PIE is not running (pie.start first)");
            }
            return PlayWorld;
        }
        return GetEditorWorld(OutError);
    }

    FDirtySnapshot SnapshotDirtyPackages()
    {
        FDirtySnapshot Snapshot;
        for (TObjectIterator<UPackage> It; It; ++It)
        {
            if (It->IsDirty())
            {
                Snapshot.DirtyPackages.Add(It->GetFName());
            }
        }
        return Snapshot;
    }

    void WriteDirtyReport(UWorld* World, const FDirtySnapshot& Before, const TSharedPtr<FJsonObject>& Result)
    {
        const UPackage* Transient = GetTransientPackage();
        TArray<TSharedPtr<FJsonValue>> Dirtied;
        for (TObjectIterator<UPackage> It; It; ++It)
        {
            if (*It != Transient && It->IsDirty() && !Before.DirtyPackages.Contains(It->GetFName()))
            {
                Dirtied.Add(MakeShared<FJsonValueString>(It->GetName()));
            }
        }
        Result->SetBoolField(TEXT("level_dirty"), Dirtied.Num() > 0);
        Result->SetArrayField(TEXT("dirtied_packages"), Dirtied);

        const UPackage* MapPackage = (World && World->PersistentLevel) ? World->PersistentLevel->GetPackage() : nullptr;
        Result->SetBoolField(TEXT("map_dirty"), MapPackage && MapPackage->IsDirty());
    }

    static bool IsVolumeLikeHit(const AActor* Actor, const UPrimitiveComponent* Component)
    {
        return (Actor && Actor->IsA<AVolume>())
            || (Component && (Component->IsA<UBrushComponent>() || Component->IsA<UShapeComponent>()));
    }

    void TraceGroundLayers(UWorld* World, double X, double Y, const FGroundQuery& Query, TArray<FGroundLayer>& OutLayers)
    {
        OutLayers.Reset();
        if (!World)
        {
            return;
        }

        const FVector Start(X, Y, Query.TopZ);
        const FVector End(X, Y, Query.BottomZ);
        FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(MCPGroundTrace), /*bInTraceComplex=*/true);
        for (const TWeakObjectPtr<AActor>& Ignored : Query.IgnoreActors)
        {
            if (Ignored.IsValid())
            {
                QueryParams.AddIgnoredActor(Ignored.Get());
            }
        }

        // Each hit component is ignored for the next trace, so the ray walks down through
        // roof, floor and terrain one component at a time. The guard counts skipped volumes too.
        for (int32 Guard = 0; Guard < 256 && OutLayers.Num() < Query.MaxLayers; ++Guard)
        {
            FHitResult Hit;
            if (!World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, QueryParams))
            {
                break;
            }
            UPrimitiveComponent* Component = Hit.GetComponent();
            AActor* Actor = Hit.GetActor();
            if (!Component)
            {
                break;
            }
            QueryParams.AddIgnoredComponent(Component);

            if (Query.bIgnoreVolumes && IsVolumeLikeHit(Actor, Component))
            {
                continue;
            }
            if (Query.bIgnoreTemp && Actor && Actor->ActorHasTag(TempActorTag))
            {
                continue;
            }

            FGroundLayer Layer;
            Layer.Point = Hit.ImpactPoint;
            Layer.Normal = Hit.ImpactNormal;
            Layer.Actor = Actor;
            Layer.Component = Component;
            OutLayers.Add(Layer);
        }
    }

    bool PickGroundLayer(const TArray<FGroundLayer>& Layers, int32 Layer, FGroundLayer& Out)
    {
        const int32 Index = Layer < 0 ? Layers.Num() - 1 : Layer;
        if (!Layers.IsValidIndex(Index))
        {
            return false;
        }
        Out = Layers[Index];
        return true;
    }

    void ReadGroundQuery(const TSharedPtr<FJsonObject>& Params, FGroundQuery& OutQuery, int32& OutLayer)
    {
        OutLayer = 0;
        if (!Params.IsValid())
        {
            return;
        }

        // Absent = the defaults (volumes + temp). Present = exactly what is listed.
        const TArray<TSharedPtr<FJsonValue>>* Ignore = nullptr;
        if (Params->TryGetArrayField(TEXT("ignore"), Ignore) && Ignore)
        {
            OutQuery.bIgnoreVolumes = false;
            OutQuery.bIgnoreTemp = false;
            for (const TSharedPtr<FJsonValue>& Value : *Ignore)
            {
                const FString Kind = Value->AsString();
                if (Kind.Equals(TEXT("volumes"), ESearchCase::IgnoreCase))     { OutQuery.bIgnoreVolumes = true; }
                else if (Kind.Equals(TEXT("temp"), ESearchCase::IgnoreCase))   { OutQuery.bIgnoreTemp = true; }
                else if (Kind.Equals(TEXT("roofs"), ESearchCase::IgnoreCase))  { OutLayer = -1; }
            }
        }

        double Number = 0.0;
        if (Params->TryGetNumberField(TEXT("layer"), Number))  { OutLayer = static_cast<int32>(Number); }
        if (Params->TryGetNumberField(TEXT("top_z"), Number))  { OutQuery.TopZ = Number; }
        if (Params->TryGetNumberField(TEXT("bottom_z"), Number)) { OutQuery.BottomZ = Number; }
    }

    bool SnapActorToGround(AActor* Actor, const FGroundQuery& InQuery, int32 Layer, double ZOffset, FString& OutMessage)
    {
        FGroundQuery Query = InQuery;
        Query.IgnoreActors.Add(Actor);

        const FVector Location = Actor->GetActorLocation();
        TArray<FGroundLayer> Layers;
        TraceGroundLayers(Actor->GetWorld(), Location.X, Location.Y, Query, Layers);

        FGroundLayer Ground;
        if (!PickGroundLayer(Layers, Layer, Ground))
        {
            OutMessage = FString::Printf(TEXT("No ground at (%.0f, %.0f): asked for layer %d, found %d"),
                Location.X, Location.Y, Layer, Layers.Num());
            return false;
        }

        const FBox Box = Actor->GetComponentsBoundingBox(/*bNonColliding=*/true);
        const double Bottom = Box.IsValid ? Box.Min.Z : Location.Z;
        Actor->SetActorLocation(Location + FVector(0.0, 0.0, Ground.Point.Z + ZOffset - Bottom));
        return true;
    }

    TSharedPtr<FJsonObject> MakeBoundsJson(AActor* Actor)
    {
        using UnrealMCPBridge::Json::MakeVector3JsonArray;

        auto Obj = MakeShared<FJsonObject>();
        const FBox Box = Actor ? Actor->GetComponentsBoundingBox(/*bNonColliding=*/true) : FBox(ForceInit);
        Obj->SetBoolField(TEXT("valid"), !!Box.IsValid);
        if (Box.IsValid)
        {
            Obj->SetField(TEXT("center"), MakeVector3JsonArray(Box.GetCenter()));
            Obj->SetField(TEXT("extent"), MakeVector3JsonArray(Box.GetExtent()));
            Obj->SetField(TEXT("size_m"), MakeVector3JsonArray(Box.GetSize() / 100.0));
            Obj->SetField(TEXT("min"),    MakeVector3JsonArray(Box.Min));
            Obj->SetField(TEXT("max"),    MakeVector3JsonArray(Box.Max));
        }
        return Obj;
    }

    AActor* FindActor(UWorld* World, const FString& NameOrPath, FString& OutError)
    {
        if (NameOrPath.IsEmpty())
        {
            OutError = TEXT("actor is empty");
            return nullptr;
        }
        if (NameOrPath.StartsWith(TEXT("/")))
        {
            if (AActor* ByPath = FindObject<AActor>(nullptr, *NameOrPath))
            {
                return ByPath;
            }
        }

        TArray<AActor*> ByLabel;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (Actor->GetName().Equals(NameOrPath, ESearchCase::IgnoreCase))
            {
                return Actor;
            }
            if (Actor->GetActorLabel().Equals(NameOrPath, ESearchCase::IgnoreCase))
            {
                ByLabel.Add(Actor);
            }
        }

        if (ByLabel.Num() == 1)
        {
            return ByLabel[0];
        }
        if (ByLabel.Num() > 1)
        {
            TArray<FString> Paths;
            for (const AActor* Actor : ByLabel)
            {
                Paths.Add(Actor->GetPathName());
            }
            OutError = FString::Printf(TEXT("Label '%s' matches %d actors — use the object path: %s"),
                *NameOrPath, ByLabel.Num(), *FString::Join(Paths, TEXT(", ")));
            return nullptr;
        }

        OutError = FString::Printf(TEXT("No actor named, labelled or at path '%s' in the editor world"), *NameOrPath);
        return nullptr;
    }
}

// ---------------------------------------------------------------------------------------------
// scene.* internals. A named namespace, not an anonymous one: UBT's unity build merges the
// command files into one translation unit (see CommandJsonHelpers.h).
// ---------------------------------------------------------------------------------------------

namespace MCPScene
{
    using namespace UnrealMCPBridge::Scene;
    using UnrealMCPBridge::Json::ReadVector3;
    using UnrealMCPBridge::Json::ReadRotator;
    using UnrealMCPBridge::Json::MakeVector3JsonArray;
    using UnrealMCPBridge::Json::MakeRotatorJsonArray;

    void Fail(MCPProtocol::FMCPError& OutError, int32 Code, const FString& Message)
    {
        OutError.Code = Code;
        OutError.Message = Message;
    }

    TArray<TSharedPtr<FJsonValue>> ToJsonStrings(const TArray<FString>& Strings)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const FString& S : Strings)
        {
            Values.Add(MakeShared<FJsonValueString>(S));
        }
        return Values;
    }

    TSharedPtr<FJsonObject> MakeTempActorJson(AActor* Actor)
    {
        auto Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"),  Actor->GetName());
        Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
        Obj->SetStringField(TEXT("path"),  Actor->GetPathName());
        Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
        Obj->SetField(TEXT("location"), MakeVector3JsonArray(Actor->GetActorLocation()));
        Obj->SetField(TEXT("rotation"), MakeRotatorJsonArray(Actor->GetActorRotation()));
        Obj->SetObjectField(TEXT("bounds"), MakeBoundsJson(Actor));

        TArray<FString> Tags;
        for (const FName& Tag : Actor->Tags)
        {
            Tags.Add(Tag.ToString());
        }
        Obj->SetArrayField(TEXT("tags"), ToJsonStrings(Tags));
        return Obj;
    }

    /**
     * Spawns a never-saved actor: RF_Transient and no external package, so even a World Partition
     * map gets no __ExternalActors__ file. Construction is deferred so the caller can set the
     * mesh before components register; the caller must call FinishSpawning.
     */
    AActor* BeginTransientSpawn(UWorld* World, UClass* Class, const FTransform& Transform)
    {
        FActorSpawnParameters SpawnParams;
        SpawnParams.ObjectFlags |= RF_Transient;
        SpawnParams.bCreateActorPackage = false;
        SpawnParams.OverrideLevel = World->PersistentLevel;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        SpawnParams.bDeferConstruction = true;
        SpawnParams.bNoFail = true;
        return World->SpawnActor(Class, &Transform, SpawnParams);
    }

    void TagTempActor(AActor* Actor, const TSharedPtr<FJsonObject>& Params, FName ExtraTag)
    {
        Actor->Tags.AddUnique(TempActorTag);
        FString Tag;
        if (Params->TryGetStringField(TEXT("tag"), Tag) && !Tag.IsEmpty())
        {
            Actor->Tags.AddUnique(FName(*Tag));
        }
        if (!ExtraTag.IsNone())
        {
            Actor->Tags.AddUnique(ExtraTag);
        }
        FString Label;
        if (Params->TryGetStringField(TEXT("label"), Label) && !Label.IsEmpty())
        {
            Actor->SetActorLabel(Label, /*bMarkDirty=*/false);
        }
    }

    /** Applies {"Path.To.Property": "ImportText value"} to an actor. Stops at the first failure. */
    bool ApplyPropertyMap(UObject* Target, const TSharedPtr<FJsonObject>& Params, FString& OutError)
    {
        const TSharedPtr<FJsonObject>* Properties = nullptr;
        if (!Params->TryGetObjectField(TEXT("properties"), Properties) || !Properties)
        {
            return true;
        }
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Properties)->Values)
        {
            // Numbers and bools are convenient to pass raw; ImportText takes their text form.
            FString Text;
            if (Pair.Value->Type == EJson::Boolean)
            {
                Text = Pair.Value->AsBool() ? TEXT("True") : TEXT("False");
            }
            else if (Pair.Value->Type == EJson::Number)
            {
                Text = FString::SanitizeFloat(Pair.Value->AsNumber(), /*InMinFractionalDigits=*/0);
            }
            else if (!Pair.Value->TryGetString(Text))
            {
                OutError = FString::Printf(TEXT("properties.%s: give the value as ImportText (a string), a number or a bool"), *Pair.Key);
                return false;
            }
            FString OldValue, NewValue, Error;
            if (!UnrealMCPBridge::Reflection::SetPropertyFromText(Target, Pair.Key, Text, OldValue, NewValue, Error))
            {
                OutError = FString::Printf(TEXT("properties.%s: %s"), *Pair.Key, *Error);
                return false;
            }
        }
        return true;
    }

    void SnapIfAsked(AActor* Actor, const TSharedPtr<FJsonObject>& Params, TArray<FString>& Warnings)
    {
        bool bSnap = false;
        if (!Params->TryGetBoolField(TEXT("snap_to_ground"), bSnap) || !bSnap)
        {
            return;
        }
        FGroundQuery Query;
        int32 Layer = 0;
        ReadGroundQuery(Params, Query, Layer);
        double ZOffset = 0.0;
        Params->TryGetNumberField(TEXT("z_offset"), ZOffset);
        FString Message;
        if (!SnapActorToGround(Actor, Query, Layer, ZOffset, Message))
        {
            Warnings.Add(Message);
        }
    }

    // --- blueprint allow list -----------------------------------------------------------------

    /** Object path of a Blueprint asset, from either the asset or its generated class. */
    FString BlueprintObjectPath(const UObject* AssetOrClass)
    {
        FString Path = AssetOrClass->GetPathName();
        if (Cast<UBlueprintGeneratedClass>(AssetOrClass) != nullptr)
        {
            Path.RemoveFromEnd(TEXT("_C"));
        }
        return Path;
    }

    bool IsBlueprintAllowed(const FString& ObjectPath)
    {
        const FString PackagePath = FPackageName::ObjectPathToPackageName(ObjectPath);
        for (FString Entry : GetDefault<UMCPSettings>()->TempSpawnBlueprintAllowList)
        {
            Entry.TrimStartAndEndInline();
            if (Entry.IsEmpty())
            {
                continue;
            }
            if (Entry.Contains(TEXT(".")))
            {
                FString EntryObject = Entry;
                EntryObject.RemoveFromEnd(TEXT("_C"));
                if (ObjectPath.Equals(EntryObject, ESearchCase::IgnoreCase))
                {
                    return true;
                }
                continue;
            }
            Entry.RemoveFromEnd(TEXT("/"));
            if (PackagePath.Equals(Entry, ESearchCase::IgnoreCase) || PackagePath.StartsWith(Entry + TEXT("/"), ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    }

    // --- spawn_temp ---------------------------------------------------------------------------

    struct FTempSpawnSpec
    {
        UClass* Class = nullptr;
        UStaticMesh* StaticMesh = nullptr;
        USkeletalMesh* SkeletalMesh = nullptr;
        bool bBlockout = false;
    };

    bool IsAllowedNativeTempClass(const UClass* Class)
    {
        return Class->IsChildOf<ALight>() || Class->IsChildOf<ACameraActor>()
            || Class->IsChildOf<AStaticMeshActor>() || Class->IsChildOf<ASkeletalMeshActor>();
    }

    bool ResolveTempAsset(const FString& Asset, bool bHasSize, FTempSpawnSpec& Out, FString& OutError)
    {
        if (Asset.IsEmpty() || Asset.Equals(TEXT("Cube"), ESearchCase::IgnoreCase) || Asset.Equals(TEXT("Box"), ESearchCase::IgnoreCase))
        {
            if (Asset.IsEmpty() && !bHasSize)
            {
                OutError = TEXT("asset is required (or size_m for a blockout box)");
                return false;
            }
            Out.bBlockout = true;
            Out.Class = AStaticMeshActor::StaticClass();
            Out.StaticMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
            if (!Out.StaticMesh)
            {
                OutError = TEXT("Could not load /Engine/BasicShapes/Cube");
                return false;
            }
            return true;
        }

        // Short names for the classes a photo set usually needs.
        static const TCHAR* const ShortNames[] = { TEXT("PointLight"), TEXT("SpotLight"), TEXT("RectLight"), TEXT("DirectionalLight"), TEXT("CameraActor") };
        UClass* const ShortClasses[] = { APointLight::StaticClass(), ASpotLight::StaticClass(), ARectLight::StaticClass(), ADirectionalLight::StaticClass(), ACameraActor::StaticClass() };
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(ShortNames); ++Index)
        {
            if (Asset.Equals(ShortNames[Index], ESearchCase::IgnoreCase))
            {
                Out.Class = ShortClasses[Index];
                return true;
            }
        }

        UObject* Object = UnrealMCPBridge::Reflection::ResolveObject(Asset);
        if (!Object)
        {
            OutError = FString::Printf(TEXT("Could not load asset or class: %s"), *Asset);
            return false;
        }

        if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Object))
        {
            Out.Class = AStaticMeshActor::StaticClass();
            Out.StaticMesh = StaticMesh;
            return true;
        }
        if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Object))
        {
            Out.Class = ASkeletalMeshActor::StaticClass();
            Out.SkeletalMesh = SkeletalMesh;
            return true;
        }

        UClass* Class = nullptr;
        if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
        {
            Class = Blueprint->GeneratedClass;
        }
        else
        {
            Class = Cast<UClass>(Object);
        }
        if (!Class || !Class->IsChildOf<AActor>())
        {
            OutError = FString::Printf(TEXT("%s is a %s — spawn_temp takes a static/skeletal mesh, an actor Blueprint, a light/camera class, or size_m for a blockout"),
                *Asset, *Object->GetClass()->GetName());
            return false;
        }

        if (Cast<UBlueprintGeneratedClass>(Class) != nullptr)
        {
            const FString BlueprintPath = BlueprintObjectPath(Class);
            if (!IsBlueprintAllowed(BlueprintPath))
            {
                OutError = FString::Printf(
                    TEXT("Blueprint %s is not in TempSpawnBlueprintAllowList (Project Settings > Plugins > Unreal MCP Bridge). Its construction script could create or change other level actors, so temp spawning only opens Blueprints someone has checked."),
                    *BlueprintPath);
                return false;
            }
        }
        else if (!IsAllowedNativeTempClass(Class))
        {
            OutError = FString::Printf(TEXT("Native class %s is not a temp-spawnable kind (lights, cameras, mesh actors)"), *Class->GetPathName());
            return false;
        }

        Out.Class = Class;
        return true;
    }

    bool ReadColor(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FLinearColor& Out)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!Params->TryGetArrayField(Field, Arr) || !Arr || Arr->Num() < 3)
        {
            return false;
        }
        Out = FLinearColor((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber(),
            Arr->Num() > 3 ? (*Arr)[3]->AsNumber() : 1.0);
        return true;
    }

    /** One spawn_temp item. Returns the actor JSON, or null with OutError (nothing is left behind then). */
    TSharedPtr<FJsonObject> SpawnTempItem(UWorld* World, const TSharedPtr<FJsonObject>& Item, FName ExtraTag, MCPProtocol::FMCPError& OutError)
    {
        FString Asset;
        Item->TryGetStringField(TEXT("asset"), Asset);

        FVector Location = FVector::ZeroVector;
        FRotator Rotation = FRotator::ZeroRotator;
        FVector Scale = FVector::OneVector;
        FVector SizeM = FVector::OneVector;
        if (!ReadVector3(Item, TEXT("location"), Location, OutError)) { return nullptr; }
        if (!ReadRotator(Item, TEXT("rotation"), Rotation, OutError)) { return nullptr; }
        if (!ReadVector3(Item, TEXT("scale"), Scale, OutError))       { return nullptr; }
        if (!ReadVector3(Item, TEXT("size_m"), SizeM, OutError))      { return nullptr; }

        FTempSpawnSpec Spec;
        FString Error;
        if (!ResolveTempAsset(Asset, Item->HasField(TEXT("size_m")), Spec, Error))
        {
            Fail(OutError, MCPProtocol::FMCPError::InvalidParams, Error);
            return nullptr;
        }
        if (Spec.bBlockout)
        {
            // The engine cube is 1 m on a side with its pivot at the centre, so metres are the scale.
            Scale *= SizeM;
        }

        const FTransform Transform(Rotation, Location, Scale);
        AActor* Actor = BeginTransientSpawn(World, Spec.Class, Transform);
        if (!Actor)
        {
            Fail(OutError, MCPProtocol::FMCPError::InternalError, FString::Printf(TEXT("SpawnActor failed for %s"), *Spec.Class->GetName()));
            return nullptr;
        }

        if (Spec.StaticMesh)
        {
            if (UStaticMeshComponent* MeshComponent = CastChecked<AStaticMeshActor>(Actor)->GetStaticMeshComponent())
            {
                MeshComponent->SetMobility(EComponentMobility::Movable);
                MeshComponent->SetStaticMesh(Spec.StaticMesh);
            }
        }
        if (Spec.SkeletalMesh)
        {
            if (USkeletalMeshComponent* MeshComponent = CastChecked<ASkeletalMeshActor>(Actor)->GetSkeletalMeshComponent())
            {
                MeshComponent->SetSkeletalMeshAsset(Spec.SkeletalMesh);
            }
        }
        if (ALight* Light = Cast<ALight>(Actor))
        {
            // Spawned lights have no baked data; anything but Movable renders as preview-unbuilt.
            if (ULightComponent* LightComponent = Light->GetLightComponent())
            {
                LightComponent->SetMobility(EComponentMobility::Movable);
            }
        }

        Actor->FinishSpawning(Transform);

        TArray<FString> Warnings;

        FString MaterialPath;
        FLinearColor Color;
        const bool bHasColor = ReadColor(Item, TEXT("color"), Color);
        if (Item->TryGetStringField(TEXT("material"), MaterialPath) || bHasColor)
        {
            UMeshComponent* MeshComponent = Actor->FindComponentByClass<UMeshComponent>();
            UMaterialInterface* Material = nullptr;
            if (!MaterialPath.IsEmpty())
            {
                Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
                if (!Material)
                {
                    Warnings.Add(FString::Printf(TEXT("Could not load material %s"), *MaterialPath));
                }
            }
            else if (MeshComponent)
            {
                UMaterialInterface* BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
                if (UMaterialInstanceDynamic* Dynamic = BaseMaterial ? UMaterialInstanceDynamic::Create(BaseMaterial, MeshComponent) : nullptr)
                {
                    Dynamic->SetVectorParameterValue(TEXT("Color"), Color);
                    Material = Dynamic;
                }
            }
            if (MeshComponent && Material)
            {
                for (int32 Slot = 0; Slot < FMath::Max(1, MeshComponent->GetNumMaterials()); ++Slot)
                {
                    MeshComponent->SetMaterial(Slot, Material);
                }
            }
        }

        TagTempActor(Actor, Item, ExtraTag);

        if (!ApplyPropertyMap(Actor, Item, Error))
        {
            World->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/false);
            Fail(OutError, MCPProtocol::FMCPError::InvalidParams, Error);
            return nullptr;
        }

        SnapIfAsked(Actor, Item, Warnings);

        TSharedPtr<FJsonObject> Json = MakeTempActorJson(Actor);
        if (Warnings.Num() > 0)
        {
            Json->SetArrayField(TEXT("warnings"), ToJsonStrings(Warnings));
        }
        return Json;
    }

    /**
     * Runs spawn_temp for a single item or an items[] list (top-level fields are defaults for each
     * item). All-or-nothing: on a failure every actor this call spawned is removed again.
     */
    TSharedPtr<FJsonObject> RunSpawnTemp(UWorld* World, const TSharedPtr<FJsonObject>& Params, FName ExtraTag, MCPProtocol::FMCPError& OutError)
    {
        TArray<TSharedPtr<FJsonObject>> Items;
        const TArray<TSharedPtr<FJsonValue>>* ItemValues = nullptr;
        if (Params->TryGetArrayField(TEXT("items"), ItemValues) && ItemValues)
        {
            for (const TSharedPtr<FJsonValue>& Value : *ItemValues)
            {
                const TSharedPtr<FJsonObject>* ItemObject = nullptr;
                if (!Value->TryGetObject(ItemObject) || !ItemObject)
                {
                    Fail(OutError, MCPProtocol::FMCPError::InvalidParams, TEXT("items must be an array of objects"));
                    return nullptr;
                }
                // Item fields win over the shared top-level ones.
                TSharedPtr<FJsonObject> Merged = MakeShared<FJsonObject>();
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Params->Values)
                {
                    if (Pair.Key != TEXT("items"))
                    {
                        Merged->SetField(Pair.Key, Pair.Value);
                    }
                }
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*ItemObject)->Values)
                {
                    Merged->SetField(Pair.Key, Pair.Value);
                }
                Items.Add(Merged);
            }
        }
        else
        {
            Items.Add(Params);
        }

        TArray<TSharedPtr<FJsonValue>> Actors;
        TArray<FString> SpawnedPaths;
        for (int32 Index = 0; Index < Items.Num(); ++Index)
        {
            TSharedPtr<FJsonObject> ActorJson = SpawnTempItem(World, Items[Index], ExtraTag, OutError);
            if (!ActorJson)
            {
                for (const FString& Path : SpawnedPaths)
                {
                    if (AActor* Spawned = FindObject<AActor>(nullptr, *Path))
                    {
                        World->EditorDestroyActor(Spawned, /*bShouldModifyLevel=*/false);
                    }
                }
                OutError.Message = FString::Printf(TEXT("items[%d]: %s"), Index, *OutError.Message);
                return nullptr;
            }
            SpawnedPaths.Add(ActorJson->GetStringField(TEXT("path")));
            Actors.Add(MakeShared<FJsonValueObject>(ActorJson));
        }

        auto Result = MakeShared<FJsonObject>();
        Result->SetArrayField(TEXT("actors"), Actors);
        return Result;
    }

    // --- pose_character ------------------------------------------------------------------------

    FString PosePresetPath(const TSharedPtr<FJsonObject>& Params)
    {
        FString File;
        Params->TryGetStringField(TEXT("preset_file"), File);
        if (File.IsEmpty())
        {
            File = GetDefault<UMCPSettings>()->PosePresetFile;
        }
        if (File.IsEmpty())
        {
            File = FPaths::ProjectSavedDir() / TEXT("MCPBridge/pose_presets.json");
        }
        else if (FPaths::IsRelative(File))
        {
            File = FPaths::ProjectDir() / File;
        }
        return FPaths::ConvertRelativePathToFull(File);
    }

    /** Returns Params with the named preset's fields underneath (explicit params win). */
    TSharedPtr<FJsonObject> ApplyPosePreset(const TSharedPtr<FJsonObject>& Params, FString& OutError)
    {
        FString PresetName;
        if (!Params->TryGetStringField(TEXT("preset"), PresetName) || PresetName.IsEmpty())
        {
            return Params;
        }

        const FString File = PosePresetPath(Params);
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *File))
        {
            OutError = FString::Printf(TEXT("Preset file not found: %s"), *File);
            return nullptr;
        }
        TSharedPtr<FJsonObject> Root;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
        {
            OutError = FString::Printf(TEXT("Preset file is not a JSON object: %s"), *File);
            return nullptr;
        }

        // Either {"presets": {name: {...}}} or {name: {...}} directly.
        TSharedPtr<FJsonObject> Table = Root;
        const TSharedPtr<FJsonObject>* Nested = nullptr;
        if (Root->TryGetObjectField(TEXT("presets"), Nested) && Nested)
        {
            Table = *Nested;
        }
        const TSharedPtr<FJsonObject>* Preset = nullptr;
        if (!Table->TryGetObjectField(PresetName, Preset) || !Preset)
        {
            TArray<FString> Names;
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Table->Values)
            {
                Names.Add(Pair.Key);
            }
            OutError = FString::Printf(TEXT("Preset '%s' not in %s (has: %s)"), *PresetName, *File, *FString::Join(Names, TEXT(", ")));
            return nullptr;
        }

        TSharedPtr<FJsonObject> Merged = MakeShared<FJsonObject>();
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Preset)->Values)
        {
            Merged->SetField(Pair.Key, Pair.Value);
        }
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Params->Values)
        {
            if (Pair.Key != TEXT("preset") && Pair.Key != TEXT("preset_file"))
            {
                Merged->SetField(Pair.Key, Pair.Value);
            }
        }
        return Merged;
    }

    /**
     * Evaluates the pose now instead of on the next editor tick. A background editor throttles its
     * tick (see the bridge notes), so "it will update next frame" can mean "not before the capture".
     * With no tick function the evaluation runs synchronously on the game thread.
     */
    void ForceEvaluatePose(USkeletalMeshComponent* Leader)
    {
        Leader->TickAnimation(0.f, /*bNeedsValidRootMotion=*/false);
        Leader->RefreshBoneTransforms();
        Leader->RefreshFollowerComponents();
        Leader->UpdateChildTransforms(EUpdateTransformFlags::OnlyUpdateIfUsingSocket);
        Leader->MarkRenderDynamicDataDirty();
    }

    /**
     * Single-node playback frozen at Time. Returns the clip length (0 for assets without one).
     * Shared by pose_character and temp_override so both freeze a pose the same way.
     */
    float FreezeAnimation(USkeletalMeshComponent* Component, UAnimationAsset* Anim, const TSharedPtr<FJsonObject>& Params, TArray<FString>& Warnings, float& OutTime)
    {
        const UAnimSequenceBase* Sequence = Cast<UAnimSequenceBase>(Anim);
        const float Length = Sequence ? Sequence->GetPlayLength() : 0.f;

        double Time = 0.0;
        double Fraction = 0.0;
        if (Params->TryGetNumberField(TEXT("fraction"), Fraction))
        {
            Time = Fraction * Length;
        }
        else
        {
            Params->TryGetNumberField(TEXT("time"), Time);
        }
        OutTime = Length > 0.f ? FMath::Clamp(static_cast<float>(Time), 0.f, Length) : static_cast<float>(Time);

        const USkeleton* AnimSkeleton = Anim->GetSkeleton();
        const USkeletalMesh* Mesh = Component->GetSkeletalMeshAsset();
        const USkeleton* MeshSkeleton = Mesh ? Mesh->GetSkeleton() : nullptr;
        if (AnimSkeleton && MeshSkeleton && AnimSkeleton != MeshSkeleton && !AnimSkeleton->IsCompatibleForEditor(MeshSkeleton))
        {
            Warnings.Add(FString::Printf(TEXT("%s uses skeleton %s, the mesh uses %s and they are not marked compatible — the pose may not apply"),
                *Anim->GetName(), *AnimSkeleton->GetName(), *MeshSkeleton->GetName()));
        }

        Component->SetUpdateAnimationInEditor(true);
        Component->SetAnimationMode(EAnimationMode::AnimationSingleNode);
        Component->OverrideAnimationData(Anim, /*bIsLooping=*/false, /*bIsPlaying=*/false, OutTime, /*PlayRate=*/0.f);
        Component->SetPosition(OutTime, /*bFireNotifies=*/false);
        ForceEvaluatePose(Component);
        return Length;
    }

    TSharedPtr<FJsonObject> RunPoseCharacter(UWorld* World, const TSharedPtr<FJsonObject>& InParams, FName ExtraTag, MCPProtocol::FMCPError& OutError)
    {
        FString Error;
        const TSharedPtr<FJsonObject> Params = ApplyPosePreset(InParams, Error);
        if (!Params)
        {
            Fail(OutError, MCPProtocol::FMCPError::InvalidParams, Error);
            return nullptr;
        }

        TArray<FString> MeshPaths;
        const TArray<TSharedPtr<FJsonValue>>* MeshValues = nullptr;
        if (Params->TryGetArrayField(TEXT("meshes"), MeshValues) && MeshValues)
        {
            for (const TSharedPtr<FJsonValue>& Value : *MeshValues)
            {
                MeshPaths.Add(Value->AsString());
            }
        }
        if (MeshPaths.Num() == 0)
        {
            Fail(OutError, MCPProtocol::FMCPError::InvalidParams, TEXT("meshes (first = leader) or a preset that has them is required"));
            return nullptr;
        }

        TArray<USkeletalMesh*> Meshes;
        for (const FString& Path : MeshPaths)
        {
            USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *Path);
            if (!Mesh)
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams, FString::Printf(TEXT("Could not load skeletal mesh %s"), *Path));
                return nullptr;
            }
            Meshes.Add(Mesh);
        }

        UAnimationAsset* Anim = nullptr;
        FString AnimPath;
        if (Params->TryGetStringField(TEXT("anim"), AnimPath) && !AnimPath.IsEmpty())
        {
            Anim = LoadObject<UAnimationAsset>(nullptr, *AnimPath);
            if (!Anim)
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams, FString::Printf(TEXT("Could not load animation %s"), *AnimPath));
                return nullptr;
            }
        }

        FVector Location = FVector::ZeroVector;
        FRotator Rotation = FRotator::ZeroRotator;
        if (!ReadVector3(Params, TEXT("location"), Location, OutError)) { return nullptr; }
        if (!ReadRotator(Params, TEXT("rotation"), Rotation, OutError)) { return nullptr; }
        double YawOffset = 0.0;
        Params->TryGetNumberField(TEXT("yaw_offset"), YawOffset);
        const FRotator FacingRotation = Rotation;
        Rotation.Yaw += YawOffset;

        const FTransform Transform(Rotation, Location);
        ASkeletalMeshActor* Actor = Cast<ASkeletalMeshActor>(BeginTransientSpawn(World, ASkeletalMeshActor::StaticClass(), Transform));
        if (!Actor)
        {
            Fail(OutError, MCPProtocol::FMCPError::InternalError, TEXT("SpawnActor failed for SkeletalMeshActor"));
            return nullptr;
        }
        USkeletalMeshComponent* Leader = Actor->GetSkeletalMeshComponent();
        Leader->SetSkeletalMeshAsset(Meshes[0]);
        Actor->FinishSpawning(Transform);

        TArray<FString> Warnings;
        TArray<TSharedPtr<FJsonValue>> Parts;
        auto AddPart = [&Parts](const FString& Mesh, const UActorComponent* Component, const FString& Role)
        {
            auto Part = MakeShared<FJsonObject>();
            Part->SetStringField(TEXT("mesh"), Mesh);
            Part->SetStringField(TEXT("component"), Component->GetName());
            Part->SetStringField(TEXT("role"), Role);
            Parts.Add(MakeShared<FJsonValueObject>(Part));
        };
        AddPart(MeshPaths[0], Leader, TEXT("leader"));

        for (int32 Index = 1; Index < Meshes.Num(); ++Index)
        {
            USkeletalMeshComponent* Follower = NewObject<USkeletalMeshComponent>(Actor, NAME_None, RF_Transient);
            Follower->SetSkeletalMeshAsset(Meshes[Index]);
            Follower->SetupAttachment(Leader);
            Follower->RegisterComponent();
            Actor->AddInstanceComponent(Follower);
            Follower->SetLeaderPoseComponent(Leader, /*bForceUpdate=*/true);
            AddPart(MeshPaths[Index], Follower, TEXT("leader_pose"));
        }

        float Time = 0.f;
        float Length = 0.f;
        if (Anim)
        {
            Length = FreezeAnimation(Leader, Anim, Params, Warnings, Time);
        }

        FString WeaponPath;
        if (Params->TryGetStringField(TEXT("weapon"), WeaponPath) && !WeaponPath.IsEmpty())
        {
            FString SocketString;
            Params->TryGetStringField(TEXT("socket"), SocketString);
            const FName Socket(*SocketString);
            if (!Socket.IsNone() && !Leader->DoesSocketExist(Socket))
            {
                Warnings.Add(FString::Printf(TEXT("Socket %s does not exist on %s — the weapon sits at the mesh origin"), *SocketString, *Meshes[0]->GetName()));
            }

            FVector WeaponLocation = FVector::ZeroVector;
            FRotator WeaponRotation = FRotator::ZeroRotator;
            if (!ReadVector3(Params, TEXT("weapon_location"), WeaponLocation, OutError) || !ReadRotator(Params, TEXT("weapon_rotation"), WeaponRotation, OutError))
            {
                World->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/false);
                return nullptr;
            }

            UObject* WeaponAsset = UnrealMCPBridge::Reflection::ResolveObject(WeaponPath);
            USceneComponent* WeaponComponent = nullptr;
            if (UStaticMesh* WeaponStatic = Cast<UStaticMesh>(WeaponAsset))
            {
                UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Actor, NAME_None, RF_Transient);
                Component->SetMobility(EComponentMobility::Movable);
                Component->SetStaticMesh(WeaponStatic);
                WeaponComponent = Component;
            }
            else if (USkeletalMesh* WeaponSkeletal = Cast<USkeletalMesh>(WeaponAsset))
            {
                USkeletalMeshComponent* Component = NewObject<USkeletalMeshComponent>(Actor, NAME_None, RF_Transient);
                Component->SetSkeletalMeshAsset(WeaponSkeletal);
                WeaponComponent = Component;
            }

            if (WeaponComponent)
            {
                WeaponComponent->SetupAttachment(Leader, Socket);
                WeaponComponent->SetRelativeLocationAndRotation(WeaponLocation, WeaponRotation);
                WeaponComponent->RegisterComponent();
                Actor->AddInstanceComponent(WeaponComponent);
                AddPart(WeaponPath, WeaponComponent, TEXT("weapon"));
            }
            else
            {
                Warnings.Add(FString::Printf(TEXT("weapon %s is not a static or skeletal mesh — skipped"), *WeaponPath));
            }
        }

        TagTempActor(Actor, Params, ExtraTag);
        SnapIfAsked(Actor, Params, Warnings);
        if (Anim)
        {
            // Snapping moved the actor; socket children follow through the transform update, but
            // the pose itself must survive any re-register the move caused.
            ForceEvaluatePose(Leader);
        }

        TSharedPtr<FJsonObject> Result = MakeTempActorJson(Actor);
        Result->SetArrayField(TEXT("parts"), Parts);
        Result->SetNumberField(TEXT("facing_yaw"), FacingRotation.Yaw);
        Result->SetNumberField(TEXT("yaw_offset"), YawOffset);
        if (Anim)
        {
            Result->SetStringField(TEXT("anim"), Anim->GetPathName());
            Result->SetNumberField(TEXT("time"), Time);
            Result->SetNumberField(TEXT("length"), Length);
        }
        if (Warnings.Num() > 0)
        {
            Result->SetArrayField(TEXT("warnings"), ToJsonStrings(Warnings));
        }
        return Result;
    }

    // --- temp_override ---------------------------------------------------------------------------

    /**
     * What temp_override changed on a real level actor, and what to put back. Weak pointers
     * throughout: the registry is static and must not keep a closed map's actors alive.
     * Only transient editor state (bHiddenEdTemporary) and in-memory animation playback are
     * touched — never a saved hidden flag.
     */
    struct FTempOverride
    {
        TWeakObjectPtr<AActor> Actor;
        FName Tag;

        bool bHidden = false;
        bool bWasTemporarilyHidden = false;

        TWeakObjectPtr<USkeletalMeshComponent> AnimComponent;
        EAnimationMode::Type SavedMode = EAnimationMode::AnimationBlueprint;
        TWeakObjectPtr<UAnimationAsset> SavedAnim;
        bool bSavedLooping = true;
        bool bSavedPlaying = true;
        float SavedPosition = 0.f;
        float SavedPlayRate = 1.f;
        bool bSavedUpdateInEditor = false;
    };

    TArray<FTempOverride>& Overrides()
    {
        static TArray<FTempOverride> Registry;
        return Registry;
    }

    void RevertOverride(FTempOverride& Override)
    {
        AActor* Actor = Override.Actor.Get();
        if (Actor && Override.bHidden && !Override.bWasTemporarilyHidden)
        {
            Actor->SetIsTemporarilyHiddenInEditor(false);
        }
        if (USkeletalMeshComponent* Component = Override.AnimComponent.Get())
        {
            Component->AnimationData.AnimToPlay = Override.SavedAnim.Get();
            Component->AnimationData.bSavedLooping = Override.bSavedLooping;
            Component->AnimationData.bSavedPlaying = Override.bSavedPlaying;
            Component->AnimationData.SavedPosition = Override.SavedPosition;
            Component->AnimationData.SavedPlayRate = Override.SavedPlayRate;
            Component->SetUpdateAnimationInEditor(Override.bSavedUpdateInEditor);
            // AnimClass was never changed, so restoring the mode re-creates the original instance.
            Component->SetAnimationMode(Override.SavedMode, /*bForceInitAnimScriptInstance=*/true);
            ForceEvaluatePose(Component);
        }
    }

    /** Packages that became dirty since Before are set clean again; returns their names. */
    TArray<FString> RestoreCleanFlags(const FDirtySnapshot& Before)
    {
        TArray<FString> Cleaned;
        for (TObjectIterator<UPackage> It; It; ++It)
        {
            if (*It != GetTransientPackage() && It->IsDirty() && !Before.DirtyPackages.Contains(It->GetFName()))
            {
                It->SetDirtyFlag(false);
                Cleaned.Add(It->GetName());
            }
        }
        return Cleaned;
    }

    /** Reverts every override whose actor lives in Package (map package or OFPA actor package). */
    void OnPreSavePackage(UPackage* Package, FObjectPreSaveContext /*Context*/)
    {
        TArray<FTempOverride>& Registry = Overrides();
        for (int32 Index = Registry.Num() - 1; Index >= 0; --Index)
        {
            const AActor* Actor = Registry[Index].Actor.Get();
            if (!Actor)
            {
                Registry.RemoveAtSwap(Index);
                continue;
            }
            if (Actor->GetPackage() == Package || Actor->GetOutermost() == Package)
            {
                UE_LOG(LogMCPBridge, Warning, TEXT("scene.temp_override: reverting %s before %s is saved"), *Actor->GetActorLabel(), *Package->GetName());
                RevertOverride(Registry[Index]);
                Registry.RemoveAtSwap(Index);
            }
        }
    }

    FDelegateHandle& PreSaveHandle()
    {
        static FDelegateHandle Handle;
        return Handle;
    }

    TSharedPtr<FJsonObject> RunTempOverride(UWorld* World, const TSharedPtr<FJsonObject>& Params, FName ExtraTag, MCPProtocol::FMCPError& OutError)
    {
        FString ActorName, Error;
        if (!UnrealMCPBridge::Json::RequireString(Params, TEXT("actor"), ActorName, OutError))
        {
            return nullptr;
        }
        AActor* Actor = FindActor(World, ActorName, Error);
        if (!Actor)
        {
            Fail(OutError, MCPProtocol::FMCPError::InvalidParams, Error);
            return nullptr;
        }
        if (Actor->ActorHasTag(TempActorTag))
        {
            Fail(OutError, MCPProtocol::FMCPError::InvalidParams, TEXT("That is a temp actor — pose or hide it directly; temp_override is for real level actors"));
            return nullptr;
        }

        const FDirtySnapshot Before = SnapshotDirtyPackages();
        TArray<FTempOverride>& Registry = Overrides();
        int32 Index = Registry.IndexOfByPredicate([Actor](const FTempOverride& O) { return O.Actor.Get() == Actor; });

        bool bRevert = false;
        if (Params->TryGetBoolField(TEXT("revert"), bRevert) && bRevert)
        {
            if (Index != INDEX_NONE)
            {
                RevertOverride(Registry[Index]);
                Registry.RemoveAtSwap(Index);
            }
            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("reverted"), Index != INDEX_NONE);
            Result->SetArrayField(TEXT("cleaned_dirty_flags"), ToJsonStrings(RestoreCleanFlags(Before)));
            return Result;
        }

        if (Index == INDEX_NONE)
        {
            FTempOverride Fresh;
            Fresh.Actor = Actor;
            FString Tag;
            Params->TryGetStringField(TEXT("tag"), Tag);
            Fresh.Tag = !ExtraTag.IsNone() ? ExtraTag : (Tag.IsEmpty() ? FName(NAME_None) : FName(*Tag));
            Index = Registry.Add(Fresh);
        }
        FTempOverride& Override = Registry[Index];

        TArray<FString> Warnings;
        bool bHide = false;
        if (Params->TryGetBoolField(TEXT("hide"), bHide))
        {
            if (bHide && !Override.bHidden)
            {
                Override.bWasTemporarilyHidden = Actor->IsTemporarilyHiddenInEditor();
                Actor->SetIsTemporarilyHiddenInEditor(true);
                Override.bHidden = true;
            }
            else if (!bHide && Override.bHidden)
            {
                if (!Override.bWasTemporarilyHidden)
                {
                    Actor->SetIsTemporarilyHiddenInEditor(false);
                }
                Override.bHidden = false;
            }
        }

        float Time = 0.f;
        FString AnimPath;
        if (Params->TryGetStringField(TEXT("anim"), AnimPath) && !AnimPath.IsEmpty())
        {
            UAnimationAsset* Anim = LoadObject<UAnimationAsset>(nullptr, *AnimPath);
            if (!Anim)
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams, FString::Printf(TEXT("Could not load animation %s"), *AnimPath));
                return nullptr;
            }

            FString ComponentName;
            Params->TryGetStringField(TEXT("component"), ComponentName);
            USkeletalMeshComponent* Component = nullptr;
            TInlineComponentArray<USkeletalMeshComponent*> Components(Actor);
            for (USkeletalMeshComponent* Candidate : Components)
            {
                if (ComponentName.IsEmpty() || Candidate->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
                {
                    Component = Candidate;
                    break;
                }
            }
            if (!Component)
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams, FString::Printf(TEXT("%s has no skeletal mesh component%s%s"),
                    *Actor->GetActorLabel(), ComponentName.IsEmpty() ? TEXT("") : TEXT(" named "), *ComponentName));
                return nullptr;
            }

            if (!Override.AnimComponent.IsValid())
            {
                Override.AnimComponent = Component;
                Override.SavedMode = Component->GetAnimationMode();
                Override.SavedAnim = Component->AnimationData.AnimToPlay.Get();
                Override.bSavedLooping = Component->AnimationData.bSavedLooping;
                Override.bSavedPlaying = Component->AnimationData.bSavedPlaying;
                Override.SavedPosition = Component->AnimationData.SavedPosition;
                Override.SavedPlayRate = Component->AnimationData.SavedPlayRate;
                Override.bSavedUpdateInEditor = Component->GetUpdateAnimationInEditor();
            }
            FreezeAnimation(Component, Anim, Params, Warnings, Time);
        }

        // Nothing here is meant to persist, and the PreSave hook reverts it before any save — so
        // a package this call dirtied would only nag the user to save a level they did not change.
        const TArray<FString> Cleaned = RestoreCleanFlags(Before);

        auto Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("actor"), Actor->GetPathName());
        Result->SetBoolField(TEXT("hidden"), Override.bHidden);
        Result->SetBoolField(TEXT("anim_overridden"), Override.AnimComponent.IsValid());
        if (!AnimPath.IsEmpty())
        {
            Result->SetNumberField(TEXT("time"), Time);
        }
        Result->SetStringField(TEXT("tag"), Override.Tag.IsNone() ? FString() : Override.Tag.ToString());
        Result->SetArrayField(TEXT("cleaned_dirty_flags"), ToJsonStrings(Cleaned));
        if (Warnings.Num() > 0)
        {
            Result->SetArrayField(TEXT("warnings"), ToJsonStrings(Warnings));
        }
        return Result;
    }

    // --- clear_temp ------------------------------------------------------------------------------

    /** Removes temp actors (all, or those carrying Tag) found by scanning the world, and reverts overrides. */
    TSharedPtr<FJsonObject> RunClearTemp(UWorld* World, FName Tag)
    {
        TArray<AActor*> ToRemove;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (Actor->ActorHasTag(TempActorTag) && (Tag.IsNone() || Actor->ActorHasTag(Tag)))
            {
                ToRemove.Add(Actor);
            }
        }

        TArray<FString> Removed;
        for (AActor* Actor : ToRemove)
        {
            Removed.Add(Actor->GetActorLabel());
            if (GEditor && Actor->IsSelected())
            {
                GEditor->SelectActor(Actor, /*bInSelected=*/false, /*bNotify=*/true);
            }
            World->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/false);
        }

        const FDirtySnapshot BeforeRevert = SnapshotDirtyPackages();
        TArray<FString> Reverted;
        TArray<FTempOverride>& Registry = Overrides();
        for (int32 Index = Registry.Num() - 1; Index >= 0; --Index)
        {
            if (Tag.IsNone() || Registry[Index].Tag == Tag)
            {
                if (const AActor* Actor = Registry[Index].Actor.Get())
                {
                    Reverted.Add(Actor->GetActorLabel());
                }
                RevertOverride(Registry[Index]);
                Registry.RemoveAtSwap(Index);
            }
        }
        RestoreCleanFlags(BeforeRevert);

        auto Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("removed_count"), Removed.Num());
        Result->SetArrayField(TEXT("removed"), ToJsonStrings(Removed));
        Result->SetArrayField(TEXT("reverted_overrides"), ToJsonStrings(Reverted));
        return Result;
    }

    // --- get_ground_height ---------------------------------------------------------------------

    TSharedPtr<FJsonObject> RunGroundHeight(UWorld* World, const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
        if (!Params->TryGetArrayField(TEXT("points"), Points) || !Points || Points->Num() == 0)
        {
            Fail(OutError, MCPProtocol::FMCPError::InvalidParams, TEXT("points ([[x, y], ...]) is required"));
            return nullptr;
        }

        FGroundQuery Query;
        int32 Layer = 0;
        ReadGroundQuery(Params, Query, Layer);
        bool bIncludeLayers = true;
        Params->TryGetBoolField(TEXT("include_layers"), bIncludeLayers);

        TArray<TSharedPtr<FJsonValue>> Results;
        for (const TSharedPtr<FJsonValue>& PointValue : *Points)
        {
            const TArray<TSharedPtr<FJsonValue>>* XY = nullptr;
            if (!PointValue->TryGetArray(XY) || !XY || XY->Num() < 2)
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams, TEXT("each point must be [x, y]"));
                return nullptr;
            }
            const double X = (*XY)[0]->AsNumber();
            const double Y = (*XY)[1]->AsNumber();

            TArray<FGroundLayer> Layers;
            TraceGroundLayers(World, X, Y, Query, Layers);
            FGroundLayer Picked;
            const bool bHit = PickGroundLayer(Layers, Layer, Picked);

            auto Entry = MakeShared<FJsonObject>();
            Entry->SetNumberField(TEXT("x"), X);
            Entry->SetNumberField(TEXT("y"), Y);
            Entry->SetBoolField(TEXT("hit"), bHit);
            if (bHit)
            {
                Entry->SetNumberField(TEXT("z"), Picked.Point.Z);
                Entry->SetStringField(TEXT("actor"), Picked.Actor.IsValid() ? Picked.Actor->GetActorLabel() : FString());
            }
            if (bIncludeLayers)
            {
                TArray<TSharedPtr<FJsonValue>> LayerArray;
                for (const FGroundLayer& L : Layers)
                {
                    auto LayerJson = MakeShared<FJsonObject>();
                    LayerJson->SetNumberField(TEXT("z"), L.Point.Z);
                    LayerJson->SetNumberField(TEXT("normal_z"), L.Normal.Z);
                    LayerJson->SetStringField(TEXT("actor"), L.Actor.IsValid() ? L.Actor->GetActorLabel() : FString());
                    LayerJson->SetStringField(TEXT("actor_path"), L.Actor.IsValid() ? L.Actor->GetPathName() : FString());
                    LayerJson->SetStringField(TEXT("component"), L.Component.IsValid() ? L.Component->GetName() : FString());
                    LayerArray.Add(MakeShared<FJsonValueObject>(LayerJson));
                }
                Entry->SetArrayField(TEXT("layers"), LayerArray);
            }
            Results.Add(MakeShared<FJsonValueObject>(Entry));
        }

        auto Result = MakeShared<FJsonObject>();
        Result->SetArrayField(TEXT("points"), Results);
        Result->SetNumberField(TEXT("layer"), Layer);
        return Result;
    }

    // --- capture_view ------------------------------------------------------------------------------

    struct FCaptureMark
    {
        FString Name;
        FVector Location = FVector::ZeroVector;
    };

    struct FCaptureRequest
    {
        FVector Location = FVector::ZeroVector;
        FRotator Rotation = FRotator::ZeroRotator;
        float Fov = 60.f;
        int32 Width = 1600;
        int32 Height = 900;
        float OrthoWidth = 0.f;
        bool bGameView = true;
        int32 Warmup = 4;
        bool bStreamTextures = true;
        float StreamSeconds = 2.f;
        FString Path;
        TArray<FCaptureMark> Marks;
        TArray<TWeakObjectPtr<AActor>> HiddenActors;
        TArray<FString> Warnings;
    };

    bool ResolveCapturePath(const FString& InPath, FString& OutPath, FString& OutError)
    {
        FString Path = InPath;
        if (Path.IsEmpty())
        {
            Path = FString::Printf(TEXT("MCPBridge/Captures/capture_%s.png"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S_%s")));
        }
        if (FPaths::IsRelative(Path))
        {
            Path = (Path.StartsWith(TEXT("Saved/")) || Path.StartsWith(TEXT("Saved\\")))
                ? FPaths::ProjectDir() / Path
                : FPaths::ProjectSavedDir() / Path;
        }
        Path = FPaths::ConvertRelativePathToFull(Path);
        FPaths::NormalizeFilename(Path);
        FPaths::CollapseRelativeDirectories(Path);

        const FString Extension = FPaths::GetExtension(Path).ToLower();
        if (Extension.IsEmpty())
        {
            Path += TEXT(".png");
        }
        else if (Extension != TEXT("png") && Extension != TEXT("jpg") && Extension != TEXT("jpeg"))
        {
            OutError = FString::Printf(TEXT("path must end in .png or .jpg: %s"), *Path);
            return false;
        }

        TArray<FString> Roots;
        Roots.Add(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()));
        for (const FString& Root : GetDefault<UMCPSettings>()->CaptureOutputRoots)
        {
            if (!Root.IsEmpty())
            {
                Roots.Add(FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Root) ? FPaths::ProjectDir() / Root : Root));
            }
        }
        for (FString Root : Roots)
        {
            FPaths::NormalizeDirectoryName(Root);
            if (FPaths::IsUnderDirectory(Path, Root))
            {
                OutPath = Path;
                return true;
            }
        }
        OutError = FString::Printf(TEXT("path must be under <Project>/Saved or a CaptureOutputRoots folder: %s"), *Path);
        return false;
    }

    bool ParseCaptureRequest(UWorld* World, const TSharedPtr<FJsonObject>& Params, FCaptureRequest& Out, MCPProtocol::FMCPError& OutError)
    {
        if (!Params->HasField(TEXT("location")))
        {
            Fail(OutError, MCPProtocol::FMCPError::InvalidParams, TEXT("location is required"));
            return false;
        }
        if (!ReadVector3(Params, TEXT("location"), Out.Location, OutError)) { return false; }

        FVector LookAt;
        if (Params->HasField(TEXT("look_at")))
        {
            if (!ReadVector3(Params, TEXT("look_at"), LookAt, OutError)) { return false; }
            Out.Rotation = (LookAt - Out.Location).Rotation();
        }
        else if (!ReadRotator(Params, TEXT("rotation"), Out.Rotation, OutError))
        {
            return false;
        }

        double Number = 0.0;
        if (Params->TryGetNumberField(TEXT("fov"), Number))           { Out.Fov = FMath::Clamp(static_cast<float>(Number), 1.f, 170.f); }
        if (Params->TryGetNumberField(TEXT("width"), Number))         { Out.Width = FMath::Clamp(static_cast<int32>(Number), 16, 8192); }
        if (Params->TryGetNumberField(TEXT("height"), Number))        { Out.Height = FMath::Clamp(static_cast<int32>(Number), 16, 8192); }
        if (Params->TryGetNumberField(TEXT("ortho_width"), Number))   { Out.OrthoWidth = static_cast<float>(Number); }
        if (Params->TryGetNumberField(TEXT("warmup"), Number))        { Out.Warmup = FMath::Clamp(static_cast<int32>(Number), 0, 64); }
        if (Params->TryGetNumberField(TEXT("stream_seconds"), Number)) { Out.StreamSeconds = FMath::Max(0.f, static_cast<float>(Number)); }
        Params->TryGetBoolField(TEXT("game_view"), Out.bGameView);
        Params->TryGetBoolField(TEXT("stream_textures"), Out.bStreamTextures);

        FString PathParam, Error;
        Params->TryGetStringField(TEXT("path"), PathParam);
        if (!ResolveCapturePath(PathParam, Out.Path, Error))
        {
            Fail(OutError, MCPProtocol::FMCPError::InvalidParams, Error);
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Marks = nullptr;
        if (Params->TryGetArrayField(TEXT("marks"), Marks) && Marks)
        {
            for (int32 Index = 0; Index < Marks->Num(); ++Index)
            {
                const TSharedPtr<FJsonValue>& Value = (*Marks)[Index];
                FCaptureMark Mark;
                Mark.Name = FString::FromInt(Index);
                const TArray<TSharedPtr<FJsonValue>>* XYZ = nullptr;
                const TSharedPtr<FJsonObject>* Object = nullptr;
                if (Value->TryGetArray(XYZ) && XYZ && XYZ->Num() == 3)
                {
                    Mark.Location = FVector((*XYZ)[0]->AsNumber(), (*XYZ)[1]->AsNumber(), (*XYZ)[2]->AsNumber());
                }
                else if (Value->TryGetObject(Object) && Object)
                {
                    (*Object)->TryGetStringField(TEXT("name"), Mark.Name);
                    if (!ReadVector3(*Object, TEXT("location"), Mark.Location, OutError)) { return false; }
                }
                else
                {
                    Fail(OutError, MCPProtocol::FMCPError::InvalidParams, TEXT("marks entries are [x, y, z] or {name, location}"));
                    return false;
                }
                Out.Marks.Add(Mark);
            }
        }

        // Actors to leave out: what temp_override hid, what the user hid in the viewport (H), and
        // anything named in hide_actors.
        for (const FTempOverride& Override : Overrides())
        {
            if (Override.bHidden && Override.Actor.IsValid())
            {
                Out.HiddenActors.AddUnique(Override.Actor);
            }
        }
        bool bRespectEditorHidden = true;
        Params->TryGetBoolField(TEXT("respect_editor_hidden"), bRespectEditorHidden);
        if (bRespectEditorHidden)
        {
            for (TActorIterator<AActor> It(World); It; ++It)
            {
                if (It->IsTemporarilyHiddenInEditor(/*bIncludeParent=*/true))
                {
                    Out.HiddenActors.AddUnique(*It);
                }
            }
        }
        const TArray<TSharedPtr<FJsonValue>>* HideActors = nullptr;
        if (Params->TryGetArrayField(TEXT("hide_actors"), HideActors) && HideActors)
        {
            for (const TSharedPtr<FJsonValue>& Value : *HideActors)
            {
                FString FindError;
                if (AActor* Actor = FindActor(World, Value->AsString(), FindError))
                {
                    Out.HiddenActors.AddUnique(Actor);
                }
                else
                {
                    Out.Warnings.Add(FindError);
                }
            }
        }
        return true;
    }

    /** A capture component that lives outside any actor, so placing the camera touches no level. */
    struct FCaptureRig
    {
        TStrongObjectPtr<USceneCaptureComponent2D> Capture;
        TStrongObjectPtr<UTextureRenderTarget2D> Target;
    };

    bool CreateCaptureRig(UWorld* World, const FCaptureRequest& Request, FCaptureRig& OutRig, MCPProtocol::FMCPError& OutError)
    {
        UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
        // Plain UNORM target: FinalColorLDR is already gamma-encoded, so the bytes read back are the image.
        Target->RenderTargetFormat = RTF_RGBA8;
        Target->ClearColor = FLinearColor::Black;
        Target->bAutoGenerateMips = false;
        Target->InitAutoFormat(Request.Width, Request.Height);
        Target->UpdateResourceImmediate(true);

        USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient);
        Capture->bCaptureEveryFrame = false;
        Capture->bCaptureOnMovement = false;
        // Keeps the view state between captures, so warmup / settle frames build up temporal AA,
        // Lumen and eye adaptation history the way consecutive game frames would.
        Capture->bAlwaysPersistRenderingState = true;
        Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
        Capture->ShowFlags = FEngineShowFlags(Request.bGameView ? ESFIM_Game : ESFIM_Editor);
        Capture->ShowFlags.SetMotionBlur(false);
        Capture->FOVAngle = Request.Fov;
        if (Request.OrthoWidth > 0.f)
        {
            Capture->ProjectionType = ECameraProjectionMode::Orthographic;
            Capture->OrthoWidth = Request.OrthoWidth;
        }
        for (const TWeakObjectPtr<AActor>& Hidden : Request.HiddenActors)
        {
            if (Hidden.IsValid())
            {
                Capture->HiddenActors.Add(Hidden.Get());
            }
        }
        Capture->TextureTarget = Target;
        Capture->SetWorldLocationAndRotation(Request.Location, Request.Rotation);
        Capture->RegisterComponentWithWorld(World);
        if (!Capture->IsRegistered())
        {
            Fail(OutError, MCPProtocol::FMCPError::InternalError, TEXT("Scene capture component failed to register with the editor world"));
            return false;
        }

        OutRig.Capture.Reset(Capture);
        OutRig.Target.Reset(Target);

        if (Request.bStreamTextures)
        {
            const float HalfFovRadians = FMath::DegreesToRadians(Request.Fov * 0.5f);
            const float FovScreenSize = Request.Width / FMath::Max(KINDA_SMALL_NUMBER, FMath::Tan(HalfFovRadians));
            IStreamingManager::Get().AddViewInformation(Request.Location, static_cast<float>(Request.Width), FovScreenSize,
                /*BoostFactor=*/1.f, /*bOverrideLocation=*/false, /*Duration=*/Request.StreamSeconds + 1.f);
            if (Request.StreamSeconds > 0.f)
            {
                IStreamingManager::Get().StreamAllResources(Request.StreamSeconds);
            }
        }
        return true;
    }

    void RenderCaptureFrame(UWorld* World, FCaptureRig& Rig)
    {
        if (Rig.Capture.IsValid())
        {
            // Pose changes and freshly spawned actors reach the render thread at end of frame;
            // push them now so this render sees them.
            World->SendAllEndOfFrameUpdates();
            Rig.Capture->CaptureScene();
        }
    }

    void DestroyCaptureRig(FCaptureRig& Rig)
    {
        if (Rig.Capture.IsValid())
        {
            Rig.Capture->UnregisterComponent();
            Rig.Capture->TextureTarget = nullptr;
        }
        Rig.Capture.Reset();
        Rig.Target.Reset();
    }

    /** Final render, read back, write the file and confirm it is on disk, project the marks. */
    TSharedPtr<FJsonObject> FinishCapture(UWorld* World, FCaptureRig& Rig, const FCaptureRequest& Request, MCPProtocol::FMCPError& OutError)
    {
        RenderCaptureFrame(World, Rig);

        FTextureRenderTargetResource* Resource = Rig.Target.IsValid() ? Rig.Target->GameThread_GetRenderTargetResource() : nullptr;
        TArray<FColor> Pixels;
        if (!Resource || !Resource->ReadPixels(Pixels) || Pixels.Num() != Request.Width * Request.Height)
        {
            Fail(OutError, MCPProtocol::FMCPError::InternalError, TEXT("Reading the capture back failed"));
            return nullptr;
        }
        for (FColor& Pixel : Pixels)
        {
            // The scene capture writes inverse opacity into alpha; a photo wants it opaque.
            Pixel.A = 255;
        }

        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Request.Path), /*Tree=*/true);
        const FImageView Image(Pixels.GetData(), Request.Width, Request.Height);
        if (!FImageUtils::SaveImageByExtension(*Request.Path, Image))
        {
            Fail(OutError, MCPProtocol::FMCPError::InternalError, FString::Printf(TEXT("Writing %s failed"), *Request.Path));
            return nullptr;
        }
        const int64 FileSize = IFileManager::Get().FileSize(*Request.Path);
        if (FileSize <= 0)
        {
            Fail(OutError, MCPProtocol::FMCPError::InternalError, FString::Printf(TEXT("%s was not written"), *Request.Path));
            return nullptr;
        }

        auto Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("ok"), true);
        Result->SetStringField(TEXT("path"), Request.Path);
        Result->SetNumberField(TEXT("file_size"), static_cast<double>(FileSize));
        Result->SetNumberField(TEXT("width"), Request.Width);
        Result->SetNumberField(TEXT("height"), Request.Height);
        Result->SetField(TEXT("location"), MakeVector3JsonArray(Request.Location));
        Result->SetField(TEXT("rotation"), MakeRotatorJsonArray(Request.Rotation));
        Result->SetNumberField(TEXT("fov"), Request.Fov);
        if (Request.OrthoWidth > 0.f)
        {
            Result->SetNumberField(TEXT("ortho_width"), Request.OrthoWidth);
        }
        Result->SetNumberField(TEXT("hidden_actor_count"), Request.HiddenActors.Num());

        if (Request.Marks.Num() > 0)
        {
            FMinimalViewInfo View;
            View.Location = Request.Location;
            View.Rotation = Request.Rotation;
            View.FOV = Request.Fov;
            View.AspectRatio = static_cast<float>(Request.Width) / static_cast<float>(Request.Height);
            View.bConstrainAspectRatio = false;
            if (Request.OrthoWidth > 0.f)
            {
                View.ProjectionMode = ECameraProjectionMode::Orthographic;
                View.OrthoWidth = Request.OrthoWidth;
            }
            FMatrix ViewMatrix, ProjectionMatrix, ViewProjectionMatrix;
            UGameplayStatics::GetViewProjectionMatrix(View, ViewMatrix, ProjectionMatrix, ViewProjectionMatrix);

            const FIntRect ViewRect(0, 0, Request.Width, Request.Height);
            TArray<TSharedPtr<FJsonValue>> MarksPx;
            for (const FCaptureMark& Mark : Request.Marks)
            {
                FVector2D Pixel;
                const bool bInFront = FSceneView::ProjectWorldToScreen(Mark.Location, ViewRect, ViewProjectionMatrix, Pixel);
                const bool bInView = bInFront && Pixel.X >= 0.0 && Pixel.Y >= 0.0 && Pixel.X < Request.Width && Pixel.Y < Request.Height;

                auto MarkJson = MakeShared<FJsonObject>();
                MarkJson->SetStringField(TEXT("name"), Mark.Name);
                MarkJson->SetNumberField(TEXT("x"), bInFront ? Pixel.X : -1.0);
                MarkJson->SetNumberField(TEXT("y"), bInFront ? Pixel.Y : -1.0);
                MarkJson->SetBoolField(TEXT("in_view"), bInView);
                MarkJson->SetBoolField(TEXT("behind_camera"), !bInFront);
                MarksPx.Add(MakeShared<FJsonValueObject>(MarkJson));
            }
            Result->SetArrayField(TEXT("marks_px"), MarksPx);
        }

        if (Request.Warnings.Num() > 0)
        {
            Result->SetArrayField(TEXT("warnings"), ToJsonStrings(Request.Warnings));
        }
        return Result;
    }

    TSharedPtr<FJsonObject> RunCaptureView(UWorld* World, const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        FCaptureRequest Request;
        if (!ParseCaptureRequest(World, Params, Request, OutError))
        {
            return nullptr;
        }
        FCaptureRig Rig;
        if (!CreateCaptureRig(World, Request, Rig, OutError))
        {
            return nullptr;
        }
        for (int32 Frame = 0; Frame < Request.Warmup; ++Frame)
        {
            RenderCaptureFrame(World, Rig);
        }
        TSharedPtr<FJsonObject> Result = FinishCapture(World, Rig, Request, OutError);
        DestroyCaptureRig(Rig);
        return Result;
    }

    // --- run_shots -------------------------------------------------------------------------------

    /**
     * A shot list worked through on the core ticker, one step per editor frame, so settle frames
     * are real frames (streaming, Lumen and TAA history advance between them) and the bridge call
     * that started it returns at once instead of running into the dispatch timeout.
     */
    struct FShotJob
    {
        enum class EPhase : uint8 { Setup, Settle, Done };

        int32 Id = 0;
        TArray<TSharedPtr<FJsonObject>> Shots;
        int32 DefaultSettleFrames = 4;
        double TimeoutSeconds = 60.0;
        bool bStopOnFail = false;

        int32 Index = 0;
        EPhase Phase = EPhase::Setup;
        int32 FramesLeft = 0;
        double ShotStarted = 0.0;
        double JobStarted = 0.0;
        double JobFinished = 0.0;
        FName ShotTag;
        FCaptureRequest Request;
        FCaptureRig Rig;
        bool bHasCamera = false;
        TArray<TSharedPtr<FJsonValue>> Results;
        TArray<TSharedPtr<FJsonValue>> PendingSetup;
    };

    TMap<int32, TSharedPtr<FShotJob>>& Jobs()
    {
        static TMap<int32, TSharedPtr<FShotJob>> Map;
        return Map;
    }

    FTSTicker::FDelegateHandle& JobTickerHandle()
    {
        static FTSTicker::FDelegateHandle Handle;
        return Handle;
    }

    /** Runs one step of a shot list (spawn/pose/override entries) and returns the error text, if any. */
    bool RunShotSetupEntries(UWorld* World, const TSharedPtr<FJsonObject>& Shot, const TCHAR* Field, FName Tag,
        TFunctionRef<TSharedPtr<FJsonObject>(UWorld*, const TSharedPtr<FJsonObject>&, FName, MCPProtocol::FMCPError&)> Run,
        TArray<TSharedPtr<FJsonValue>>& OutSetup, FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        if (!Shot->TryGetArrayField(Field, Entries) || !Entries)
        {
            return true;
        }
        for (int32 EntryIndex = 0; EntryIndex < Entries->Num(); ++EntryIndex)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            if (!(*Entries)[EntryIndex]->TryGetObject(Entry) || !Entry)
            {
                OutError = FString::Printf(TEXT("%s[%d] is not an object"), Field, EntryIndex);
                return false;
            }
            MCPProtocol::FMCPError Error;
            TSharedPtr<FJsonObject> EntryResult = Run(World, *Entry, Tag, Error);
            if (!EntryResult)
            {
                OutError = FString::Printf(TEXT("%s[%d]: %s"), Field, EntryIndex, *Error.Message);
                return false;
            }
            OutSetup.Add(MakeShared<FJsonValueObject>(EntryResult));
        }
        return true;
    }

    void FinishShot(UWorld* World, FShotJob& Job, const TSharedPtr<FJsonObject>& Capture, const FString& Error)
    {
        const TSharedPtr<FJsonObject>& Shot = Job.Shots[Job.Index];
        auto Entry = MakeShared<FJsonObject>();
        FString ShotId;
        if (!Shot->TryGetStringField(TEXT("id"), ShotId))
        {
            ShotId = FString::FromInt(Job.Index);
        }
        Entry->SetStringField(TEXT("id"), ShotId);
        Entry->SetBoolField(TEXT("ok"), Error.IsEmpty() && Capture.IsValid());
        if (Capture.IsValid())
        {
            Entry->SetObjectField(TEXT("capture"), Capture);
            Entry->SetStringField(TEXT("path"), Capture->GetStringField(TEXT("path")));
        }
        if (!Error.IsEmpty())
        {
            Entry->SetStringField(TEXT("error"), Error);
        }
        Entry->SetArrayField(TEXT("setup"), Job.PendingSetup);
        Entry->SetNumberField(TEXT("seconds"), FPlatformTime::Seconds() - Job.ShotStarted);

        DestroyCaptureRig(Job.Rig);
        if (World)
        {
            bool bKeep = false;
            Shot->TryGetBoolField(TEXT("keep"), bKeep);
            if (!bKeep)
            {
                RunClearTemp(World, Job.ShotTag);
            }
        }
        Job.Results.Add(MakeShared<FJsonValueObject>(Entry));
        Job.PendingSetup.Reset();

        ++Job.Index;
        const bool bStop = !Error.IsEmpty() && Job.bStopOnFail;
        Job.Phase = (bStop || Job.Index >= Job.Shots.Num()) ? FShotJob::EPhase::Done : FShotJob::EPhase::Setup;
        if (Job.Phase == FShotJob::EPhase::Done)
        {
            Job.JobFinished = FPlatformTime::Seconds();
        }
    }

    void StepJob(FShotJob& Job)
    {
        MCPProtocol::FMCPError WorldError;
        UWorld* World = GetEditorWorld(WorldError);
        if (!World)
        {
            FinishShot(nullptr, Job, nullptr, WorldError.Message);
            return;
        }

        if (Job.Phase == FShotJob::EPhase::Setup)
        {
            const TSharedPtr<FJsonObject>& Shot = Job.Shots[Job.Index];
            Job.ShotStarted = FPlatformTime::Seconds();
            Job.ShotTag = FName(*FString::Printf(TEXT("MCPShot_%d_%d"), Job.Id, Job.Index));
            Job.PendingSetup.Reset();
            Job.bHasCamera = false;

            FString Error;
            const bool bSetupOk =
                RunShotSetupEntries(World, Shot, TEXT("spawn"), Job.ShotTag, RunSpawnTemp, Job.PendingSetup, Error)
                && RunShotSetupEntries(World, Shot, TEXT("pose"), Job.ShotTag, RunPoseCharacter, Job.PendingSetup, Error)
                && RunShotSetupEntries(World, Shot, TEXT("override"), Job.ShotTag, RunTempOverride, Job.PendingSetup, Error);
            if (!bSetupOk)
            {
                FinishShot(World, Job, nullptr, Error);
                return;
            }

            const TSharedPtr<FJsonObject>* Camera = nullptr;
            if (Shot->TryGetObjectField(TEXT("camera"), Camera) && Camera)
            {
                // "path" may sit on the shot or inside camera.
                TSharedPtr<FJsonObject> CameraParams = MakeShared<FJsonObject>(**Camera);
                FString ShotPath;
                if (!CameraParams->HasField(TEXT("path")) && Shot->TryGetStringField(TEXT("path"), ShotPath))
                {
                    CameraParams->SetStringField(TEXT("path"), ShotPath);
                }
                Job.Request = FCaptureRequest();
                MCPProtocol::FMCPError CaptureError;
                if (!ParseCaptureRequest(World, CameraParams, Job.Request, CaptureError) || !CreateCaptureRig(World, Job.Request, Job.Rig, CaptureError))
                {
                    FinishShot(World, Job, nullptr, CaptureError.Message);
                    return;
                }
                Job.bHasCamera = true;
            }

            double Settle = Job.DefaultSettleFrames;
            Shot->TryGetNumberField(TEXT("settle_frames"), Settle);
            Job.FramesLeft = FMath::Max(0, static_cast<int32>(Settle));
            Job.Phase = FShotJob::EPhase::Settle;
            return;
        }

        if (Job.Phase == FShotJob::EPhase::Settle)
        {
            if (FPlatformTime::Seconds() - Job.ShotStarted > Job.TimeoutSeconds)
            {
                FinishShot(World, Job, nullptr, FString::Printf(TEXT("timed out after %.0f s"), Job.TimeoutSeconds));
                return;
            }
            if (Job.FramesLeft > 0)
            {
                if (Job.bHasCamera)
                {
                    RenderCaptureFrame(World, Job.Rig);
                }
                --Job.FramesLeft;
                return;
            }

            TSharedPtr<FJsonObject> Capture;
            FString Error;
            if (Job.bHasCamera)
            {
                MCPProtocol::FMCPError CaptureError;
                Capture = FinishCapture(World, Job.Rig, Job.Request, CaptureError);
                if (!Capture)
                {
                    Error = CaptureError.Message;
                }
            }
            else
            {
                // A shot with no camera only sets up and clears — still a success.
                Capture = MakeShared<FJsonObject>();
                Capture->SetBoolField(TEXT("ok"), true);
                Capture->SetStringField(TEXT("path"), FString());
            }
            FinishShot(World, Job, Capture, Error);
        }
    }

    bool TickJobs(float /*DeltaSeconds*/)
    {
        bool bAnyRunning = false;
        const double Now = FPlatformTime::Seconds();
        TArray<int32> Expired;
        for (TPair<int32, TSharedPtr<FShotJob>>& Pair : Jobs())
        {
            FShotJob& Job = *Pair.Value;
            if (Job.Phase != FShotJob::EPhase::Done)
            {
                StepJob(Job);
                bAnyRunning |= Job.Phase != FShotJob::EPhase::Done;
            }
            else if (Now - Job.JobFinished > 600.0)
            {
                Expired.Add(Pair.Key);
            }
        }
        for (const int32 Id : Expired)
        {
            Jobs().Remove(Id);
        }
        if (!bAnyRunning)
        {
            JobTickerHandle().Reset();
        }
        return bAnyRunning;
    }

    TSharedPtr<FJsonObject> MakeJobStatus(const FShotJob& Job)
    {
        auto Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("job_id"), Job.Id);
        Result->SetBoolField(TEXT("done"), Job.Phase == FShotJob::EPhase::Done);
        Result->SetNumberField(TEXT("index"), Job.Index);
        Result->SetNumberField(TEXT("total"), Job.Shots.Num());
        const double End = Job.Phase == FShotJob::EPhase::Done ? Job.JobFinished : FPlatformTime::Seconds();
        Result->SetNumberField(TEXT("elapsed_seconds"), End - Job.JobStarted);
        Result->SetArrayField(TEXT("results"), Job.Results);
        int32 Failed = 0;
        for (const TSharedPtr<FJsonValue>& Value : Job.Results)
        {
            Failed += Value->AsObject()->GetBoolField(TEXT("ok")) ? 0 : 1;
        }
        Result->SetNumberField(TEXT("failed"), Failed);
        return Result;
    }
}

// ---------------------------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------------------------

FSceneCommandHandler::~FSceneCommandHandler()
{
    if (MCPScene::PreSaveHandle().IsValid())
    {
        UPackage::PreSavePackageWithContextEvent.Remove(MCPScene::PreSaveHandle());
        MCPScene::PreSaveHandle().Reset();
    }
    if (MCPScene::JobTickerHandle().IsValid())
    {
        FTSTicker::RemoveTicker(MCPScene::JobTickerHandle());
        MCPScene::JobTickerHandle().Reset();
    }
}

void FSceneCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    using namespace MCPScene;

    if (!PreSaveHandle().IsValid())
    {
        PreSaveHandle() = UPackage::PreSavePackageWithContextEvent.AddStatic(&OnPreSavePackage);
    }

    // scene.capture_view — renders a game view to an image file in the same call and returns once
    // the file is on disk. Params: location ([x,y,z], required), look_at ([x,y,z]) or rotation
    // ([pitch,yaw,roll]), fov (deg, default 60), width/height (default 1600x900), ortho_width
    // (> 0 = orthographic top view), path (png/jpg under <Project>/Saved or CaptureOutputRoots;
    // relative = under Saved), marks ([[x,y,z]] or [{name, location}]), game_view (default true),
    // warmup (extra renders before the kept one, default 4), hide_actors (labels/paths),
    // respect_editor_hidden (default true), stream_textures (default true), stream_seconds (2).
    // Returns { ok, path, file_size, width, height, location, rotation, fov, marks_px: [{name, x,
    // y, in_view, behind_camera}], hidden_actor_count }. Touches no actor — nothing to dirty.
    Registry.Register(TEXT("scene.capture_view"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWorld* World = GetEditorWorld(OutError);
            if (!World) return nullptr;
            if (!Params.IsValid())
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams, TEXT("params object is required"));
                return nullptr;
            }
            return RunCaptureView(World, Params, OutError);
        });

    // scene.spawn_temp — places never-saved actors (RF_Transient, no external package).
    // Params: asset (static/skeletal mesh, allow-listed actor Blueprint, PointLight/SpotLight/
    // RectLight/DirectionalLight/CameraActor, or Cube), location, rotation, scale, size_m
    // ([x,y,z] metres — blockout box when asset is empty or Cube), material or color ([r,g,b(,a)]),
    // properties ({"LightComponent.Intensity": "5000"}), tag, label, snap_to_ground (+ z_offset,
    // ignore, layer). items: [...] spawns several in one call; top-level fields are their defaults.
    // Returns { actors: [{name, label, path, class, location, rotation, bounds, tags}], level_dirty,
    // dirtied_packages, map_dirty }.
    Registry.Register(TEXT("scene.spawn_temp"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWorld* World = GetEditorWorld(OutError);
            if (!World) return nullptr;
            if (!Params.IsValid())
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams, TEXT("params object is required"));
                return nullptr;
            }
            const FDirtySnapshot Before = SnapshotDirtyPackages();
            TSharedPtr<FJsonObject> Result = RunSpawnTemp(World, Params, NAME_None, OutError);
            if (Result)
            {
                WriteDirtyReport(World, Before, Result);
            }
            return Result;
        });

    // scene.clear_temp — scans the whole editor world for temp actors and removes them (all, or
    // only those carrying tag), and reverts temp_override changes (all, or that tag's).
    // Returns { removed_count, removed, reverted_overrides, level_dirty, dirtied_packages, map_dirty }.
    Registry.Register(TEXT("scene.clear_temp"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWorld* World = GetEditorWorld(OutError);
            if (!World) return nullptr;
            FString Tag;
            if (Params.IsValid())
            {
                Params->TryGetStringField(TEXT("tag"), Tag);
            }
            const FDirtySnapshot Before = SnapshotDirtyPackages();
            TSharedPtr<FJsonObject> Result = RunClearTemp(World, Tag.IsEmpty() ? NAME_None : FName(*Tag));
            WriteDirtyReport(World, Before, Result);
            return Result;
        });

    // scene.pose_character — a multi-part character as a temp actor, frozen at one frame.
    // Params: meshes ([leader, ...parts], parts use leader pose) or preset (+ preset_file), anim,
    // time or fraction, yaw_offset (deg, per-clip facing fix), weapon (static/skeletal mesh) +
    // socket + weapon_location/weapon_rotation, location, rotation, tag, label, snap_to_ground.
    // Editor animation update is switched on and the pose is evaluated before returning.
    // Returns the actor JSON + { parts, anim, time, length, facing_yaw, yaw_offset, warnings,
    // level_dirty, dirtied_packages, map_dirty }.
    Registry.Register(TEXT("scene.pose_character"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWorld* World = GetEditorWorld(OutError);
            if (!World) return nullptr;
            if (!Params.IsValid())
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams, TEXT("params object is required"));
                return nullptr;
            }
            const FDirtySnapshot Before = SnapshotDirtyPackages();
            TSharedPtr<FJsonObject> Result = RunPoseCharacter(World, Params, NAME_None, OutError);
            if (Result)
            {
                WriteDirtyReport(World, Before, Result);
            }
            return Result;
        });

    // scene.temp_override — borrows a real level actor: hide it (editor-temporary hide, never the
    // saved flag; capture_view leaves it out) and/or freeze its skeletal mesh on an animation frame.
    // Params: actor (label/name/path, required), hide (bool), anim + time/fraction (+ component),
    // tag, revert (bool). Reverted by revert, by scene.clear_temp, and automatically just before
    // the actor's package is saved. Packages this call dirtied are set clean again (reported).
    Registry.Register(TEXT("scene.temp_override"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWorld* World = GetEditorWorld(OutError);
            if (!World) return nullptr;
            if (!Params.IsValid())
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams, TEXT("params object is required"));
                return nullptr;
            }
            const FDirtySnapshot Before = SnapshotDirtyPackages();
            TSharedPtr<FJsonObject> Result = RunTempOverride(World, Params, NAME_None, OutError);
            if (Result)
            {
                WriteDirtyReport(World, Before, Result);
            }
            return Result;
        });

    // scene.get_ground_height — read-only. Params: points ([[x, y], ...], required), ignore
    // (default [volumes, temp]; listing replaces the default; "roofs" = take the lowest layer),
    // layer (0 = top, -1 = lowest), top_z / bottom_z, include_layers (default true).
    // Returns { points: [{x, y, hit, z, actor, layers: [{z, normal_z, actor, actor_path, component}]}], layer }.
    Registry.Register(TEXT("scene.get_ground_height"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWorld* World = GetEditorWorld(OutError);
            if (!World) return nullptr;
            if (!Params.IsValid())
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams, TEXT("params object is required"));
                return nullptr;
            }
            return RunGroundHeight(World, Params, OutError);
        });

    // scene.run_shots — starts a shot list and returns { job_id } at once; poll scene.shots_status.
    // Params: shots ([{id, spawn: [spawn_temp params], pose: [pose_character params], override:
    // [temp_override params], camera: {capture_view params}, path, settle_frames, keep}]),
    // settle_frames (default 4 — real editor frames rendered into the capture before the kept
    // one), timeout_s (per shot, default 60), on_fail ("continue" default, or "stop").
    // Each shot's temp actors and overrides carry a per-shot tag and are cleared after its capture
    // unless keep=true.
    Registry.Register(TEXT("scene.run_shots"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWorld* World = GetEditorWorld(OutError);
            if (!World) return nullptr;
            const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
            if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("shots"), Shots) || !Shots || Shots->Num() == 0)
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams, TEXT("shots (non-empty array) is required"));
                return nullptr;
            }

            static int32 NextJobId = 1;
            TSharedPtr<FShotJob> Job = MakeShared<FShotJob>();
            Job->Id = NextJobId++;
            for (const TSharedPtr<FJsonValue>& Value : *Shots)
            {
                const TSharedPtr<FJsonObject>* Shot = nullptr;
                if (!Value->TryGetObject(Shot) || !Shot)
                {
                    Fail(OutError, MCPProtocol::FMCPError::InvalidParams, TEXT("shots must be an array of objects"));
                    return nullptr;
                }
                Job->Shots.Add(*Shot);
            }
            double Number = 0.0;
            if (Params->TryGetNumberField(TEXT("settle_frames"), Number)) { Job->DefaultSettleFrames = FMath::Max(0, static_cast<int32>(Number)); }
            if (Params->TryGetNumberField(TEXT("timeout_s"), Number))     { Job->TimeoutSeconds = FMath::Max(1.0, Number); }
            FString OnFail;
            Params->TryGetStringField(TEXT("on_fail"), OnFail);
            Job->bStopOnFail = OnFail.Equals(TEXT("stop"), ESearchCase::IgnoreCase);
            Job->JobStarted = FPlatformTime::Seconds();

            Jobs().Add(Job->Id, Job);
            if (!JobTickerHandle().IsValid())
            {
                JobTickerHandle() = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickJobs), 0.0f);
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("job_id"), Job->Id);
            Result->SetNumberField(TEXT("total"), Job->Shots.Num());
            return Result;
        });

    // scene.shots_status — progress of a run_shots job. Params: job_id (required).
    // Returns { job_id, done, index, total, elapsed_seconds, failed, results: [{id, ok, path,
    // capture, error, setup, seconds}] }. Finished jobs are kept for 10 minutes.
    Registry.Register(TEXT("scene.shots_status"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            double JobId = 0.0;
            if (!Params.IsValid() || !Params->TryGetNumberField(TEXT("job_id"), JobId))
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams, TEXT("job_id is required"));
                return nullptr;
            }
            const TSharedPtr<FShotJob>* Job = Jobs().Find(static_cast<int32>(JobId));
            if (!Job)
            {
                Fail(OutError, MCPProtocol::FMCPError::InvalidParams, FString::Printf(TEXT("No shot job %d (finished jobs expire after 10 minutes)"), static_cast<int32>(JobId)));
                return nullptr;
            }
            return MakeJobStatus(**Job);
        });
}
