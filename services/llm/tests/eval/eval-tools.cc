#include "eval-tools.hxx"

#include <feature/llm/services/tools/core-tools.hxx>
#include <feature/memory/services/memory/memory-tool-descriptors.hxx>

#include <mcp/schema.hxx>

#include <algorithm>
#include <string_view>
#include <utility>

namespace eval
{

namespace
{

namespace schema = argus::mcp::schema;
using argus::mcp::ToolSpec;

constexpr std::string_view kIsoHint = "ISO-8601, hora local, por ejemplo 2026-10-07T15:00:00";

Json::Value moment()
{
  Json::Value property = schema::text({.description = std::string(kIsoHint), .minimum = std::nullopt, .maximum = std::nullopt});
  property["format"] = "date-time";
  return property;
}

Json::Value moduleProperty()
{
  return schema::text({.description = "id del módulo: productivity, surveillance...", .minimum = std::nullopt, .maximum = std::nullopt});
}

Json::Value bounded()
{
  return schema::integer({.description = "", .minimum = 1, .maximum = 20});
}

struct Declared
{
  std::string name;
  std::string description;
  Json::Value inputSchema;
  argus::mcp::ToolAnnotations annotations;
  std::string module;
  std::string capability;
};

ToolSpec specOf(Declared declared)
{
  return {.name = std::move(declared.name),
          .title = "",
          .description = std::move(declared.description),
          .inputSchema = std::move(declared.inputSchema),
          .annotations = declared.annotations,
          .module = std::move(declared.module),
          .capability = std::move(declared.capability)};
}

std::vector<ToolSpec> mirroredProviders()
{
  std::vector<ToolSpec> out;
  out.push_back(specOf({.name = "calendar.list_events",
                        .description = "Lista los eventos de la agenda del usuario entre dos momentos (por defecto, los "
                                       "próximos 7 días). from y to son fecha y hora",
                        .inputSchema = schema::object({{.name = "from", .schema = moment(), .required = false},
                                                       {.name = "to", .schema = moment(), .required = false},
                                                       {.name = "limit", .schema = bounded(), .required = false}}),
                        .annotations = {.readOnly = true},
                        .module = "productivity",
                        .capability = "agenda.read"}));
  out.push_back(specOf({.name = "calendar.create_event",
                        .description = "Agenda un evento en el calendario del usuario. title es el nombre; starts_at el día y la "
                                       "hora de inicio",
                        .inputSchema = schema::object({{.name = "title", .schema = schema::text(), .required = true},
                                                       {.name = "starts_at", .schema = moment(), .required = true},
                                                       {.name = "ends_at", .schema = moment(), .required = false},
                                                       {.name = "location", .schema = schema::text(), .required = false},
                                                       {.name = "description", .schema = schema::text(), .required = false},
                                                       {.name = "all_day", .schema = schema::boolean(), .required = false}}),
                        .annotations = {},
                        .module = "productivity",
                        .capability = "agenda.write"}));
  out.push_back(specOf({.name = "calendar.cancel_event",
                        .description = "Cancela un evento de la agenda. Indica event_id o el title del evento. Primero devuelve "
                                       "qué cancelaría y un código; con el sí del usuario se repite la llamada con confirmation",
                        .inputSchema = schema::object({{.name = "event_id", .schema = schema::integer(), .required = false},
                                                       {.name = "title", .schema = schema::text(), .required = false},
                                                       {.name = "confirmation", .schema = schema::text(), .required = false}}),
                        .annotations = {.destructive = true},
                        .module = "productivity",
                        .capability = "agenda.write"}));
  out.push_back(specOf({.name = "project.list",
                        .description = "Lista los proyectos del usuario (por defecto, los que siguen abiertos)",
                        .inputSchema = schema::object(
                            {{.name = "status", .schema = schema::choice({"planned", "active", "paused", "done", "canceled"}), .required = false},
                             {.name = "limit", .schema = bounded(), .required = false}}),
                        .annotations = {.readOnly = true},
                        .module = "productivity",
                        .capability = "projects.read"}));
  out.push_back(specOf({.name = "project.create",
                        .description = "Crea un proyecto del usuario. name es el nombre",
                        .inputSchema = schema::object({{.name = "name", .schema = schema::text(), .required = true},
                                                       {.name = "description", .schema = schema::text(), .required = false},
                                                       {.name = "target_at", .schema = moment(), .required = false}}),
                        .annotations = {},
                        .module = "productivity",
                        .capability = "projects.write"}));
  out.push_back(specOf({.name = "task.list",
                        .description = "Lista las tareas pendientes del usuario, de todos sus proyectos o de uno (project)",
                        .inputSchema = schema::object({{.name = "project", .schema = schema::text(), .required = false},
                                                       {.name = "project_id", .schema = schema::integer(), .required = false},
                                                       {.name = "limit", .schema = bounded(), .required = false}}),
                        .annotations = {.readOnly = true},
                        .module = "productivity",
                        .capability = "projects.read"}));
  out.push_back(specOf({.name = "task.create",
                        .description = "Agrega una tarea a un proyecto del usuario. title es la tarea; project el proyecto "
                                       "(si el usuario solo tiene uno abierto, se usa ese)",
                        .inputSchema = schema::object(
                            {{.name = "title", .schema = schema::text(), .required = true},
                             {.name = "project", .schema = schema::text(), .required = false},
                             {.name = "project_id", .schema = schema::integer(), .required = false},
                             {.name = "priority", .schema = schema::choice({"none", "low", "medium", "high", "urgent"}), .required = false},
                             {.name = "due_at", .schema = moment(), .required = false}}),
                        .annotations = {},
                        .module = "productivity",
                        .capability = "projects.write"}));
  out.push_back(specOf({.name = "task.complete",
                        .description = "Marca una tarea pendiente como hecha. Indica task_id o el title de la tarea",
                        .inputSchema = schema::object({{.name = "task_id", .schema = schema::integer(), .required = false},
                                                       {.name = "title", .schema = schema::text(), .required = false}}),
                        .annotations = {.idempotent = true},
                        .module = "productivity",
                        .capability = "projects.write"}));
  out.push_back(specOf({.name = "app.show_camera",
                        .description = "Muestra en la app del usuario la imagen en vivo de una cámara, o una foto reciente si "
                                       "view es snapshot. El argumento camera es el nombre de la cámara; vacío muestra la del "
                                       "último aviso",
                        .inputSchema = schema::object({{.name = "camera", .schema = schema::text(), .required = false},
                                                       {.name = "view", .schema = schema::choice({"live", "snapshot"}), .required = false}}),
                        .annotations = {.readOnly = true},
                        .module = "surveillance",
                        .capability = "camera.view"}));
  out.push_back(specOf({.name = "app.set_guard_mode",
                        .description = "Cambia el modo de vigilancia: home al estar en casa, night al dormir, away al salir, "
                                       "armed para máxima alerta. environment es el nombre del lugar (casa, restaurante, "
                                       "oficina...) si el usuario nombra uno; vacío cambia todos",
                        .inputSchema = schema::object({{.name = "mode", .schema = schema::choice({"home", "night", "away", "armed"}), .required = true},
                                                       {.name = "environment", .schema = schema::text(), .required = false}}),
                        .annotations = {.idempotent = true},
                        .module = "surveillance",
                        .capability = "guard.mode.set"}));
  out.push_back(specOf({.name = "modules.list",
                        .description = "Lista los módulos de Argus y si están activos, apagados o próximamente",
                        .inputSchema = schema::emptyObject(),
                        .annotations = {.readOnly = true},
                        .module = "core",
                        .capability = "modules.read"}));
  out.push_back(specOf({.name = "modules.explain",
                        .description = "Explica qué es un módulo de Argus, con ejemplos, y si está activo",
                        .inputSchema = schema::object({{.name = "module", .schema = moduleProperty(), .required = true}}),
                        .annotations = {.readOnly = true},
                        .module = "core",
                        .capability = "modules.read"}));
  out.push_back(specOf({.name = "modules.request",
                        .description = "Pide al dueño de la casa que active un módulo apagado; solo con el sí del usuario",
                        .inputSchema = schema::object({{.name = "module", .schema = moduleProperty(), .required = true}}),
                        .annotations = {},
                        .module = "core",
                        .capability = "modules.request"}));
  out.push_back(specOf({.name = "modules.enable",
                        .description = "Activa un módulo apagado (el dueño); solo con el sí del usuario",
                        .inputSchema = schema::object({{.name = "module", .schema = moduleProperty(), .required = true}}),
                        .annotations = {.idempotent = true},
                        .module = "core",
                        .capability = "modules.manage"}));
  out.push_back(specOf({.name = "modules.disable",
                        .description = "Apaga un módulo (el dueño). Primero explica qué se detendría y devuelve un código; con "
                                       "el sí del usuario se repite la llamada con confirmation",
                        .inputSchema = schema::object({{.name = "module", .schema = moduleProperty(), .required = true},
                                                       {.name = "confirmation", .schema = schema::text(), .required = false}}),
                        .annotations = {.destructive = true},
                        .module = "core",
                        .capability = "modules.manage"}));
  out.push_back(specOf({.name = "modules.open_purge_screen",
                        .description = "Abre en la app la pantalla donde el dueño puede borrar los datos de un módulo; por voz "
                                       "nunca se borran datos",
                        .inputSchema = schema::object({{.name = "module", .schema = moduleProperty(), .required = true}}),
                        .annotations = {},
                        .module = "core",
                        .capability = "modules.manage"}));
  return out;
}

std::string targetOf(const tools::ToolCall& call)
{
  for (const char* key : {"module", "title", "event_id"}) {
    if (call.arguments.isMember(key))
      return call.arguments[key].asString();
  }
  return {};
}

std::string_view outputFor(const tools::ToolCall& call)
{
  const bool english = call.context.lang == "en";
  const std::string& tool = call.name;
  if (tool == "memory.recall")
    return english ? "I have nothing saved about that." : "No tengo nada guardado sobre eso.";
  if (tool == "reminder.list" || tool == "calendar.list_events" || tool == "task.list" || tool == "project.list")
    return english ? "There is nothing pending." : "No hay nada pendiente.";
  if (tool == "modules.list")
    return english ? "core active; productivity and surveillance active."
                   : "core activo; productivity y surveillance activos.";
  if (tool == "modules.explain")
    return english ? "It is an Argus module; it is active." : "Es un módulo de Argus; está activo.";
  return english ? "Done." : "Hecho.";
}

std::function<tools::ToolResult(const tools::ToolCall&)> handlerFor(const ToolSpec& spec, const StubInput& input)
{
  const bool destructive = spec.annotations.destructive && spec.inputSchema["properties"].isMember("confirmation");
  return [input, destructive](const tools::ToolCall& call) {
    tools::ToolResult result;
    result.tool = call.name;
    if (!destructive) {
      input.recorder->ran({.tool = call.name, .arguments = call.arguments});
      result.ok = true;
      result.output = outputFor(call);
      return result;
    }
    const argus::mcp::ConfirmationKey key{.userId = call.context.userId, .tool = call.name, .target = targetOf(call)};
    const std::string token = call.arguments.get("confirmation", "").asString();
    if (token.empty()) {
      const std::string code = input.ledger->issue(key);
      input.recorder->previewed(call.name);
      result.ok = true;
      result.output = (call.context.lang == "en" ? "This would stop or cancel «" : "Esto detendría o cancelaría «") +
                      key.target + "».";
      result.data["needsConfirmation"] = true;
      result.data["confirmation"] = code;
      return result;
    }
    if (!input.ledger->consume(key, token)) {
      result.code = "confirmation_invalid";
      result.output = call.context.lang == "en"
                          ? "That code is not valid (used, expired or for another target). Ask the user to confirm again."
                          : "Ese código no vale (ya se usó, venció o es de otro objetivo). Vuelve a pedirle la confirmación al usuario.";
      return result;
    }
    input.recorder->confirmed(call.name);
    input.recorder->ran({.tool = call.name, .arguments = call.arguments});
    result.ok = true;
    result.output = call.context.lang == "en" ? "Done." : "Hecho.";
    return result;
  };
}

}

void Recorder::clear()
{
  const std::scoped_lock lock(mutex_);
  executed_.clear();
  previews_.clear();
  confirmed_.clear();
}

void Recorder::ran(const RecordedCall& call)
{
  const std::scoped_lock lock(mutex_);
  executed_.push_back(call);
}

void Recorder::previewed(const std::string& tool)
{
  const std::scoped_lock lock(mutex_);
  previews_.push_back(tool);
}

void Recorder::confirmed(const std::string& tool)
{
  const std::scoped_lock lock(mutex_);
  confirmed_.push_back(tool);
}

std::vector<RecordedCall> Recorder::executed() const
{
  const std::scoped_lock lock(mutex_);
  return executed_;
}

std::vector<std::string> Recorder::previews() const
{
  const std::scoped_lock lock(mutex_);
  return previews_;
}

std::vector<std::string> Recorder::confirmations() const
{
  const std::scoped_lock lock(mutex_);
  return confirmed_;
}

std::vector<tools::ToolDescriptor> stubTools(const StubInput& input)
{
  std::vector<ToolSpec> specs = mirroredProviders();
  specs.push_back(appOpenSpec());
  std::vector<tools::ToolDescriptor> out;
  for (auto& descriptor : memoryToolDescriptors())
    specs.push_back(std::move(descriptor.spec));
  for (auto& spec : specs) {
    auto handler = handlerFor(spec, input);
    out.push_back({.spec = std::move(spec), .handler = std::move(handler)});
  }
  return out;
}

}
