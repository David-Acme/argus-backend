#include "memory-tool-descriptors.hxx"

std::vector<tools::ToolDescriptor> memoryToolDescriptors()
{
  using tools::ToolArgumentSpec;
  std::vector<tools::ToolDescriptor> descriptors;
  descriptors.push_back({.name = "memory.remember",
                         .description = "Almacena un hecho sobre una persona, "
                                        "dispositivo o lugar de la casa",
                         .arguments = {{"subject", "string", true, {}, ""},
                                       {"predicate", "string", true, {}, ""},
                                       {"value", "string", true, {}, ""},
                                       {"type",
                                        "enum",
                                        true,
                                        {"persona", "preference", "schedule",
                                         "instruction", "attribute"},
                                        ""},
                                       {"confidence", "number", false, {}, ""}},
                         .accessTable = TableName::Memory,
                         .accessPermission = RolePermission::Create,
                         .handler = nullptr});
  descriptors.push_back({.name = "memory.recall",
                         .description = "Recupera hechos guardados sobre la "
                                        "casa, las personas o los dispositivos",
                         .arguments = {{"query", "string", true, {}, ""}},
                         .accessTable = TableName::Memory,
                         .accessPermission = RolePermission::Read,
                         .handler = nullptr});
  descriptors.push_back({.name = "procedure.run",
                         .description = "Ejecuta un procedimiento conocido "
                                        "para conseguir un objetivo",
                         .arguments = {{"goal", "string", true, {}, ""}},
                         .accessTable = TableName::Memory,
                         .accessPermission = RolePermission::Read,
                         .handler = nullptr});
  descriptors.push_back({.name = "memory.forget",
                         .description = "Olvida un hecho guardado por su id",
                         .arguments = {{"fact_id", "number", true, {}, ""}},
                         .accessTable = TableName::Memory,
                         .accessPermission = RolePermission::Delete,
                         .handler = nullptr});
  return descriptors;
}
