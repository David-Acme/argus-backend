#include "app-tool-descriptors.hxx"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace
{
constexpr std::string_view kAppPrefix = "app.";

struct AppActionInput
{
  const tools::ToolCall& call;
  std::string spokenEs;
  std::string spokenEn;
};

tools::ToolResult emit(const AppActionInput& input)
{
  const bool english = input.call.context.lang == "en";
  tools::ToolResult result;
  result.tool = input.call.name;
  if (!input.call.context.emitAction) {
    result.output = english ? "The app is not connected to this conversation."
                            : "La app no está conectada a esta conversación.";
    return result;
  }
  input.call.context.emitAction(input.call.name, input.call.arguments);
  result.ok = true;
  result.output = english ? input.spokenEn : input.spokenEs;
  result.data = input.call.arguments;
  return result;
}

struct Label
{
  std::string_view value;
  std::string_view es;
  std::string_view en;
};

constexpr std::array<Label, 4> kModeLabels{{{.value = "home", .es = "en casa", .en = "home"},
                                            {.value = "night", .es = "noche", .en = "night"},
                                            {.value = "away", .es = "fuera de casa", .en = "away"},
                                            {.value = "armed", .es = "máxima alerta", .en = "armed"}}};

constexpr std::array<Label, 7> kScreenLabels{{{.value = "home", .es = "el inicio", .en = "the home screen"},
                                              {.value = "agenda", .es = "la agenda", .en = "the agenda"},
                                              {.value = "projects", .es = "los proyectos", .en = "the projects"},
                                              {.value = "cameras", .es = "las cámaras", .en = "the cameras"},
                                              {.value = "security", .es = "la seguridad", .en = "security"},
                                              {.value = "people", .es = "las personas", .en = "the people"},
                                              {.value = "settings", .es = "los ajustes", .en = "the settings"}}};

template <size_t N>
std::string labelFor(const std::array<Label, N>& labels, const tools::ToolCall& call, const char* argument)
{
  const std::string value = call.arguments.get(argument, "").asString();
  const auto found = std::ranges::find(labels, std::string_view(value), &Label::value);
  if (found == labels.end())
    return value;
  return std::string(call.context.lang == "en" ? found->en : found->es);
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
         const std::string camera = call.arguments.get("camera", "").asString();
         return emit({.call = call,
                      .spokenEs = camera.empty() ? "La app está mostrando la cámara del último aviso."
                                                 : "La app está mostrando la cámara " + camera + ".",
                      .spokenEn = camera.empty() ? "The app is showing the camera from the last notice."
                                                 : "The app is showing the " + camera + " camera."});
       }});
  descriptors.push_back(
      {.name = "app.open",
       .description = "Abre una sección de la app del usuario",
       .arguments = {argument("screen", "enum",
                              {"home", "agenda", "projects", "cameras", "security", "people", "settings"})},
       .accessTable = TableName::Notification,
       .accessPermission = RolePermission::Read,
       .handler = [](const tools::ToolCall& call) {
         const std::string screen = labelFor(kScreenLabels, call, "screen");
         return emit({.call = call,
                      .spokenEs = "La app abrió " + screen + ".",
                      .spokenEn = "The app opened " + screen + "."});
       }});
  descriptors.push_back(
      {.name = "app.set_guard_mode",
       .description = "Cambia el modo de vigilancia: home al estar en casa, night al dormir, away al "
                      "salir, armed para máxima alerta. environment es el nombre del lugar (casa, "
                      "restaurante, oficina...) si el usuario nombra uno; vacío cambia todos",
       .arguments = {argument("mode", "enum", {"home", "night", "away", "armed"}),
                     {.name = "environment",
                      .type = "string",
                      .required = false,
                      .enumValues = {},
                      .description = ""}},
       .accessTable = TableName::Camera,
       .accessPermission = RolePermission::Update,
       .handler = [](const tools::ToolCall& call) {
         const std::string mode = labelFor(kModeLabels, call, "mode");
         const std::string place = call.arguments.get("environment", "").asString();
         if (!place.empty())
           return emit({.call = call,
                        .spokenEs = "La app puso la vigilancia de " + place + " en modo " + mode + ".",
                        .spokenEn = "The app set the guard mode of " + place + " to " + mode + "."});
         return emit({.call = call,
                      .spokenEs = "La app puso la vigilancia en modo " + mode + ".",
                      .spokenEn = "The app set the guard mode to " + mode + "."});
       }});
  return descriptors;
}
