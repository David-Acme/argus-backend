#include "productivity-tools.hxx"

#include "calendar-tools.hxx"
#include "planner-tools.hxx"

#include <auth/tool-gate.hxx>
#include <mcp/confirmation.hxx>
#include <mcp/loop-tool.hxx>
#include <mcp/schema.hxx>

#include <utility>

namespace
{
namespace schema = argus::mcp::schema;

constexpr const char* kProductivity = "productivity";

Json::Value moment(const std::string& description)
{
  Json::Value property = schema::text({.description = description, .minimum = std::nullopt, .maximum = std::nullopt});
  property["format"] = "date-time";
  return property;
}

constexpr const char* kIsoHint = "ISO-8601, hora local, por ejemplo 2026-10-07T15:00:00";

argus::mcp::ToolSpec spec(argus::mcp::ToolSpec base)
{
  base.module = kProductivity;
  return base;
}

template <class Handler>
argus::mcp::McpServer::Handler withLoop(const ProductivityToolsInput& input, Handler handler)
{
  return argus::mcp::onLoop({.loop = input.loop, .handler = std::move(handler)});
}
}

std::shared_ptr<argus::mcp::McpServer> productivityToolServer(const ProductivityToolsInput& input)
{
  auto server = std::make_shared<argus::mcp::McpServer>(
      argus::mcp::ServerIdentity{.name = "argus-productivity", .version = "1", .instructions = ""});
  const auto ledger = std::make_shared<argus::mcp::ConfirmationLedger>();
  using productivity_tools::cancelEvent;
  using productivity_tools::completeTask;
  using productivity_tools::createEvent;
  using productivity_tools::createProject;
  using productivity_tools::createTask;
  using productivity_tools::listEvents;
  using productivity_tools::listProjects;
  using productivity_tools::listTasks;

  server->add(spec({.name = "calendar.list_events",
                    .title = "",
                    .description = "Lista los eventos de la agenda del usuario entre dos momentos (por defecto, los "
                                   "próximos 7 días). from y to son fecha y hora",
                    .inputSchema = schema::object({{.name = "from", .schema = moment(kIsoHint), .required = false},
                                                   {.name = "to", .schema = moment(kIsoHint), .required = false},
                                                   {.name = "limit", .schema = schema::integer({.description = "", .minimum = 1, .maximum = 20}), .required = false}}),
                    .annotations = {.readOnly = true},
                    .capability = "agenda.read"}),
              withLoop(input, listEvents));
  server->add(spec({.name = "calendar.create_event",
                    .title = "",
                    .description = "Agenda un evento en el calendario del usuario. title es el nombre; starts_at el día y la "
                                   "hora de inicio",
                    .inputSchema = schema::object({{.name = "title", .schema = schema::text(), .required = true},
                                                   {.name = "starts_at", .schema = moment(kIsoHint), .required = true},
                                                   {.name = "ends_at", .schema = moment(kIsoHint), .required = false},
                                                   {.name = "location", .schema = schema::text(), .required = false},
                                                   {.name = "description", .schema = schema::text(), .required = false},
                                                   {.name = "all_day", .schema = schema::boolean(), .required = false}}),
                    .annotations = {},
                    .capability = "agenda.write"}),
              withLoop(input, createEvent));
  server->add(spec({.name = "calendar.cancel_event",
                    .title = "",
                    .description = "Cancela un evento de la agenda. Indica event_id o el title del evento. Primero devuelve "
                                   "qué cancelaría y un código; con el sí del usuario se repite la llamada con confirmation",
                    .inputSchema = schema::object({{.name = "event_id", .schema = schema::integer(), .required = false},
                                                   {.name = "title", .schema = schema::text(), .required = false},
                                                   {.name = "confirmation", .schema = schema::text(), .required = false}}),
                    .annotations = {.destructive = true},
                    .capability = "agenda.write"}),
              withLoop(input, [ledger](const argus::mcp::ToolInvocation& invocation) {
                return cancelEvent({.invocation = invocation, .ledger = ledger});
              }));
  server->add(spec({.name = "project.list",
                    .title = "",
                    .description = "Lista los proyectos del usuario (por defecto, los que siguen abiertos)",
                    .inputSchema = schema::object(
                        {{.name = "status", .schema = schema::choice({"planned", "active", "paused", "done", "canceled"}), .required = false},
                         {.name = "limit", .schema = schema::integer({.description = "", .minimum = 1, .maximum = 20}), .required = false}}),
                    .annotations = {.readOnly = true},
                    .capability = "projects.read"}),
              withLoop(input, listProjects));
  server->add(spec({.name = "project.create",
                    .title = "",
                    .description = "Crea un proyecto del usuario. name es el nombre",
                    .inputSchema = schema::object({{.name = "name", .schema = schema::text(), .required = true},
                                                   {.name = "description", .schema = schema::text(), .required = false},
                                                   {.name = "target_at", .schema = moment(kIsoHint), .required = false}}),
                    .annotations = {},
                    .capability = "projects.write"}),
              withLoop(input, createProject));
  server->add(spec({.name = "task.list",
                    .title = "",
                    .description = "Lista las tareas pendientes del usuario, de todos sus proyectos o de uno (project)",
                    .inputSchema = schema::object({{.name = "project", .schema = schema::text(), .required = false},
                                                   {.name = "project_id", .schema = schema::integer(), .required = false},
                                                   {.name = "limit", .schema = schema::integer({.description = "", .minimum = 1, .maximum = 20}), .required = false}}),
                    .annotations = {.readOnly = true},
                    .capability = "projects.read"}),
              withLoop(input, listTasks));
  server->add(spec({.name = "task.create",
                    .title = "",
                    .description = "Agrega una tarea a un proyecto del usuario. title es la tarea; project el proyecto "
                                   "(si el usuario solo tiene uno abierto, se usa ese)",
                    .inputSchema = schema::object(
                        {{.name = "title", .schema = schema::text(), .required = true},
                         {.name = "project", .schema = schema::text(), .required = false},
                         {.name = "project_id", .schema = schema::integer(), .required = false},
                         {.name = "priority", .schema = schema::choice({"none", "low", "medium", "high", "urgent"}), .required = false},
                         {.name = "due_at", .schema = moment(kIsoHint), .required = false}}),
                    .annotations = {},
                    .capability = "projects.write"}),
              withLoop(input, createTask));
  server->add(spec({.name = "task.complete",
                    .title = "",
                    .description = "Marca una tarea pendiente como hecha. Indica task_id o el title de la tarea",
                    .inputSchema = schema::object({{.name = "task_id", .schema = schema::integer(), .required = false},
                                                   {.name = "title", .schema = schema::text(), .required = false}}),
                    .annotations = {.idempotent = true},
                    .capability = "projects.write"}),
              withLoop(input, completeTask));
  server->setGate(tool_gate::capabilities());
  return server;
}
