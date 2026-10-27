#pragma once

#include "CoreMinimal.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "UObject/PropertyPortFlags.h"

/**
 * Reflection helpers shared by every handler that reads or writes UObject properties by path:
 * object.* (any object), level.* (level actors, with dry-run diffs) and scene.* (temp actors).
 *
 * These used to live in ObjectCommands.cpp's anonymous namespace. They moved here when the
 * level data commands needed the very same path walk and import/export rules — two copies of
 * "how a property path resolves" would drift, and a value one command accepts the other would
 * reject. Inline in a named namespace for the same unity-build reason as CommandJsonHelpers.h.
 */
namespace UnrealMCPBridge::Reflection
{
    /**
     * Resolves an object path to a live UObject, loading the package if needed.
     *
     * Accepts the three shapes that show up in practice:
     *   /Game/Path/Asset.Asset                  — the asset itself
     *   /Game/Path/Asset.Asset:SubObject        — an instanced subobject (camera nodes, and such)
     *   /Game/Map.Map:PersistentLevel.Actor_0   — a live actor, PIE worlds included
     *
     * FindObject comes first so that already-loaded objects (and PIE-world objects, which no
     * loader can produce) resolve without touching the package loader. Only then do we try
     * LoadObject, and only then the split fallback: LoadObject can fail on a subobject path
     * whose package is not yet in memory, because it resolves the subobject before the outer
     * has been created. Loading the outer explicitly and searching inside it fixes that case.
     *
     * Note the deliberate absence of any short-name lookup. FindFirstObject would accept
     * "BoomArmCameraNode_0", but it fires an EnsureIfAmbiguous ensure when two assets contain
     * a subobject of that name — which halts a debugger inside whatever is executing the
     * request and reads as a crash of the bridge. Full paths only.
     */
    inline UObject* ResolveObject(const FString& Path)
    {
        if (Path.IsEmpty())
        {
            return nullptr;
        }

        if (UObject* Found = FindObject<UObject>(nullptr, *Path))
        {
            return Found;
        }
        if (UObject* Loaded = LoadObject<UObject>(nullptr, *Path))
        {
            return Loaded;
        }

        // Subobject path whose outer is not loaded yet: load the outer, then walk inward.
        FString OuterPath, SubPath;
        if (!Path.Split(TEXT(":"), &OuterPath, &SubPath))
        {
            return nullptr;
        }
        UObject* Outer = LoadObject<UObject>(nullptr, *OuterPath);
        if (!Outer)
        {
            return nullptr;
        }

        TArray<FString> Parts;
        SubPath.ParseIntoArray(Parts, TEXT("."), /*InCullEmpty=*/true);
        UObject* Current = Outer;
        for (const FString& Part : Parts)
        {
            Current = StaticFindObject(UObject::StaticClass(), Current, *Part);
            if (!Current)
            {
                return nullptr;
            }
        }
        return Current;
    }

    /**
     * A property plus the address of the specific value it names — the result of walking a path.
     * Owner is the innermost object that holds the value: the root for "Count", the component
     * for "PatrolRoute.Duration". Modify/PreEditChange/PostEditChange belong on Owner, not on
     * the root, or the component never hears that it changed.
     */
    struct FResolvedProperty
    {
        FProperty* Property = nullptr;
        void*      Address  = nullptr;
        UObject*   Owner    = nullptr;
    };

    /**
     * Walks a dotted property path from a root object down to one value.
     *
     * Supports struct members, object hops, and array indexing, so all of these work:
     *   "BoomOffset"                  a plain property
     *   "RelativeLocation.X"          a struct member
     *   "Children[2]"                 one array element
     *   "Children[2].bIsEnabled"      a member of the object an array element points at
     *
     * The address returned is the value's own address, which is what ImportText_Direct and
     * ExportText_Direct both want — not the container's.
     */
    inline bool ResolvePropertyPath(UObject* Root, const FString& PropertyPath, FResolvedProperty& Out, FString& OutError)
    {
        TArray<FString> Tokens;
        PropertyPath.ParseIntoArray(Tokens, TEXT("."), /*InCullEmpty=*/true);
        if (Tokens.Num() == 0)
        {
            OutError = TEXT("property_name is empty");
            return false;
        }

        UStruct* CurrentStruct = Root->GetClass();
        void*    CurrentAddr   = Root;
        UObject* CurrentOwner  = Root;
        FProperty* CurrentProp = nullptr;

        for (int32 TokenIndex = 0; TokenIndex < Tokens.Num(); ++TokenIndex)
        {
            FString Token = Tokens[TokenIndex];

            // Peel a trailing [n] off the token, if present.
            int32 ArrayIndex = INDEX_NONE;
            int32 OpenBracket = INDEX_NONE;
            if (Token.FindChar(TEXT('['), OpenBracket))
            {
                const int32 CloseBracket = Token.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
                if (CloseBracket == INDEX_NONE || CloseBracket <= OpenBracket)
                {
                    OutError = FString::Printf(TEXT("Malformed array index in '%s'"), *Token);
                    return false;
                }
                ArrayIndex = FCString::Atoi(*Token.Mid(OpenBracket + 1, CloseBracket - OpenBracket - 1));
                Token = Token.Left(OpenBracket);
            }

            if (!CurrentStruct)
            {
                OutError = FString::Printf(TEXT("Cannot descend into '%s' — the previous step is not a struct or object"), *Token);
                return false;
            }

            FProperty* Prop = FindFProperty<FProperty>(CurrentStruct, *Token);
            if (!Prop)
            {
                OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *Token, *CurrentStruct->GetName());
                return false;
            }

            void* Addr = Prop->ContainerPtrToValuePtr<void>(CurrentAddr);

            if (ArrayIndex != INDEX_NONE)
            {
                FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
                if (!ArrayProp)
                {
                    OutError = FString::Printf(TEXT("Property '%s' is not an array, so [%d] is invalid"), *Token, ArrayIndex);
                    return false;
                }
                FScriptArrayHelper Helper(ArrayProp, Addr);
                if (!Helper.IsValidIndex(ArrayIndex))
                {
                    OutError = FString::Printf(TEXT("Index %d is out of range for '%s' (%d elements)"),
                        ArrayIndex, *Token, Helper.Num());
                    return false;
                }
                Addr = Helper.GetRawPtr(ArrayIndex);
                Prop = ArrayProp->Inner;
            }

            CurrentProp = Prop;
            CurrentAddr = Addr;

            const bool bMoreToWalk = (TokenIndex + 1 < Tokens.Num());
            if (bMoreToWalk)
            {
                if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
                {
                    CurrentStruct = StructProp->Struct;
                    // CurrentAddr already points at the struct instance.
                }
                else if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
                {
                    UObject* Inner = ObjProp->GetObjectPropertyValue(Addr);
                    if (!Inner)
                    {
                        OutError = FString::Printf(TEXT("'%s' is null, cannot walk past it"), *Token);
                        return false;
                    }
                    CurrentStruct = Inner->GetClass();
                    CurrentAddr   = Inner;
                    CurrentOwner  = Inner;
                }
                else
                {
                    OutError = FString::Printf(TEXT("'%s' is a %s — only structs and objects can be walked into"),
                        *Token, *Prop->GetClass()->GetName());
                    return false;
                }
            }
        }

        Out.Property = CurrentProp;
        Out.Address  = CurrentAddr;
        Out.Owner    = CurrentOwner;
        return true;
    }

    /**
     * ExportText for one value, never delta-suppressed. ExportText_Direct skips values Identical to
     * the delta, and a null delta compares as zero — so passing null exported false / 0 / empty as
     * "". Passing the value itself as the delta is the engine's "always export" case.
     */
    inline FString ExportValue(const FProperty* Prop, const void* Address, UObject* Owner)
    {
        FString Text;
        Prop->ExportText_Direct(Text, Address, /*DefaultValue=*/Address, Owner, PPF_None);
        return Text;
    }

    /** "FVector"/"UCameraNode*"-style type name, for humans reading list_properties output. */
    inline FString TypeName(const FProperty* Prop)
    {
        return Prop->GetCPPType();
    }

    /** Optional sign, digits, optional fraction, optional exponent — what a number property should take. */
    inline bool IsNumberText(const FString& InText)
    {
        const FString Text = InText.TrimStartAndEnd();
        int32 Index = 0;
        auto Digits = [&Text, &Index]()
        {
            const int32 Start = Index;
            while (Index < Text.Len() && FChar::IsDigit(Text[Index])) { ++Index; }
            return Index - Start;
        };
        if (Index < Text.Len() && (Text[Index] == TEXT('-') || Text[Index] == TEXT('+'))) { ++Index; }
        int32 Count = Digits();
        if (Index < Text.Len() && Text[Index] == TEXT('.')) { ++Index; Count += Digits(); }
        if (Count == 0) { return false; }
        if (Index < Text.Len() && (Text[Index] == TEXT('e') || Text[Index] == TEXT('E')))
        {
            ++Index;
            if (Index < Text.Len() && (Text[Index] == TEXT('-') || Text[Index] == TEXT('+'))) { ++Index; }
            if (Digits() == 0) { return false; }
        }
        return Index == Text.Len();
    }

    /**
     * Catches the inputs ImportText takes without complaint but does not mean: number properties
     * accept "abc" as 0, and bools treat unknown words as false. Enum-backed bytes keep their names.
     */
    inline bool ValidateImportText(const FProperty* Prop, const FString& Text, FString& OutError)
    {
        if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Prop))
        {
            const bool bHex = Numeric->IsInteger() && Text.TrimStartAndEnd().StartsWith(TEXT("0x"), ESearchCase::IgnoreCase);
            if (!Numeric->GetIntPropertyEnum() && !bHex && !IsNumberText(Text))
            {
                OutError = FString::Printf(TEXT("'%s' is not a number (type %s)"), *Text, *TypeName(Prop));
                return false;
            }
        }
        else if (Prop->IsA<FBoolProperty>())
        {
            static const TCHAR* const Words[] = { TEXT("true"), TEXT("false"), TEXT("yes"), TEXT("no"), TEXT("1"), TEXT("0") };
            const FString Trimmed = Text.TrimStartAndEnd();
            bool bKnown = false;
            for (const TCHAR* Word : Words)
            {
                bKnown |= Trimmed.Equals(Word, ESearchCase::IgnoreCase);
            }
            if (!bKnown)
            {
                OutError = FString::Printf(TEXT("'%s' is not a bool (True / False)"), *Text);
                return false;
            }
        }
        return true;
    }

    /**
     * Parses Text into a scratch value of Prop's type without touching the object, and returns
     * the value as it would read back afterwards. This is what a dry run shows as "new_value":
     * the normalized form ("3" becomes "3.000000" for a float), and a parse failure surfaces
     * before anything is written.
     */
    inline bool PreviewImport(const FProperty* Prop, const FString& Text, UObject* Owner, FString& OutNormalized, FString& OutError)
    {
        if (!ValidateImportText(Prop, Text, OutError))
        {
            return false;
        }
        void* Scratch = FMemory::Malloc(Prop->GetSize(), Prop->GetMinAlignment());
        Prop->InitializeValue(Scratch);
        const TCHAR* ImportResult = Prop->ImportText_Direct(*Text, Scratch, Owner, PPF_None);
        if (ImportResult != nullptr)
        {
            OutNormalized = ExportValue(Prop, Scratch, Owner);
        }
        Prop->DestroyValue(Scratch);
        FMemory::Free(Scratch);

        if (ImportResult == nullptr)
        {
            OutError = FString::Printf(
                TEXT("Failed to parse value '%s' (type %s). Read the property first and match that format."),
                *Text, *TypeName(Prop));
            return false;
        }
        return true;
    }

    /**
     * Writes one property path from text, the way the details panel would: Modify (so an open
     * transaction records it), PreEditChange, import, PostEditChangeProperty, dirty the package.
     * All of it lands on Resolved.Owner. Returns false with OutError on a path or parse failure;
     * the value is untouched in that case.
     */
    inline bool SetPropertyFromText(UObject* Root, const FString& PropertyPath, const FString& Text,
        FString& OutOldValue, FString& OutNewValue, FString& OutError)
    {
        FResolvedProperty Resolved;
        if (!ResolvePropertyPath(Root, PropertyPath, Resolved, OutError))
        {
            return false;
        }

        UObject* Owner = Resolved.Owner;
        if (!ValidateImportText(Resolved.Property, Text, OutError))
        {
            OutError = FString::Printf(TEXT("%s: %s"), *PropertyPath, *OutError);
            return false;
        }
        OutOldValue = ExportValue(Resolved.Property, Resolved.Address, Owner);

        Owner->Modify();
        Owner->PreEditChange(Resolved.Property);

        const TCHAR* ImportResult = Resolved.Property->ImportText_Direct(*Text, Resolved.Address, Owner, PPF_None);
        if (ImportResult == nullptr)
        {
            // ImportText leaves the value untouched when it cannot parse, so there is nothing
            // to roll back — but PreEditChange was already announced, so close the pair.
            FPropertyChangedEvent FailedEvent(Resolved.Property, EPropertyChangeType::ValueSet);
            Owner->PostEditChangeProperty(FailedEvent);

            OutError = FString::Printf(
                TEXT("Failed to parse value '%s' for property '%s' (type %s). Read the property first and match that format."),
                *Text, *PropertyPath, *TypeName(Resolved.Property));
            return false;
        }

        FPropertyChangedEvent ChangedEvent(Resolved.Property, EPropertyChangeType::ValueSet);
        Owner->PostEditChangeProperty(ChangedEvent);
        Owner->MarkPackageDirty();

        OutNewValue = ExportValue(Resolved.Property, Resolved.Address, Owner);
        return true;
    }

} // namespace UnrealMCPBridge::Reflection
