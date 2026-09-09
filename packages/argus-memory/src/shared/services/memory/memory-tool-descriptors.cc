#include "memory-tool-descriptors.hxx"

std::vector<tools::ToolDescriptor> memoryToolDescriptors()
{
  std::vector<tools::ToolDescriptor> descriptors;
  descriptors.push_back(
      {.name = "memory.remember",
       .description = "Almacena un hecho sobre una persona, dispositivo o "
                      "lugar de la casa. El hecho completo va en el argumento "
                      "text, tal cual lo pidió el usuario",
       // No required arguments: the f8-b4 probes measured the model dropping
       // or mangling the middle of the subject/predicate/value triple on
       // nearly every fired call, while echoing the sentence faithfully. A
       // fired call must reach the handler; formation honors a complete
       // triple and rule-parses the text otherwise.
       .arguments = {{.name = "text",
                      .type = "string",
                      .required = false,
                      .enumValues = {},
                      .description = ""},
                     {.name = "subject",
                      .type = "string",
                      .required = false,
                      .enumValues = {},
                      .description = ""},
                     {.name = "predicate",
                      .type = "string",
                      .required = false,
                      .enumValues = {},
                      .description = ""},
                     {.name = "value",
                      .type = "string",
                      .required = false,
                      .enumValues = {},
                      .description = ""},
                     {.name = "type",
                      .type = "enum",
                      .required = false,
                      .enumValues = {"persona", "preference", "schedule",
                                     "instruction", "attribute"},
                      .description = ""},
                     {.name = "confidence",
                      .type = "number",
                      .required = false,
                      .enumValues = {},
                      .description = ""}},
       .accessTable = TableName::Memory,
       .accessPermission = RolePermission::Create,
       .handler = nullptr});
  descriptors.push_back(
      {.name = "memory.remind",
       .description = "Guarda un recordatorio del usuario que habla: un hecho "
                      "con un momento concreto. No suena ninguna alarma; el "
                      "recordatorio se recupera al preguntar por él",
       // Same all-optional shape as memory.remember, for the same measured
       // reason: a required argument keeps a fired call from reaching the
       // handler at all.
       .arguments = {{.name = "text",
                      .type = "string",
                      .required = false,
                      .enumValues = {},
                      .description = ""},
                     {.name = "when",
                      .type = "string",
                      .required = false,
                      .enumValues = {},
                      .description = ""}},
       .accessTable = TableName::Memory,
       .accessPermission = RolePermission::Create,
       .handler = nullptr});
  descriptors.push_back(
      {.name = "memory.recall",
       .description = "Recupera hechos guardados sobre la casa, las personas "
                      "o los dispositivos",
       .arguments = {{.name = "query",
                      .type = "string",
                      .required = true,
                      .enumValues = {},
                      .description = ""}},
       .accessTable = TableName::Memory,
       .accessPermission = RolePermission::Read,
       .handler = nullptr});
  descriptors.push_back(
      {.name = "procedure.run",
       .description = "Ejecuta un procedimiento conocido para conseguir un "
                      "objetivo",
       .arguments = {{.name = "goal",
                      .type = "string",
                      .required = true,
                      .enumValues = {},
                      .description = ""}},
       .accessTable = TableName::Memory,
       .accessPermission = RolePermission::Read,
       .handler = nullptr});
  descriptors.push_back(
      {.name = "memory.forget",
       .description = "Olvida un hecho guardado por su id",
       .arguments = {{.name = "fact_id",
                      .type = "number",
                      .required = true,
                      .enumValues = {},
                      .description = ""}},
       .accessTable = TableName::Memory,
       .accessPermission = RolePermission::Delete,
       .handler = nullptr});
  return descriptors;
}
