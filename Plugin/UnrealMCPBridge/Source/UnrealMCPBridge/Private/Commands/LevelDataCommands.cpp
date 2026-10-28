#include "Commands/LevelDataCommands.h"
#include "Commands/SceneTools.h"
#include "Commands/CommandJsonHelpers.h"
#include "Commands/CommandReflectionHelpers.h"
#include "MCPCommandRegistry.h"
#include "MCPProtocol.h"
#include "Dom/JsonObject.h"

#include "Editor.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/Blueprint.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Volume.h"
#include "Components/SplineComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/BrushComponent.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectHash.h"
#include "UObject/Package.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "NavigationSystemTypes.h"
#include "AbstractNavData.h"

namespace MCPLevelData
{
    using namespace UnrealMCPBridge::Scene;
    using UnrealMCPBridge::Json::RequireString;
    using UnrealMCPBridge::Json::ReadVector3;
    using UnrealMCPBridge::Json::ReadRotator;
    using UnrealMCPBridge::Json::MakeVector3JsonArray;
    using UnrealMCPBridge::Json::MakeRotatorJsonArray;
    namespace Reflection = UnrealMCPBridge::Reflection;

    void Fail(MCPProtocol::FMCPError& OutError, const FString& Message, int32 Code = MCPProtocol::FMCPError::InvalidParams)
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

    /** A JSON value as ImportText: strings as-is, numbers and bools in their text form. */
    bool JsonToImportText(const TSharedPtr<FJsonValue>& Value, FString& OutText)
    {
        if (Value->Type == EJson::Boolean)
        {
            OutText = Value->AsBool() ? TEXT("True") : TEXT("False");
            return true;
        }
        if (Value->Type == EJson::Number)
        {
            OutText = FString::SanitizeFloat(Value->AsNumber(), /*InMinFractionalDigits=*/0);
            return true;
        }
        return Value->TryGetString(OutText);
    }

    /** True when Class or one of its parents is called Name (Blueprint classes match with or without _C). */
    bool ClassNameMatches(const UClass* Class, const FString& Name, bool bExact)
    {
        for (const UClass* Current = Class; Current; Current = Current->GetSuperClass())
        {
            const FString CurrentName = Current->GetName();
            if (CurrentName.Equals(Name, ESearchCase::IgnoreCase) || CurrentName.Equals(Name + TEXT("_C"), ESearchCase::IgnoreCase)
                || Current->GetPathName().Equals(Name, ESearchCase::IgnoreCase))
            {
                return true;
            }
            if (bExact)
            {
                return false;
            }
        }
        return false;
    }

    /** class param -> UClass: /Script path, Blueprint asset or class path, or a native short name. */
    UClass* ResolveActorClass(const FString& ClassName, FString& OutError)
    {
        UClass* Class = nullptr;
        if (ClassName.StartsWith(TEXT("/")))
        {
            UObject* Object = Reflection::ResolveObject(ClassName);
            if (const UBlueprint* Blueprint = Cast<UBlueprint>(Object))
            {
                Class = Blueprint->GeneratedClass;
            }
            else
            {
                Class = Cast<UClass>(Object);
            }
            if (!Class && !ClassName.EndsWith(TEXT("_C")))
            {
                Class = Cast<UClass>(Reflection::ResolveObject(ClassName + TEXT("_C")));
            }
        }
        else
        {
            Class = UClass::TryFindTypeSlow<UClass>(ClassName, EFindFirstObjectOptions::NativeFirst);
        }

        if (!Class)
        {
            OutError = FString::Printf(TEXT("Could not resolve class '%s' — give a native name (MonsterSpawnPoint) or a Blueprint path"), *ClassName);
            return nullptr;
        }
        if (!Class->IsChildOf<AActor>())
        {
            OutError = FString::Printf(TEXT("%s is not an actor class"), *Class->GetPathName());
            return nullptr;
        }
        if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_NotPlaceable | CLASS_Deprecated))
        {
            OutError = FString::Printf(TEXT("%s is abstract or not placeable — place one of its subclasses (e.g. a TriggerVolume_* class or a Blueprint child)"), *Class->GetPathName());
            return nullptr;
        }
        return Class;
    }

    /** File the package will be saved to, or empty. */
    FString PackageFilename(const UPackage* Package)
    {
        FString Filename;
        const FString Extension = Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
        if (FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename, Extension))
        {
            return FPaths::ConvertRelativePathToFull(Filename);
        }
        return FString();
    }

    TSharedPtr<FJsonObject> MakePlacedActorJson(AActor* Actor)
    {
        auto Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("object_path"), Actor->GetPathName());
        Obj->SetStringField(TEXT("name"), Actor->GetName());
        Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
        Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
        const FName Folder = Actor->GetFolderPath();
        Obj->SetStringField(TEXT("folder"), Folder.IsNone() ? FString() : Folder.ToString());
        Obj->SetField(TEXT("location"), MakeVector3JsonArray(Actor->GetActorLocation()));
        Obj->SetField(TEXT("rotation"), MakeRotatorJsonArray(Actor->GetActorRotation()));
        if (const UPackage* External = Actor->GetExternalPackage())
        {
            Obj->SetStringField(TEXT("external_package"), External->GetName());
            Obj->SetStringField(TEXT("file"), PackageFilename(External));
        }
        return Obj;
    }

    /** Reads the property paths in Names off Actor into Obj["properties"]; unknown paths report their error. */
    void AddPropertyValues(AActor* Actor, const TArray<FString>& Names, const TSharedPtr<FJsonObject>& Obj)
    {
        if (Names.Num() == 0)
        {
            return;
        }
        auto Values = MakeShared<FJsonObject>();
        for (const FString& Name : Names)
        {
            Reflection::FResolvedProperty Resolved;
            FString Error;
            if (Reflection::ResolvePropertyPath(Actor, Name, Resolved, Error))
            {
                Values->SetStringField(Name, Reflection::ExportValue(Resolved.Property, Resolved.Address, Resolved.Owner));
            }
            else
            {
                Values->SetStringField(Name, FString::Printf(TEXT("<%s>"), *Error));
            }
        }
        Obj->SetObjectField(TEXT("properties"), Values);
    }

    TArray<FString> ReadStringArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field)
    {
        TArray<FString> Out;
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (Params->TryGetArrayField(Field, Values) && Values)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Values)
            {
                Out.Add(Value->AsString());
            }
        }
        return Out;
    }

    /** One planned property write: what it is now and what it would read back as. */
    struct FPropertyPlan
    {
        TWeakObjectPtr<AActor> Actor;
        FString Property;
        FString Text;
        FString OldValue;
        FString NewValue;
    };

    /** Validates every write before any is made, so a dry run and a failed apply leave the level untouched. */
    bool PlanPropertyWrites(AActor* Actor, const TSharedPtr<FJsonObject>& Properties, TArray<FPropertyPlan>& OutPlans, FString& OutError)
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
        {
            FPropertyPlan Plan;
            Plan.Actor = Actor;
            Plan.Property = Pair.Key;
            if (!JsonToImportText(Pair.Value, Plan.Text))
            {
                OutError = FString::Printf(TEXT("%s.%s: give the value as ImportText (a string), a number or a bool"), *Actor->GetActorLabel(), *Pair.Key);
                return false;
            }

            Reflection::FResolvedProperty Resolved;
            FString Error;
            if (!Reflection::ResolvePropertyPath(Actor, Pair.Key, Resolved, Error)
                || !Reflection::PreviewImport(Resolved.Property, Plan.Text, Resolved.Owner, Plan.NewValue, Error))
            {
                OutError = FString::Printf(TEXT("%s.%s: %s"), *Actor->GetActorLabel(), *Pair.Key, *Error);
                return false;
            }
            Plan.OldValue = Reflection::ExportValue(Resolved.Property, Resolved.Address, Resolved.Owner);
            OutPlans.Add(Plan);
        }
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> MakePlanDiff(const TArray<FPropertyPlan>& Plans)
    {
        TArray<TSharedPtr<FJsonValue>> Diff;
        for (const FPropertyPlan& Plan : Plans)
        {
            auto Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("actor"), Plan.Actor.IsValid() ? Plan.Actor->GetActorLabel() : FString());
            Entry->SetStringField(TEXT("actor_path"), Plan.Actor.IsValid() ? Plan.Actor->GetPathName() : FString());
            Entry->SetStringField(TEXT("property"), Plan.Property);
            Entry->SetStringField(TEXT("old"), Plan.OldValue);
            Entry->SetStringField(TEXT("new"), Plan.NewValue);
            Entry->SetBoolField(TEXT("changed"), !Plan.OldValue.Equals(Plan.NewValue, ESearchCase::CaseSensitive));
            Diff.Add(MakeShared<FJsonValueObject>(Entry));
        }
        return Diff;
    }

    /** Applies validated plans. Only fails if the world changed between planning and applying. */
    bool ApplyPlans(const TArray<FPropertyPlan>& Plans, FString& OutError)
    {
        for (const FPropertyPlan& Plan : Plans)
        {
            AActor* Actor = Plan.Actor.Get();
            FString OldValue, NewValue;
            if (!Actor || !Reflection::SetPropertyFromText(Actor, Plan.Property, Plan.Text, OldValue, NewValue, OutError))
            {
                return false;
            }
        }
        return true;
    }

    /** The CDO-based dry run for actor_spawn: same parse rules, no actor. */
    bool PlanDefaultsWrites(UClass* Class, const TSharedPtr<FJsonObject>& Properties, TArray<TSharedPtr<FJsonValue>>& OutDiff, FString& OutError)
    {
        UObject* Defaults = Class->GetDefaultObject();
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
        {
            FString Text, NewValue, Error;
            Reflection::FResolvedProperty Resolved;
            if (!JsonToImportText(Pair.Value, Text)
                || !Reflection::ResolvePropertyPath(Defaults, Pair.Key, Resolved, Error)
                || !Reflection::PreviewImport(Resolved.Property, Text, Resolved.Owner, NewValue, Error))
            {
                OutError = FString::Printf(TEXT("properties.%s: %s"), *Pair.Key, Error.IsEmpty() ? TEXT("value must be a string, number or bool") : *Error);
                return false;
            }
            auto Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("property"), Pair.Key);
            Entry->SetStringField(TEXT("default"), Reflection::ExportValue(Resolved.Property, Resolved.Address, Resolved.Owner));
            Entry->SetStringField(TEXT("new"), NewValue);
            OutDiff.Add(MakeShared<FJsonValueObject>(Entry));
        }
        return true;
    }

    /** Navigation data for queries, avoiding the null-in-World-Partition and abstract-nav traps. */
    const ANavigationData* FindNavData(UWorld* World, FString& OutError)
    {
        UNavigationSystemV1* NavSystem = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
        if (!NavSystem)
        {
            OutError = TEXT("This world has no navigation system");
            return nullptr;
        }
        // The argument-less GetDefaultNavDataInstance never resolves lazily, and in a World
        // Partition map the navmesh streams in as an external actor — so it can be null while a
        // navmesh exists. DontCreate resolves without spawning a RecastNavMesh in the editor.
        const ANavigationData* NavData = NavSystem->GetDefaultNavDataInstance(FNavigationSystem::DontCreate);
        if (!NavData || NavData->IsA<AAbstractNavData>())
        {
            NavData = NavSystem->GetNavDataForProps(FNavAgentProperties::DefaultProperties);
        }
        // AAbstractNavData answers every query with an empty success — worse than none.
        if (!NavData || NavData->IsA<AAbstractNavData>())
        {
            OutError = TEXT("No navmesh in this world (is it built / loaded?)");
            return nullptr;
        }
        return NavData;
    }

    UWorld* NavWorld(const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        bool bIsPie = false;
        return UnrealMCPBridge::Scene::ResolveWorld(Params, UnrealMCPBridge::Scene::EDefaultWorld::Editor, bIsPie, OutError);
    }

    bool ReadPoint(const TSharedPtr<FJsonValue>& Value, FVector& Out)
    {
        const TArray<TSharedPtr<FJsonValue>>* XYZ = nullptr;
        if (!Value.IsValid() || !Value->TryGetArray(XYZ) || !XYZ || XYZ->Num() != 3)
        {
            return false;
        }
        Out = FVector((*XYZ)[0]->AsNumber(), (*XYZ)[1]->AsNumber(), (*XYZ)[2]->AsNumber());
        return true;
    }

    template <typename ComponentType>
    ComponentType* FindNamedComponent(AActor* Actor, const FString& Name)
    {
        TInlineComponentArray<ComponentType*> Components(Actor);
        for (ComponentType* Component : Components)
        {
            if (Name.IsEmpty() || Component->GetName().Equals(Name, ESearchCase::IgnoreCase))
            {
                return Component;
            }
        }
        return nullptr;
    }

    AActor* RequireActor(UWorld* World, const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError)
    {
        FString Name, Error;
        if (!RequireString(Params, TEXT("actor"), Name, OutError))
        {
            return nullptr;
        }
        AActor* Actor = FindActor(World, Name, Error);
        if (!Actor)
        {
            Fail(OutError, Error);
        }
        return Actor;
    }
}

void FLevelDataCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    using namespace MCPLevelData;

    // level.query_actors — read-only. Params (all optional, combined with AND): class (name incl.
    // parents, Blueprint short name or path), exact_class, folder (prefix), label (substring or
    // wildcard with *), tag, sphere {center, radius} (actor location within), box {min, max},
    // include_temp (default false), properties ([property paths] to read), include_bounds, max (500).
    // Returns { actors: [{object_path, name, label, class, folder, location, rotation,
    // external_package?, file?, distance?, bounds?, properties?}], total, truncated }.
    Registry.Register(TEXT("level.query_actors"),
        [](const TSharedPtr<FJsonObject>& InParams, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWorld* World = GetEditorWorld(OutError);
            if (!World) return nullptr;
            const TSharedPtr<FJsonObject> Params = InParams.IsValid() ? InParams : MakeShared<FJsonObject>();

            FString ClassName, Folder, Label, Tag;
            bool bExactClass = false, bIncludeTemp = false, bIncludeBounds = false;
            Params->TryGetStringField(TEXT("class"), ClassName);
            Params->TryGetStringField(TEXT("folder"), Folder);
            Params->TryGetStringField(TEXT("label"), Label);
            Params->TryGetStringField(TEXT("tag"), Tag);
            Params->TryGetBoolField(TEXT("exact_class"), bExactClass);
            Params->TryGetBoolField(TEXT("include_temp"), bIncludeTemp);
            Params->TryGetBoolField(TEXT("include_bounds"), bIncludeBounds);
            const TArray<FString> PropertyNames = ReadStringArray(Params, TEXT("properties"));
            double MaxCount = 500.0;
            Params->TryGetNumberField(TEXT("max"), MaxCount);

            bool bSphere = false;
            FVector SphereCenter = FVector::ZeroVector;
            double SphereRadius = 0.0;
            const TSharedPtr<FJsonObject>* Sphere = nullptr;
            if (Params->TryGetObjectField(TEXT("sphere"), Sphere) && Sphere)
            {
                if (!ReadVector3(*Sphere, TEXT("center"), SphereCenter, OutError)) return nullptr;
                if (!(*Sphere)->TryGetNumberField(TEXT("radius"), SphereRadius))
                {
                    Fail(OutError, TEXT("sphere.radius is required"));
                    return nullptr;
                }
                bSphere = true;
            }
            bool bBox = false;
            FVector BoxMin = FVector::ZeroVector, BoxMax = FVector::ZeroVector;
            const TSharedPtr<FJsonObject>* BoxObj = nullptr;
            if (Params->TryGetObjectField(TEXT("box"), BoxObj) && BoxObj)
            {
                if (!(*BoxObj)->HasField(TEXT("min")) || !(*BoxObj)->HasField(TEXT("max")))
                {
                    Fail(OutError, TEXT("box needs min and max"));
                    return nullptr;
                }
                if (!ReadVector3(*BoxObj, TEXT("min"), BoxMin, OutError) || !ReadVector3(*BoxObj, TEXT("max"), BoxMax, OutError)) return nullptr;
                bBox = true;
            }
            const FBox Box(BoxMin.ComponentMin(BoxMax), BoxMin.ComponentMax(BoxMax));
            const bool bLabelWildcard = Label.Contains(TEXT("*")) || Label.Contains(TEXT("?"));

            struct FHit { AActor* Actor; double Distance; };
            TArray<FHit> Hits;
            for (TActorIterator<AActor> It(World); It; ++It)
            {
                AActor* Actor = *It;
                if (!bIncludeTemp && Actor->ActorHasTag(TempActorTag)) continue;
                if (!ClassName.IsEmpty() && !ClassNameMatches(Actor->GetClass(), ClassName, bExactClass)) continue;
                if (!Folder.IsEmpty() && !Actor->GetFolderPath().ToString().StartsWith(Folder, ESearchCase::IgnoreCase)) continue;
                if (!Label.IsEmpty())
                {
                    const FString ActorLabel = Actor->GetActorLabel();
                    if (bLabelWildcard ? !ActorLabel.MatchesWildcard(Label, ESearchCase::IgnoreCase) : !ActorLabel.Contains(Label, ESearchCase::IgnoreCase)) continue;
                }
                if (!Tag.IsEmpty() && !Actor->ActorHasTag(FName(*Tag))) continue;

                const FVector Location = Actor->GetActorLocation();
                const double Distance = bSphere ? FVector::Dist(Location, SphereCenter) : 0.0;
                if (bSphere && Distance > SphereRadius) continue;
                if (bBox && !Box.IsInsideOrOn(Location)) continue;
                Hits.Add({ Actor, Distance });
            }
            if (bSphere)
            {
                Hits.Sort([](const FHit& A, const FHit& B) { return A.Distance < B.Distance; });
            }

            const int32 Max = FMath::Max(1, static_cast<int32>(MaxCount));
            TArray<TSharedPtr<FJsonValue>> Actors;
            for (int32 Index = 0; Index < Hits.Num() && Index < Max; ++Index)
            {
                TSharedPtr<FJsonObject> Entry = MakePlacedActorJson(Hits[Index].Actor);
                if (bSphere)
                {
                    Entry->SetNumberField(TEXT("distance"), Hits[Index].Distance);
                }
                if (bIncludeBounds)
                {
                    Entry->SetObjectField(TEXT("bounds"), MakeBoundsJson(Hits[Index].Actor));
                }
                AddPropertyValues(Hits[Index].Actor, PropertyNames, Entry);
                Actors.Add(MakeShared<FJsonValueObject>(Entry));
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("actors"), Actors);
            Result->SetNumberField(TEXT("total"), Hits.Num());
            Result->SetBoolField(TEXT("truncated"), Hits.Num() > Max);
            return Result;
        });

    // level.actor_spawn — places SAVED actors (the level becomes dirty; nothing is saved).
    // Params: class (native name / Blueprint path) or asset (mesh etc., spawned the way a drag
    // from the Content Browser would), location, rotation, scale, label, folder, properties
    // ({path: ImportText/number/bool}), snap_to_ground (+ z_offset, ignore, layer), dry_run.
    // items: [...] places several in one undo step; top-level fields are their defaults.
    // dry_run validates class and properties against the class defaults and spawns nothing.
    // Returns { actors: [{object_path, label, class, folder, location, rotation, external_package,
    // file, bounds}], level_dirty, dirtied_packages, map_dirty } or { dry_run, plan }.
    Registry.Register(TEXT("level.actor_spawn"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWorld* World = GetEditorWorld(OutError);
            if (!World) return nullptr;
            if (!Params.IsValid())
            {
                Fail(OutError, TEXT("params object is required"));
                return nullptr;
            }
            UEditorActorSubsystem* ActorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
            if (!ActorSubsystem)
            {
                Fail(OutError, TEXT("UEditorActorSubsystem is not available"), MCPProtocol::FMCPError::InternalError);
                return nullptr;
            }

            // Items inherit the top-level fields.
            TArray<TSharedPtr<FJsonObject>> Items;
            const TArray<TSharedPtr<FJsonValue>>* ItemValues = nullptr;
            if (Params->TryGetArrayField(TEXT("items"), ItemValues) && ItemValues)
            {
                for (const TSharedPtr<FJsonValue>& Value : *ItemValues)
                {
                    const TSharedPtr<FJsonObject>* ItemObject = nullptr;
                    if (!Value->TryGetObject(ItemObject) || !ItemObject)
                    {
                        Fail(OutError, TEXT("items must be an array of objects"));
                        return nullptr;
                    }
                    TSharedPtr<FJsonObject> Merged = MakeShared<FJsonObject>();
                    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Params->Values)
                    {
                        if (Pair.Key != TEXT("items")) Merged->SetField(Pair.Key, Pair.Value);
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

            // Resolve and validate everything first: a failure must not leave half a batch placed.
            struct FSpawnPlan { UClass* Class = nullptr; UObject* Asset = nullptr; FVector Location = FVector::ZeroVector; FRotator Rotation = FRotator::ZeroRotator; FVector Scale = FVector::OneVector; };
            TArray<FSpawnPlan> Plans;
            TArray<TSharedPtr<FJsonValue>> DryRunPlan;
            for (int32 Index = 0; Index < Items.Num(); ++Index)
            {
                const TSharedPtr<FJsonObject>& Item = Items[Index];
                FSpawnPlan Plan;
                FString ClassName, AssetPath, Error;
                Item->TryGetStringField(TEXT("class"), ClassName);
                Item->TryGetStringField(TEXT("asset"), AssetPath);
                if (!ClassName.IsEmpty())
                {
                    Plan.Class = ResolveActorClass(ClassName, Error);
                }
                else if (!AssetPath.IsEmpty())
                {
                    Plan.Asset = Reflection::ResolveObject(AssetPath);
                    if (!Plan.Asset) Error = FString::Printf(TEXT("Could not load asset %s"), *AssetPath);
                }
                else
                {
                    Error = TEXT("class or asset is required");
                }
                if (Error.IsEmpty())
                {
                    if (!ReadVector3(Item, TEXT("location"), Plan.Location, OutError)
                        || !ReadRotator(Item, TEXT("rotation"), Plan.Rotation, OutError)
                        || !ReadVector3(Item, TEXT("scale"), Plan.Scale, OutError))
                    {
                        OutError.Message = FString::Printf(TEXT("items[%d]: %s"), Index, *OutError.Message);
                        return nullptr;
                    }
                }

                const TSharedPtr<FJsonObject>* Properties = nullptr;
                TArray<TSharedPtr<FJsonValue>> PropertyDiff;
                if (Error.IsEmpty() && Plan.Class && Item->TryGetObjectField(TEXT("properties"), Properties) && Properties)
                {
                    PlanDefaultsWrites(Plan.Class, *Properties, PropertyDiff, Error);
                }
                if (!Error.IsEmpty())
                {
                    Fail(OutError, FString::Printf(TEXT("items[%d]: %s"), Index, *Error));
                    return nullptr;
                }

                auto Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("class"), Plan.Class ? Plan.Class->GetPathName() : FString());
                Entry->SetStringField(TEXT("asset"), Plan.Asset ? Plan.Asset->GetPathName() : FString());
                Entry->SetField(TEXT("location"), MakeVector3JsonArray(Plan.Location));
                Entry->SetArrayField(TEXT("properties"), PropertyDiff);
                DryRunPlan.Add(MakeShared<FJsonValueObject>(Entry));
                Plans.Add(Plan);
            }

            bool bDryRun = false;
            if (Params->TryGetBoolField(TEXT("dry_run"), bDryRun) && bDryRun)
            {
                auto Result = MakeShared<FJsonObject>();
                Result->SetBoolField(TEXT("dry_run"), true);
                Result->SetArrayField(TEXT("plan"), DryRunPlan);
                return Result;
            }

            const FDirtySnapshot Before = SnapshotDirtyPackages();
            FScopedTransaction Transaction(NSLOCTEXT("UnrealMCPBridge", "ActorSpawn", "MCP: Spawn Actors"));
            TArray<AActor*> Spawned;
            TArray<TSharedPtr<FJsonValue>> Actors;
            FString Error;
            for (int32 Index = 0; Index < Items.Num() && Error.IsEmpty(); ++Index)
            {
                const TSharedPtr<FJsonObject>& Item = Items[Index];
                const FSpawnPlan& Plan = Plans[Index];
                AActor* Actor = Plan.Class
                    ? ActorSubsystem->SpawnActorFromClass(Plan.Class, Plan.Location, Plan.Rotation)
                    : ActorSubsystem->SpawnActorFromObject(Plan.Asset, Plan.Location, Plan.Rotation);
                if (!Actor)
                {
                    Error = FString::Printf(TEXT("items[%d]: spawning failed"), Index);
                    break;
                }
                Spawned.Add(Actor);
                Actor->SetActorScale3D(Plan.Scale);

                FString Label, Folder;
                if (Item->TryGetStringField(TEXT("label"), Label) && !Label.IsEmpty()) Actor->SetActorLabel(Label);
                if (Item->TryGetStringField(TEXT("folder"), Folder) && !Folder.IsEmpty()) Actor->SetFolderPath(FName(*Folder));

                const TSharedPtr<FJsonObject>* Properties = nullptr;
                if (Item->TryGetObjectField(TEXT("properties"), Properties) && Properties)
                {
                    TArray<FPropertyPlan> Writes;
                    if (!PlanPropertyWrites(Actor, *Properties, Writes, Error) || !ApplyPlans(Writes, Error))
                    {
                        Error = FString::Printf(TEXT("items[%d]: %s"), Index, *Error);
                        break;
                    }
                }

                TArray<FString> Warnings;
                bool bSnap = false;
                if (Item->TryGetBoolField(TEXT("snap_to_ground"), bSnap) && bSnap)
                {
                    FGroundQuery Query;
                    int32 Layer = 0;
                    ReadGroundQuery(Item, Query, Layer);
                    double ZOffset = 0.0;
                    Item->TryGetNumberField(TEXT("z_offset"), ZOffset);
                    FString Message;
                    if (!SnapActorToGround(Actor, Query, Layer, ZOffset, Message)) Warnings.Add(Message);
                }

                TSharedPtr<FJsonObject> Entry = MakePlacedActorJson(Actor);
                Entry->SetObjectField(TEXT("bounds"), MakeBoundsJson(Actor));
                if (Warnings.Num() > 0) Entry->SetArrayField(TEXT("warnings"), ToJsonStrings(Warnings));
                Actors.Add(MakeShared<FJsonValueObject>(Entry));
            }

            if (!Error.IsEmpty())
            {
                // Roll the whole batch back rather than leave part of it in the level.
                Transaction.Cancel();
                for (AActor* Actor : Spawned)
                {
                    World->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/true);
                }
                Fail(OutError, Error);
                return nullptr;
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("actors"), Actors);
            WriteDirtyReport(World, Before, Result);
            return Result;
        });

    // level.actors_set_properties — many actors x many properties as one undo step.
    // Params: changes ([{actor, properties: {path: value}}]) or actors ([names]) + properties
    // (the same writes for each), dry_run (default false). Values are ImportText strings (read a
    // property with level.query_actors / object.get_property first), numbers or bools.
    // Every write is parsed before any is made. Returns { dry_run, diff: [{actor, actor_path,
    // property, old, new, changed}], changed_count } + the dirty report when applied.
    Registry.Register(TEXT("level.actors_set_properties"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWorld* World = GetEditorWorld(OutError);
            if (!World) return nullptr;
            if (!Params.IsValid())
            {
                Fail(OutError, TEXT("params object is required"));
                return nullptr;
            }

            TArray<TPair<AActor*, TSharedPtr<FJsonObject>>> Targets;
            FString Error;
            const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
            const TSharedPtr<FJsonObject>* SharedProperties = nullptr;
            if (Params->TryGetArrayField(TEXT("changes"), Changes) && Changes)
            {
                for (const TSharedPtr<FJsonValue>& Value : *Changes)
                {
                    const TSharedPtr<FJsonObject>* Change = nullptr;
                    const TSharedPtr<FJsonObject>* Properties = nullptr;
                    FString ActorName;
                    if (!Value->TryGetObject(Change) || !Change || !(*Change)->TryGetStringField(TEXT("actor"), ActorName)
                        || !(*Change)->TryGetObjectField(TEXT("properties"), Properties) || !Properties)
                    {
                        Fail(OutError, TEXT("changes entries are {actor, properties: {path: value}}"));
                        return nullptr;
                    }
                    AActor* Actor = FindActor(World, ActorName, Error);
                    if (!Actor)
                    {
                        Fail(OutError, Error);
                        return nullptr;
                    }
                    Targets.Add({ Actor, *Properties });
                }
            }
            else if (Params->TryGetObjectField(TEXT("properties"), SharedProperties) && SharedProperties)
            {
                for (const FString& ActorName : ReadStringArray(Params, TEXT("actors")))
                {
                    AActor* Actor = FindActor(World, ActorName, Error);
                    if (!Actor)
                    {
                        Fail(OutError, Error);
                        return nullptr;
                    }
                    Targets.Add({ Actor, *SharedProperties });
                }
            }
            if (Targets.Num() == 0)
            {
                Fail(OutError, TEXT("give changes: [{actor, properties}] or actors: [...] + properties: {...}"));
                return nullptr;
            }

            TArray<FPropertyPlan> Plans;
            for (const TPair<AActor*, TSharedPtr<FJsonObject>>& Target : Targets)
            {
                if (Target.Key->ActorHasTag(TempActorTag))
                {
                    Fail(OutError, FString::Printf(TEXT("%s is a temp actor — this tool writes saved level data"), *Target.Key->GetActorLabel()));
                    return nullptr;
                }
                if (!PlanPropertyWrites(Target.Key, Target.Value, Plans, Error))
                {
                    Fail(OutError, Error);
                    return nullptr;
                }
            }

            int32 ChangedCount = 0;
            for (const FPropertyPlan& Plan : Plans)
            {
                ChangedCount += Plan.OldValue.Equals(Plan.NewValue, ESearchCase::CaseSensitive) ? 0 : 1;
            }

            bool bDryRun = false;
            Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("dry_run"), bDryRun);
            Result->SetNumberField(TEXT("changed_count"), ChangedCount);
            if (bDryRun)
            {
                Result->SetArrayField(TEXT("diff"), MakePlanDiff(Plans));
                return Result;
            }

            const FDirtySnapshot Before = SnapshotDirtyPackages();
            {
                FScopedTransaction Transaction(NSLOCTEXT("UnrealMCPBridge", "SetProperties", "MCP: Set Actor Properties"));
                if (!ApplyPlans(Plans, Error))
                {
                    Transaction.Cancel();
                    Fail(OutError, Error, MCPProtocol::FMCPError::InternalError);
                    return nullptr;
                }
            }
            // Re-read so "new" is what the actor holds after PostEditChange (construction scripts may clamp).
            for (FPropertyPlan& Plan : Plans)
            {
                Reflection::FResolvedProperty Resolved;
                FString ReadError;
                if (Plan.Actor.IsValid() && Reflection::ResolvePropertyPath(Plan.Actor.Get(), Plan.Property, Resolved, ReadError))
                {
                    Plan.NewValue = Reflection::ExportValue(Resolved.Property, Resolved.Address, Resolved.Owner);
                }
            }
            Result->SetArrayField(TEXT("diff"), MakePlanDiff(Plans));
            WriteDirtyReport(World, Before, Result);
            return Result;
        });

    // level.actor_add_instanced — adds an instanced subobject to an actor's array property, e.g.
    // AIFSRTriggerVolume.TriggerActions. Params: actor, property (array path), class (subclass of
    // the array's element class; native name or Blueprint path), properties ({path: value} on the
    // new object), index (insert position, default end), dry_run.
    // Returns { actor, property, index, object_path, class, count, level_dirty, ... } or the dry-run plan.
    Registry.Register(TEXT("level.actor_add_instanced"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWorld* World = GetEditorWorld(OutError);
            if (!World) return nullptr;
            if (!Params.IsValid())
            {
                Fail(OutError, TEXT("params object is required"));
                return nullptr;
            }
            AActor* Actor = RequireActor(World, Params, OutError);
            if (!Actor) return nullptr;

            FString PropertyPath, ClassName, Error;
            if (!RequireString(Params, TEXT("property"), PropertyPath, OutError)) return nullptr;
            if (!RequireString(Params, TEXT("class"), ClassName, OutError)) return nullptr;

            Reflection::FResolvedProperty Resolved;
            if (!Reflection::ResolvePropertyPath(Actor, PropertyPath, Resolved, Error))
            {
                Fail(OutError, Error);
                return nullptr;
            }
            FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Resolved.Property);
            FObjectProperty* InnerProperty = ArrayProperty ? CastField<FObjectProperty>(ArrayProperty->Inner) : nullptr;
            if (!InnerProperty)
            {
                Fail(OutError, FString::Printf(TEXT("%s is a %s, not an array of object references"), *PropertyPath, *Reflection::TypeName(Resolved.Property)));
                return nullptr;
            }

            UClass* NewClass = nullptr;
            if (ClassName.StartsWith(TEXT("/")))
            {
                UObject* Object = Reflection::ResolveObject(ClassName);
                const UBlueprint* Blueprint = Cast<UBlueprint>(Object);
                NewClass = Blueprint ? Blueprint->GeneratedClass.Get() : Cast<UClass>(Object);
            }
            else
            {
                NewClass = UClass::TryFindTypeSlow<UClass>(ClassName, EFindFirstObjectOptions::NativeFirst);
            }
            if (!NewClass || !NewClass->IsChildOf(InnerProperty->PropertyClass) || NewClass->HasAnyClassFlags(CLASS_Abstract))
            {
                Fail(OutError, FString::Printf(TEXT("class '%s' must be a concrete subclass of %s"), *ClassName, *InnerProperty->PropertyClass->GetName()));
                return nullptr;
            }

            const TSharedPtr<FJsonObject>* Properties = nullptr;
            const bool bHasProperties = Params->TryGetObjectField(TEXT("properties"), Properties) && Properties;
            TArray<TSharedPtr<FJsonValue>> PropertyDiff;
            if (bHasProperties && !PlanDefaultsWrites(NewClass, *Properties, PropertyDiff, Error))
            {
                Fail(OutError, Error);
                return nullptr;
            }

            UObject* Owner = Resolved.Owner;
            FScriptArrayHelper ArrayHelper(ArrayProperty, Resolved.Address);
            double IndexNumber = ArrayHelper.Num();
            Params->TryGetNumberField(TEXT("index"), IndexNumber);
            const int32 InsertIndex = FMath::Clamp(static_cast<int32>(IndexNumber), 0, ArrayHelper.Num());

            bool bDryRun = false;
            if (Params->TryGetBoolField(TEXT("dry_run"), bDryRun) && bDryRun)
            {
                auto Plan = MakeShared<FJsonObject>();
                Plan->SetBoolField(TEXT("dry_run"), true);
                Plan->SetStringField(TEXT("actor"), Actor->GetPathName());
                Plan->SetStringField(TEXT("property"), PropertyPath);
                Plan->SetStringField(TEXT("class"), NewClass->GetPathName());
                Plan->SetNumberField(TEXT("index"), InsertIndex);
                Plan->SetNumberField(TEXT("count_before"), ArrayHelper.Num());
                Plan->SetArrayField(TEXT("properties"), PropertyDiff);
                return Plan;
            }

            const FDirtySnapshot Before = SnapshotDirtyPackages();
            FScopedTransaction Transaction(NSLOCTEXT("UnrealMCPBridge", "AddInstanced", "MCP: Add Instanced Object"));
            Owner->Modify();
            Owner->PreEditChange(ArrayProperty);

            UObject* NewObject_ = NewObject<UObject>(Owner, NewClass, NAME_None, RF_Transactional | Owner->GetMaskedFlags(RF_PropagateToSubObjects));
            if (bHasProperties)
            {
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Properties)->Values)
                {
                    FString Text, OldValue, NewValue;
                    JsonToImportText(Pair.Value, Text);
                    if (!Reflection::SetPropertyFromText(NewObject_, Pair.Key, Text, OldValue, NewValue, Error))
                    {
                        FPropertyChangedEvent Unchanged(ArrayProperty, EPropertyChangeType::ValueSet);
                        Owner->PostEditChangeProperty(Unchanged);
                        Transaction.Cancel();
                        Fail(OutError, FString::Printf(TEXT("properties.%s: %s"), *Pair.Key, *Error));
                        return nullptr;
                    }
                }
            }

            ArrayHelper.InsertValues(InsertIndex, 1);
            InnerProperty->SetObjectPropertyValue(ArrayHelper.GetRawPtr(InsertIndex), NewObject_);

            FPropertyChangedEvent Changed(ArrayProperty, EPropertyChangeType::ArrayAdd);
            Owner->PostEditChangeProperty(Changed);
            Owner->MarkPackageDirty();

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("actor"), Actor->GetPathName());
            Result->SetStringField(TEXT("property"), PropertyPath);
            Result->SetNumberField(TEXT("index"), InsertIndex);
            Result->SetStringField(TEXT("object_path"), NewObject_->GetPathName());
            Result->SetStringField(TEXT("class"), NewClass->GetPathName());
            Result->SetNumberField(TEXT("count"), ArrayHelper.Num());
            Result->SetArrayField(TEXT("properties"), PropertyDiff);
            WriteDirtyReport(World, Before, Result);
            return Result;
        });

    // level.dirty_files — read-only. Dirty packages that saving the level would write: the map,
    // __ExternalActors__ / __ExternalObjects__ files (one per actor in a World Partition map), and
    // optionally other dirty assets. Params: include_assets (default false).
    // Returns { files: [{package, file, kind (map/external_actor/external_object/asset),
    // exists_on_disk, read_only, p4 (add / edit / delete), actor?, actor_class?}], counts }.
    // p4 is a hint from the file state; the checkout itself stays with the caller's Perforce tool.
    Registry.Register(TEXT("level.dirty_files"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWorld* World = GetEditorWorld(OutError);
            if (!World) return nullptr;
            bool bIncludeAssets = false;
            if (Params.IsValid())
            {
                Params->TryGetBoolField(TEXT("include_assets"), bIncludeAssets);
            }

            const UPackage* MapPackage = World->PersistentLevel->GetPackage();
            TMap<FString, int32> Counts;
            TArray<TSharedPtr<FJsonValue>> Files;
            for (TObjectIterator<UPackage> It; It; ++It)
            {
                UPackage* Package = *It;
                if (!Package->IsDirty() || Package == GetTransientPackage()
                    || Package->HasAnyPackageFlags(PKG_PlayInEditor | PKG_CompiledIn | PKG_ContainsScript))
                {
                    continue;
                }
                const FString Name = Package->GetName();
                FString Kind;
                if (Package == MapPackage)                                  { Kind = TEXT("map"); }
                else if (Name.Contains(TEXT("/__ExternalActors__/")))       { Kind = TEXT("external_actor"); }
                else if (Name.Contains(TEXT("/__ExternalObjects__/")))      { Kind = TEXT("external_object"); }
                else if (bIncludeAssets && !Name.StartsWith(TEXT("/Temp/"))) { Kind = TEXT("asset"); }
                else                                                        { continue; }

                const FString File = PackageFilename(Package);
                const bool bExists = !File.IsEmpty() && IFileManager::Get().FileExists(*File);
                const bool bReadOnly = bExists && IFileManager::Get().IsReadOnly(*File);

                AActor* PackageActor = nullptr;
                if (Kind == TEXT("external_actor"))
                {
                    ForEachObjectWithPackage(Package, [&PackageActor](UObject* Object)
                    {
                        if (AActor* Actor = Cast<AActor>(Object))
                        {
                            PackageActor = Actor;
                            return false;
                        }
                        return true;
                    }, EGetObjectsFlags::None);
                }

                // An external actor package with no actor left in it is a deletion waiting for save.
                const bool bDeleted = Kind == TEXT("external_actor") && !PackageActor;
                const FString P4 = !bExists ? TEXT("add") : (bDeleted ? TEXT("delete") : TEXT("edit"));

                auto Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("package"), Name);
                Entry->SetStringField(TEXT("file"), File);
                Entry->SetStringField(TEXT("kind"), Kind);
                Entry->SetBoolField(TEXT("exists_on_disk"), bExists);
                Entry->SetBoolField(TEXT("read_only"), bReadOnly);
                Entry->SetStringField(TEXT("p4"), P4);
                if (PackageActor)
                {
                    Entry->SetStringField(TEXT("actor"), PackageActor->GetActorLabel());
                    Entry->SetStringField(TEXT("actor_class"), PackageActor->GetClass()->GetName());
                }
                Files.Add(MakeShared<FJsonValueObject>(Entry));
                Counts.FindOrAdd(P4)++;
            }

            auto CountsJson = MakeShared<FJsonObject>();
            for (const TPair<FString, int32>& Pair : Counts)
            {
                CountsJson->SetNumberField(Pair.Key, Pair.Value);
            }
            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("files"), Files);
            Result->SetObjectField(TEXT("counts"), CountsJson);
            return Result;
        });

    // level.spline_set_points — rewrites a spline component (e.g. AMonsterSpawnPoint.PatrolRoute).
    // Params: actor, component (name; default the first spline), points ([[x,y,z], ...], >= 2),
    // space ("world" default / "local"), closed_loop (bool), point_type (curve / linear /
    // constant / curve_clamped), dry_run. Marks the spline as edited so a construction script does
    // not regenerate it. Returns { component, old_points, new_points (world), closed_loop, dirty report }.
    Registry.Register(TEXT("level.spline_set_points"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWorld* World = GetEditorWorld(OutError);
            if (!World) return nullptr;
            if (!Params.IsValid())
            {
                Fail(OutError, TEXT("params object is required"));
                return nullptr;
            }
            AActor* Actor = RequireActor(World, Params, OutError);
            if (!Actor) return nullptr;

            FString ComponentName;
            Params->TryGetStringField(TEXT("component"), ComponentName);
            USplineComponent* Spline = FindNamedComponent<USplineComponent>(Actor, ComponentName);
            if (!Spline)
            {
                Fail(OutError, FString::Printf(TEXT("%s has no spline component%s%s"), *Actor->GetActorLabel(), ComponentName.IsEmpty() ? TEXT("") : TEXT(" named "), *ComponentName));
                return nullptr;
            }

            const TArray<TSharedPtr<FJsonValue>>* PointValues = nullptr;
            TArray<FVector> Points;
            if (Params->TryGetArrayField(TEXT("points"), PointValues) && PointValues)
            {
                for (const TSharedPtr<FJsonValue>& Value : *PointValues)
                {
                    FVector Point;
                    if (!ReadPoint(Value, Point))
                    {
                        Fail(OutError, TEXT("points entries must be [x, y, z]"));
                        return nullptr;
                    }
                    Points.Add(Point);
                }
            }
            if (Points.Num() < 2)
            {
                Fail(OutError, TEXT("points needs at least 2 entries"));
                return nullptr;
            }

            FString Space, PointTypeName;
            Params->TryGetStringField(TEXT("space"), Space);
            Params->TryGetStringField(TEXT("point_type"), PointTypeName);
            const ESplineCoordinateSpace::Type CoordinateSpace = Space.Equals(TEXT("local"), ESearchCase::IgnoreCase) ? ESplineCoordinateSpace::Local : ESplineCoordinateSpace::World;
            TOptional<ESplinePointType::Type> PointType;
            if (!PointTypeName.IsEmpty())
            {
                if (PointTypeName.Equals(TEXT("linear"), ESearchCase::IgnoreCase))              { PointType = ESplinePointType::Linear; }
                else if (PointTypeName.Equals(TEXT("constant"), ESearchCase::IgnoreCase))       { PointType = ESplinePointType::Constant; }
                else if (PointTypeName.Equals(TEXT("curve_clamped"), ESearchCase::IgnoreCase))  { PointType = ESplinePointType::CurveClamped; }
                else if (PointTypeName.Equals(TEXT("curve"), ESearchCase::IgnoreCase))          { PointType = ESplinePointType::Curve; }
                else
                {
                    Fail(OutError, TEXT("point_type is curve, linear, constant or curve_clamped"));
                    return nullptr;
                }
            }

            TArray<TSharedPtr<FJsonValue>> OldPoints;
            for (int32 Index = 0; Index < Spline->GetNumberOfSplinePoints(); ++Index)
            {
                OldPoints.Add(MakeVector3JsonArray(Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World)));
            }

            bool bDryRun = false;
            Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("actor"), Actor->GetPathName());
            Result->SetStringField(TEXT("component"), Spline->GetName());
            Result->SetArrayField(TEXT("old_points"), OldPoints);
            Result->SetBoolField(TEXT("dry_run"), bDryRun);
            if (bDryRun)
            {
                TArray<TSharedPtr<FJsonValue>> Planned;
                const FTransform& ToWorld = Spline->GetComponentTransform();
                for (const FVector& Point : Points)
                {
                    Planned.Add(MakeVector3JsonArray(CoordinateSpace == ESplineCoordinateSpace::Local ? ToWorld.TransformPosition(Point) : Point));
                }
                Result->SetArrayField(TEXT("new_points"), Planned);
                return Result;
            }

            const FDirtySnapshot Before = SnapshotDirtyPackages();
            {
                FScopedTransaction Transaction(NSLOCTEXT("UnrealMCPBridge", "SplinePoints", "MCP: Set Spline Points"));
                Spline->Modify();
                Spline->SetSplinePoints(Points, CoordinateSpace, /*bUpdateSpline=*/false);
                bool bClosedLoop = false;
                if (Params->TryGetBoolField(TEXT("closed_loop"), bClosedLoop))
                {
                    Spline->SetClosedLoop(bClosedLoop, /*bUpdateSpline=*/false);
                }
                if (PointType.IsSet())
                {
                    for (int32 Index = 0; Index < Spline->GetNumberOfSplinePoints(); ++Index)
                    {
                        Spline->SetSplinePointType(Index, PointType.GetValue(), /*bUpdateSpline=*/false);
                    }
                }
                Spline->bSplineHasBeenEdited = true;
                Spline->UpdateSpline();
                Spline->PostEditChange();
                Spline->MarkPackageDirty();
            }

            TArray<TSharedPtr<FJsonValue>> NewPoints;
            for (int32 Index = 0; Index < Spline->GetNumberOfSplinePoints(); ++Index)
            {
                NewPoints.Add(MakeVector3JsonArray(Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World)));
            }
            Result->SetArrayField(TEXT("new_points"), NewPoints);
            Result->SetBoolField(TEXT("closed_loop"), Spline->IsClosedLoop());
            WriteDirtyReport(World, Before, Result);
            return Result;
        });

    // level.volume_set_extent — resizes a trigger / blocking shape. Params: actor, extent ([x,y,z]
    // half size, cm) or size_m ([x,y,z] full size, m), component (name; default the first shape),
    // dry_run. Box: box extent. Sphere: radius = extent.X. Capsule: radius = X, half height = Z.
    // Brush volume (no shape component): actor scale so the brush bounds match.
    // Returns { method, component, old_extent, new_extent, dirty report }.
    Registry.Register(TEXT("level.volume_set_extent"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWorld* World = GetEditorWorld(OutError);
            if (!World) return nullptr;
            if (!Params.IsValid())
            {
                Fail(OutError, TEXT("params object is required"));
                return nullptr;
            }
            AActor* Actor = RequireActor(World, Params, OutError);
            if (!Actor) return nullptr;

            FVector Extent = FVector::ZeroVector;
            if (Params->HasField(TEXT("size_m")))
            {
                FVector SizeM;
                if (!ReadVector3(Params, TEXT("size_m"), SizeM, OutError)) return nullptr;
                Extent = SizeM * 50.0;
            }
            else if (Params->HasField(TEXT("extent")))
            {
                if (!ReadVector3(Params, TEXT("extent"), Extent, OutError)) return nullptr;
            }
            else
            {
                Fail(OutError, TEXT("extent ([x,y,z] half size, cm) or size_m ([x,y,z] metres) is required"));
                return nullptr;
            }
            if (Extent.GetMin() <= 0.0)
            {
                Fail(OutError, TEXT("extent components must be > 0"));
                return nullptr;
            }

            FString ComponentName;
            Params->TryGetStringField(TEXT("component"), ComponentName);
            UBoxComponent* BoxShape = FindNamedComponent<UBoxComponent>(Actor, ComponentName);
            USphereComponent* SphereShape = BoxShape ? nullptr : FindNamedComponent<USphereComponent>(Actor, ComponentName);
            UCapsuleComponent* CapsuleShape = (BoxShape || SphereShape) ? nullptr : FindNamedComponent<UCapsuleComponent>(Actor, ComponentName);
            UBrushComponent* Brush = (BoxShape || SphereShape || CapsuleShape) ? nullptr : FindNamedComponent<UBrushComponent>(Actor, ComponentName);

            FString Method;
            FVector OldExtent = FVector::ZeroVector;
            UActorComponent* Target = nullptr;
            if (BoxShape)          { Method = TEXT("box_extent"); OldExtent = BoxShape->GetUnscaledBoxExtent(); Target = BoxShape; }
            else if (SphereShape)  { Method = TEXT("sphere_radius"); OldExtent = FVector(SphereShape->GetUnscaledSphereRadius()); Target = SphereShape; }
            else if (CapsuleShape) { Method = TEXT("capsule"); OldExtent = FVector(CapsuleShape->GetUnscaledCapsuleRadius(), CapsuleShape->GetUnscaledCapsuleRadius(), CapsuleShape->GetUnscaledCapsuleHalfHeight()); Target = CapsuleShape; }
            else if (Brush)        { Method = TEXT("brush_scale"); OldExtent = Brush->Bounds.BoxExtent; Target = Brush; }
            else
            {
                Fail(OutError, FString::Printf(TEXT("%s has no box, sphere, capsule or brush component%s%s"), *Actor->GetActorLabel(), ComponentName.IsEmpty() ? TEXT("") : TEXT(" named "), *ComponentName));
                return nullptr;
            }

            bool bDryRun = false;
            Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("actor"), Actor->GetPathName());
            Result->SetStringField(TEXT("method"), Method);
            Result->SetStringField(TEXT("component"), Target->GetName());
            Result->SetField(TEXT("old_extent"), MakeVector3JsonArray(OldExtent));
            Result->SetField(TEXT("new_extent"), MakeVector3JsonArray(Extent));
            Result->SetBoolField(TEXT("dry_run"), bDryRun);
            if (bDryRun)
            {
                return Result;
            }

            const FDirtySnapshot Before = SnapshotDirtyPackages();
            {
                FScopedTransaction Transaction(NSLOCTEXT("UnrealMCPBridge", "VolumeExtent", "MCP: Set Volume Extent"));
                if (BoxShape)
                {
                    BoxShape->Modify();
                    BoxShape->SetBoxExtent(Extent);
                    BoxShape->PostEditChange();
                }
                else if (SphereShape)
                {
                    SphereShape->Modify();
                    SphereShape->SetSphereRadius(Extent.X);
                    SphereShape->PostEditChange();
                }
                else if (CapsuleShape)
                {
                    CapsuleShape->Modify();
                    CapsuleShape->SetCapsuleSize(Extent.X, Extent.Z);
                    CapsuleShape->PostEditChange();
                }
                else
                {
                    // Brush geometry stays as authored; scale the actor so the world bounds match.
                    const FBoxSphereBounds LocalBounds = Brush->CalcBounds(FTransform::Identity);
                    const FVector LocalExtent = LocalBounds.BoxExtent.ComponentMax(FVector(KINDA_SMALL_NUMBER));
                    Actor->Modify();
                    Actor->SetActorScale3D(Extent / LocalExtent);
                    Actor->PostEditMove(/*bFinished=*/true);
                }
                Actor->MarkPackageDirty();
            }
            WriteDirtyReport(World, Before, Result);
            return Result;
        });

    // nav.project_point — read-only. Params: points ([[x,y,z], ...]), extent ([x,y,z] query box
    // half size, default [50, 50, 250]), world ("editor" default / "pie").
    // Returns { points: [{input, ok, location, distance}] }.
    Registry.Register(TEXT("nav.project_point"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWorld* World = NavWorld(Params, OutError);
            if (!World) return nullptr;
            const TArray<TSharedPtr<FJsonValue>>* PointValues = nullptr;
            if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("points"), PointValues) || !PointValues || PointValues->Num() == 0)
            {
                Fail(OutError, TEXT("points ([[x, y, z], ...]) is required"));
                return nullptr;
            }
            FVector Extent(50.0, 50.0, 250.0);
            if (!ReadVector3(Params, TEXT("extent"), Extent, OutError)) return nullptr;

            FString Error;
            const ANavigationData* NavData = FindNavData(World, Error);
            UNavigationSystemV1* NavSystem = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
            if (!NavData || !NavSystem)
            {
                Fail(OutError, Error, MCPProtocol::FMCPError::InternalError);
                return nullptr;
            }

            TArray<TSharedPtr<FJsonValue>> Results;
            for (const TSharedPtr<FJsonValue>& Value : *PointValues)
            {
                FVector Point;
                if (!ReadPoint(Value, Point))
                {
                    Fail(OutError, TEXT("points entries must be [x, y, z]"));
                    return nullptr;
                }
                FNavLocation Projected;
                const bool bOk = NavSystem->ProjectPointToNavigation(Point, Projected, Extent, NavData);
                auto Entry = MakeShared<FJsonObject>();
                Entry->SetField(TEXT("input"), MakeVector3JsonArray(Point));
                Entry->SetBoolField(TEXT("ok"), bOk);
                if (bOk)
                {
                    Entry->SetField(TEXT("location"), MakeVector3JsonArray(Projected.Location));
                    Entry->SetNumberField(TEXT("distance"), FVector::Dist(Point, Projected.Location));
                }
                Results.Add(MakeShared<FJsonValueObject>(Entry));
            }
            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("points"), Results);
            Result->SetStringField(TEXT("nav_data"), NavData->GetName());
            return Result;
        });

    // nav.test_path — read-only. Does a navmesh path connect start to end? Params: start + end,
    // or pairs ([{start, end}]), extent (projection box, default [50, 50, 250]), world.
    // Start and end are projected onto the navmesh first.
    // Returns { paths: [{start, end, projected, ok, partial, length, points}] }.
    Registry.Register(TEXT("nav.test_path"),
        [](const TSharedPtr<FJsonObject>& Params, MCPProtocol::FMCPError& OutError) -> TSharedPtr<FJsonObject>
        {
            UWorld* World = NavWorld(Params, OutError);
            if (!World) return nullptr;
            if (!Params.IsValid())
            {
                Fail(OutError, TEXT("start + end or pairs is required"));
                return nullptr;
            }

            TArray<TPair<FVector, FVector>> Pairs;
            const TArray<TSharedPtr<FJsonValue>>* PairValues = nullptr;
            if (Params->TryGetArrayField(TEXT("pairs"), PairValues) && PairValues)
            {
                for (const TSharedPtr<FJsonValue>& Value : *PairValues)
                {
                    const TSharedPtr<FJsonObject>* Pair = nullptr;
                    FVector Start, End;
                    if (!Value->TryGetObject(Pair) || !Pair || !ReadPoint((*Pair)->TryGetField(TEXT("start")), Start) || !ReadPoint((*Pair)->TryGetField(TEXT("end")), End))
                    {
                        Fail(OutError, TEXT("pairs entries are {start: [x,y,z], end: [x,y,z]}"));
                        return nullptr;
                    }
                    Pairs.Add({ Start, End });
                }
            }
            else
            {
                FVector Start, End;
                if (!ReadPoint(Params->TryGetField(TEXT("start")), Start) || !ReadPoint(Params->TryGetField(TEXT("end")), End))
                {
                    Fail(OutError, TEXT("start and end ([x, y, z]) are required"));
                    return nullptr;
                }
                Pairs.Add({ Start, End });
            }
            FVector Extent(50.0, 50.0, 250.0);
            if (!ReadVector3(Params, TEXT("extent"), Extent, OutError)) return nullptr;

            FString Error;
            const ANavigationData* NavData = FindNavData(World, Error);
            UNavigationSystemV1* NavSystem = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
            if (!NavData || !NavSystem)
            {
                Fail(OutError, Error, MCPProtocol::FMCPError::InternalError);
                return nullptr;
            }

            TArray<TSharedPtr<FJsonValue>> Results;
            for (const TPair<FVector, FVector>& Pair : Pairs)
            {
                auto Entry = MakeShared<FJsonObject>();
                Entry->SetField(TEXT("start"), MakeVector3JsonArray(Pair.Key));
                Entry->SetField(TEXT("end"), MakeVector3JsonArray(Pair.Value));

                FNavLocation StartNav, EndNav;
                const bool bStartOk = NavSystem->ProjectPointToNavigation(Pair.Key, StartNav, Extent, NavData);
                const bool bEndOk = NavSystem->ProjectPointToNavigation(Pair.Value, EndNav, Extent, NavData);
                Entry->SetBoolField(TEXT("projected"), bStartOk && bEndOk);
                if (!bStartOk || !bEndOk)
                {
                    Entry->SetBoolField(TEXT("ok"), false);
                    Entry->SetStringField(TEXT("error"), !bStartOk ? TEXT("start is not on the navmesh") : TEXT("end is not on the navmesh"));
                    Results.Add(MakeShared<FJsonValueObject>(Entry));
                    continue;
                }

                FPathFindingQuery Query(nullptr, *NavData, StartNav.Location, EndNav.Location);
                const FPathFindingResult PathResult = NavSystem->FindPathSync(Query);
                const bool bFound = PathResult.IsSuccessful() && PathResult.Path.IsValid();
                Entry->SetBoolField(TEXT("ok"), bFound && !PathResult.IsPartial());
                Entry->SetBoolField(TEXT("partial"), PathResult.IsPartial());
                if (bFound)
                {
                    Entry->SetNumberField(TEXT("length"), PathResult.Path->GetLength());
                    TArray<TSharedPtr<FJsonValue>> PathPoints;
                    for (const FNavPathPoint& PathPoint : PathResult.Path->GetPathPoints())
                    {
                        PathPoints.Add(MakeVector3JsonArray(PathPoint.Location));
                    }
                    Entry->SetArrayField(TEXT("points"), PathPoints);
                }
                Results.Add(MakeShared<FJsonValueObject>(Entry));
            }
            auto Result = MakeShared<FJsonObject>();
            Result->SetArrayField(TEXT("paths"), Results);
            Result->SetStringField(TEXT("nav_data"), NavData->GetName());
            return Result;
        });
}
