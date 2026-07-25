#pragma once

#include "CoreMinimal.h"
#include "IMCPCommandHandler.h"

// Handles material.* JSON-RPC methods:
//   creation      : material.create / create_instance / create_function
//   inspection    : material.get_info / list_parameters / list_expressions / get_statistics
//   instance edit : material.set_scalar_param / set_vector_param / set_texture_param /
//                   set_static_switch_param / set_instance_parent / clear_instance_parameters /
//                   set_parameter_override / update_instance
//   graph edit    : material.add_expression / set_expression_property / connect_expressions /
//                   connect_property / disconnect_expression / disconnect_property /
//                   delete_expression / delete_all_expressions / delete_unused_expressions /
//                   layout_expressions
//   base / build  : material.set_property / recompile / update_function
//
// UE 5.3 note: these 5.8-only commands are omitted here because their UMaterialEditingLibrary
// backing functions do not exist in 5.3 — set_parameter_override, disconnect_expression,
// disconnect_property, delete_unused_expressions. (recompile returns no error list in 5.3.)
class FMaterialCommandHandler : public IMCPCommandHandler
{
public:
    virtual void RegisterCommands(FMCPCommandRegistry& Registry) override;
};
