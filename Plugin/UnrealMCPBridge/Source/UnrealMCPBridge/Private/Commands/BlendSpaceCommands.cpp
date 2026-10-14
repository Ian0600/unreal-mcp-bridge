#include "Commands/BlendSpaceCommands.h"
#include "Commands/InspectorInvoke.h"
#include "MCPCommandRegistry.h"

void FBlendSpaceCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    // ---- BEGIN commands forwarded to BlueprintInspector (generated from its function headers) ----
    Registry.Register(TEXT("blendspace.dump_structure"), MakeInspectorCommand(TEXT("/Script/UnrealMCPBridge.MCPInspectorLibrary"), TEXT("DumpBlendSpaceStructure"), { TEXT("asset_path") }));
    Registry.Register(TEXT("blendspace.add_sample"), MakeInspectorCommand(TEXT("/Script/UnrealMCPBridge.MCPInspectorLibrary"), TEXT("AddBlendSpaceSample"), { TEXT("asset_path"), TEXT("sequence_path"), TEXT("x"), TEXT("y") }));
    Registry.Register(TEXT("blendspace.move_sample"), MakeInspectorCommand(TEXT("/Script/UnrealMCPBridge.MCPInspectorLibrary"), TEXT("MoveBlendSpaceSample"), { TEXT("asset_path"), TEXT("sample_index"), TEXT("x"), TEXT("y") }));
    Registry.Register(TEXT("blendspace.replace_sample_animation"), MakeInspectorCommand(TEXT("/Script/UnrealMCPBridge.MCPInspectorLibrary"), TEXT("ReplaceBlendSpaceSampleAnimation"), { TEXT("asset_path"), TEXT("sample_index"), TEXT("sequence_path") }));
    Registry.Register(TEXT("blendspace.remove_sample"), MakeInspectorCommand(TEXT("/Script/UnrealMCPBridge.MCPInspectorLibrary"), TEXT("RemoveBlendSpaceSample"), { TEXT("asset_path"), TEXT("sample_index") }));
    Registry.Register(TEXT("blendspace.set_sample_rate_scale"), MakeInspectorCommand(TEXT("/Script/UnrealMCPBridge.MCPInspectorLibrary"), TEXT("SetBlendSpaceSampleRateScale"), { TEXT("asset_path"), TEXT("sample_index"), TEXT("rate_scale") }));
    Registry.Register(TEXT("blendspace.set_axis"), MakeInspectorCommand(TEXT("/Script/UnrealMCPBridge.MCPInspectorLibrary"), TEXT("SetBlendSpaceAxis"), { TEXT("asset_path"), TEXT("axis_index"), TEXT("axis_name"), TEXT("min"), TEXT("max"), TEXT("grid_num"), TEXT("snap_to_grid"), TEXT("wrap_input") }));
    // ---- END forwarded commands ----
}
