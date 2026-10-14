#include "Commands/BehaviorTreeCommands.h"
#include "Commands/InspectorInvoke.h"
#include "MCPCommandRegistry.h"

void FBehaviorTreeCommandHandler::RegisterCommands(FMCPCommandRegistry& Registry)
{
    // ---- BEGIN commands forwarded to BlueprintInspector (generated from its function headers) ----
    Registry.Register(TEXT("behaviortree.create_blackboard"), MakeInspectorCommand(TEXT("/Script/UnrealMCPBridge.MCPInspectorBehaviorTreeToolset"), TEXT("CreateBlackboard"), { TEXT("asset_path"), TEXT("parent_blackboard_path") }));
    Registry.Register(TEXT("behaviortree.add_blackboard_key"), MakeInspectorCommand(TEXT("/Script/UnrealMCPBridge.MCPInspectorBehaviorTreeToolset"), TEXT("AddBlackboardKey"), { TEXT("blackboard_path"), TEXT("key_name"), TEXT("key_type"), TEXT("base_class_or_enum_path"), TEXT("instance_synced") }));
    Registry.Register(TEXT("behaviortree.remove_blackboard_key"), MakeInspectorCommand(TEXT("/Script/UnrealMCPBridge.MCPInspectorBehaviorTreeToolset"), TEXT("RemoveBlackboardKey"), { TEXT("blackboard_path"), TEXT("key_name") }));
    Registry.Register(TEXT("behaviortree.create"), MakeInspectorCommand(TEXT("/Script/UnrealMCPBridge.MCPInspectorBehaviorTreeToolset"), TEXT("CreateBehaviorTree"), { TEXT("asset_path"), TEXT("blackboard_path") }));
    Registry.Register(TEXT("behaviortree.add_node"), MakeInspectorCommand(TEXT("/Script/UnrealMCPBridge.MCPInspectorBehaviorTreeToolset"), TEXT("AddBTNode"), { TEXT("tree_path"), TEXT("parent_node_id"), TEXT("node_class"), TEXT("child_index") }));
    Registry.Register(TEXT("behaviortree.add_aux_node"), MakeInspectorCommand(TEXT("/Script/UnrealMCPBridge.MCPInspectorBehaviorTreeToolset"), TEXT("AddBTAuxNode"), { TEXT("tree_path"), TEXT("node_id"), TEXT("aux_class") }));
    Registry.Register(TEXT("behaviortree.set_node_property"), MakeInspectorCommand(TEXT("/Script/UnrealMCPBridge.MCPInspectorBehaviorTreeToolset"), TEXT("SetBTNodeProperty"), { TEXT("tree_path"), TEXT("node_id"), TEXT("property_path"), TEXT("value_text") }));
    Registry.Register(TEXT("behaviortree.remove_node"), MakeInspectorCommand(TEXT("/Script/UnrealMCPBridge.MCPInspectorBehaviorTreeToolset"), TEXT("RemoveBTNode"), { TEXT("tree_path"), TEXT("node_id") }));
    Registry.Register(TEXT("behaviortree.dump"), MakeInspectorCommand(TEXT("/Script/UnrealMCPBridge.MCPInspectorBehaviorTreeToolset"), TEXT("DumpBehaviorTree"), { TEXT("tree_path") }));
    // ---- END forwarded commands ----
}
