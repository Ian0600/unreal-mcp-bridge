#include "MCPInspectorEditorToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "IPythonScriptPlugin.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/StringOutputDevice.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/ObjectRedirector.h"

static TAutoConsoleVariable<bool> CVarBPIEnablePythonTool(
	TEXT("BlueprintInspector.EnablePythonTool"),
	true,
	TEXT("Allow the MCPInspectorEditorToolset.ExecutePython tool to run Python code."));

namespace MCPInspectorEditorToolset
{
	static FString ToJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	// Captures log lines emitted while a console command runs.
	class FCaptureDevice : public FStringOutputDevice
	{
	public:
		FCaptureDevice() { SetAutoEmitLineTerminator(true); }
	};
}


FString UMCPInspectorEditorToolset::ExecuteConsoleCommand(const FString& Command)
{
	using namespace MCPInspectorEditorToolset;
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	FCaptureDevice Capture;
	GLog->AddOutputDevice(&Capture);
	bool bHandled = false;
	if (GEditor)
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		bHandled = GEditor->Exec(World, *Command, Capture);
	}
	GLog->RemoveOutputDevice(&Capture);

	Result->SetBoolField(TEXT("success"), bHandled);
	Result->SetStringField(TEXT("output"), Capture);
	return ToJson(Result);
}

FString UMCPInspectorEditorToolset::FixupRedirectors(const TArray<FString>& ContentPaths)
{
	using namespace MCPInspectorEditorToolset;
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
	for (const FString& Path : ContentPaths)
	{
		Filter.PackagePaths.Add(FName(*Path));
	}
	TArray<FAssetData> Found;
	AssetRegistry.GetAssets(Filter, Found);

	TArray<UObjectRedirector*> Redirectors;
	for (const FAssetData& Asset : Found)
	{
		if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(Asset.GetAsset()))
		{
			Redirectors.Add(Redirector);
		}
	}

	if (Redirectors.Num() > 0)
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		AssetTools.FixupReferencers(Redirectors, /*bCheckoutDialogPrompt*/ false);
	}

	TArray<FAssetData> Remaining;
	AssetRegistry.GetAssets(Filter, Remaining);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("found"), Found.Num());
	Result->SetNumberField(TEXT("fixed"), Found.Num() - Remaining.Num());
	TArray<TSharedPtr<FJsonValue>> Left;
	for (const FAssetData& Asset : Remaining)
	{
		Left.Add(MakeShared<FJsonValueString>(Asset.GetObjectPathString()));
	}
	Result->SetArrayField(TEXT("remaining"), Left);
	return ToJson(Result);
}
