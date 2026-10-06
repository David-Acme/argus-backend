#include "memory-tool-descriptors.hxx"

#include <mcp/schema.hxx>

namespace
{
namespace schema = argus::mcp::schema;

constexpr const char* kCore = "core";
}

std::vector<tools::ToolDescriptor> memoryToolDescriptors()
{
  std::vector<tools::ToolDescriptor> descriptors;
  descriptors.push_back(
      {.spec = {.name = "memory.remember",
                .title = "",
                .description = "Almacena un hecho sobre una persona, dispositivo o "
                               "lugar de la casa. El hecho completo va en el argumento "
                               "text, tal cual lo pidió el usuario",
                .inputSchema = schema::object(
                    {{.name = "text", .schema = schema::text(), .required = false},
                     {.name = "subject", .schema = schema::text(), .required = false},
                     {.name = "predicate", .schema = schema::text(), .required = false},
                     {.name = "value", .schema = schema::text(), .required = false},
                     {.name = "type",
                      .schema = schema::choice({"persona", "preference", "schedule", "instruction", "attribute"}),
                      .required = false},
                     {.name = "confidence", .schema = schema::number(), .required = false}}),
                .annotations = {},
                .module = kCore,
                .capability = "memory.manage"},
       .handler = nullptr});
  descriptors.push_back(
      {.spec = {.name = "memory.remind",
                .title = "",
                .description = "Guarda un recordatorio del usuario que habla: un hecho "
                               "con un momento concreto. Si dice la hora, Argus le "
                               "llama a esa hora para recordárselo",
                .inputSchema = schema::object({{.name = "text", .schema = schema::text(), .required = false},
                                               {.name = "when", .schema = schema::text(), .required = false}}),
                .annotations = {},
                .module = kCore,
                .capability = "reminders.write"},
       .handler = nullptr});
  descriptors.push_back(
      {.spec = {.name = "memory.recall",
                .title = "",
                .description = "Recupera hechos guardados sobre la casa, las personas "
                               "o los dispositivos",
                .inputSchema = schema::object({{.name = "query", .schema = schema::text(), .required = true}}),
                .annotations = {.readOnly = true},
                .module = kCore,
                .capability = "memory.manage"},
       .handler = nullptr});
  descriptors.push_back(
      {.spec = {.name = "memory.forget",
                .title = "",
                .description = "Olvida un hecho guardado que el usuario pide olvidar; "
                               "query describe ese hecho con sus palabras",
                .inputSchema = schema::object({{.name = "query", .schema = schema::text(), .required = true}}),
                .annotations = {.destructive = true},
                .module = kCore,
                .capability = "memory.manage"},
       .handler = nullptr});
  descriptors.push_back(
      {.spec = {.name = "reminder.list",
                .title = "",
                .description = "Lista los recordatorios pendientes del usuario que habla",
                .inputSchema = schema::object(
                    {{.name = "include_done", .schema = schema::boolean(), .required = false},
                     {.name = "limit",
                      .schema = schema::integer({.description = "", .minimum = 1, .maximum = 20}),
                      .required = false}}),
                .annotations = {.readOnly = true},
                .module = kCore,
                .capability = "reminders.read",
                    .policy = {.spanish = "Si pregunta qué recordatorios tiene, llama a reminder.list.",
                               .english = "If they ask which reminders they have, call reminder.list."}},
       .handler = nullptr});
  return descriptors;
}
