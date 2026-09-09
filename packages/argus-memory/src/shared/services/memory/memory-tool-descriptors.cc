#include "memory-tool-descriptors.hxx"

std::vector<tools::ToolDescriptor> memoryToolDescriptors()
{
  using tools::ToolArgumentSpec;
  std::vector<tools::ToolDescriptor> descriptors;
  descriptors.push_back({.name = "memory.remember",
                         .description = "Almacena un hecho sobre una persona, "
                                        "dispositivo o lugar de la casa. El "
                                        "hecho completo va en el argumento "
                                        "text, tal cual lo pidió el usuario",
                         // No required arguments: the f8-b4 probes measured
                         // the model dropping or mangling the middle of the
                         // subject/predicate/value triple on nearly every
                         // fired call, while echoing the sentence faithfully.
                         // A fired call must reach the handler; formation
                         // honors a complete triple and rule-parses the text
                         // otherwise.
                         .arguments = {{"text", "string", false, {}, ""},
                                       {"subject", "string", false, {}, ""},
                                       {"predicate", "string", false, {}, ""},
                                       {"value", "string", false, {}, ""},
                                       {"type",
                                        "enum",
                                        false,
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
