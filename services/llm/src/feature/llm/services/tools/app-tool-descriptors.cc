#include "app-tool-descriptors.hxx"

#include <string>
#include <utility>

namespace
{
constexpr std::string_view kAppPrefix = "app.";

struct AppActionInput
{
  const tools::ToolCall& call;
  std::string spoken;
};

tools::ToolResult emit(const AppActionInput& input)
{
  tools::ToolResult result;
  result.tool = input.call.name;
  if (!input.call.context.emitAction) {
    result.output = "la app no está conectada a esta conversación";
    return result;
  }
  input.call.context.emitAction(input.call.name, input.call.arguments);
  result.ok = true;
  result.output = input.spoken;
  result.data = input.call.arguments;
  return result;
}

tools::ToolArgumentSpec argument(std::string name, std::string type, std::vector<std::string> values)
{
  return {.name = std::move(name),
          .type = std::move(type),
          .required = true,
          .enumValues = std::move(values),
          .description = ""};
}
}

bool isAppTool(std::string_view name)
{
  return name.starts_with(kAppPrefix);
}

std::vector<tools::ToolDescriptor> appToolDescriptors()
{
  std::vector<tools::ToolDescriptor> descriptors;
  descriptors.push_back(
      {.name = "app.show_camera",
       .description = "Muestra en la app del usuario la imagen en vivo de una cámara. El argumento "
                      "camera es el nombre de la cámara; vacío muestra la del último aviso",
       .arguments = {{.name = "camera", .type = "string", .required = false, .enumValues = {}, .description = ""}},
       .accessTable = TableName::Camera,
       .accessPermission = RolePermission::Read,
       .handler = [](const tools::ToolCall& call) {
         return emit({.call = call, .spoken = "La app está mostrando la cámara."});
       }});
  descriptors.push_back(
      {.name = "app.open",
       .description = "Abre una sección de la app del usuario",
       .arguments = {argument("screen", "enum",
                              {"home", "agenda", "projects", "cameras", "security", "people", "settings"})},
       .accessTable = TableName::Notification,
       .accessPermission = RolePermission::Read,
       .handler = [](const tools::ToolCall& call) {
         return emit({.call = call, .spoken = "La app abrió la sección pedida."});
       }});
  descriptors.push_back(
      {.name = "app.set_guard_mode",
       .description = "Cambia el modo de vigilancia de la casa: home al estar en casa, night al dormir, "
                      "away al salir, armed para máxima alerta",
       .arguments = {argument("mode", "enum", {"home", "night", "away", "armed"})},
       .accessTable = TableName::Camera,
       .accessPermission = RolePermission::Update,
       .handler = [](const tools::ToolCall& call) {
         return emit({.call = call, .spoken = "La app cambió el modo de vigilancia."});
       }});
  return descriptors;
}
