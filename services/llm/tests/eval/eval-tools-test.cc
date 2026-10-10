#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "eval-tools.hxx"

#include <mcp/confirmation.hxx>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{

eval::StubInput stubs()
{
  return {.recorder = std::make_shared<eval::Recorder>(),
          .ledger = std::make_shared<argus::mcp::ConfirmationLedger>()};
}

struct Call
{
  std::string_view tool;
  std::string_view lang;
  std::string_view target;
};

tools::ToolCall callFor(const Call& input)
{
  tools::ToolCall call;
  call.name = std::string(input.tool);
  call.arguments = Json::Value(Json::objectValue);
  call.arguments["module"] = std::string(input.target);
  call.arguments["title"] = std::string(input.target);
  call.context.lang = std::string(input.lang);
  call.context.userId = 7;
  return call;
}

const tools::ToolDescriptor* find(const std::vector<tools::ToolDescriptor>& tools, std::string_view name)
{
  const auto found = std::ranges::find(tools, name, [](const tools::ToolDescriptor& descriptor) {
    return std::string_view(descriptor.spec.name);
  });
  return found == tools.end() ? nullptr : &*found;
}

struct Expected
{
  std::string_view tool;
  std::string_view en;
  std::string_view es;
};

constexpr std::array kOutcomes{
    Expected{.tool = "memory.recall", .en = "I have nothing saved about that.", .es = "No tengo nada guardado sobre eso."},
    Expected{.tool = "calendar.list_events", .en = "There is nothing pending.", .es = "No hay nada pendiente."},
    Expected{.tool = "project.list", .en = "There is nothing pending.", .es = "No hay nada pendiente."},
    Expected{.tool = "task.list", .en = "There is nothing pending.", .es = "No hay nada pendiente."},
    Expected{.tool = "reminder.list", .en = "There is nothing pending.", .es = "No hay nada pendiente."},
    Expected{.tool = "modules.list",
             .en = "core active; productivity and surveillance active.",
             .es = "core activo; productivity y surveillance activos."},
    Expected{.tool = "modules.explain", .en = "It is an Argus module; it is active.", .es = "Es un módulo de Argus; está activo."},
    Expected{.tool = "app.open", .en = "Done.", .es = "Hecho."},
    Expected{.tool = "app.show_camera", .en = "Done.", .es = "Hecho."},
    Expected{.tool = "app.set_guard_mode", .en = "Done.", .es = "Hecho."},
    Expected{.tool = "calendar.create_event", .en = "Done.", .es = "Hecho."},
    Expected{.tool = "project.create", .en = "Done.", .es = "Hecho."},
    Expected{.tool = "task.create", .en = "Done.", .es = "Hecho."},
    Expected{.tool = "task.complete", .en = "Done.", .es = "Hecho."},
    Expected{.tool = "modules.enable", .en = "Done.", .es = "Hecho."},
    Expected{.tool = "modules.request", .en = "Done.", .es = "Hecho."},
    Expected{.tool = "modules.open_purge_screen", .en = "Done.", .es = "Hecho."},
    Expected{.tool = "memory.remember", .en = "Done.", .es = "Hecho."},
    Expected{.tool = "memory.remind", .en = "Done.", .es = "Hecho."},
    Expected{.tool = "memory.forget", .en = "Done.", .es = "Hecho."},
};

}

TEST_CASE("a stubbed confirmation previews in the invocation's language")
{
  const eval::StubInput input = stubs();
  const auto tools = eval::stubTools(input);
  const tools::ToolDescriptor* cancel = find(tools, "calendar.cancel_event");
  REQUIRE(cancel != nullptr);
  const auto en = cancel->handler(callFor({.tool = "calendar.cancel_event", .lang = "en", .target = "dentist appointment"}));
  const auto es = cancel->handler(callFor({.tool = "calendar.cancel_event", .lang = "es", .target = "cita con el dentista"}));
  CHECK(en.output == "This would stop or cancel «dentist appointment».");
  CHECK(es.output == "Esto detendría o cancelaría «cita con el dentista».");
  CHECK(en.data["needsConfirmation"].asBool());
  CHECK(es.data["needsConfirmation"].asBool());
}

TEST_CASE("a stubbed module disable previews in the invocation's language")
{
  const eval::StubInput input = stubs();
  const auto tools = eval::stubTools(input);
  const tools::ToolDescriptor* disable = find(tools, "modules.disable");
  REQUIRE(disable != nullptr);
  CHECK(disable->handler(callFor({.tool = "modules.disable", .lang = "en", .target = "surveillance"})).output ==
        "This would stop or cancel «surveillance».");
  CHECK(disable->handler(callFor({.tool = "modules.disable", .lang = "es", .target = "surveillance"})).output ==
        "Esto detendría o cancelaría «surveillance».");
}

TEST_CASE("a stubbed invalid confirmation is refused in the invocation's language")
{
  const eval::StubInput input = stubs();
  const auto tools = eval::stubTools(input);
  const tools::ToolDescriptor* cancel = find(tools, "calendar.cancel_event");
  REQUIRE(cancel != nullptr);
  const auto refused = [cancel](std::string_view lang) {
    tools::ToolCall call = callFor({.tool = "calendar.cancel_event", .lang = lang, .target = "dentist appointment"});
    call.arguments["confirmation"] = "000000";
    return cancel->handler(call);
  };
  const auto en = refused("en");
  const auto es = refused("es");
  CHECK(en.code == "confirmation_invalid");
  CHECK(es.code == "confirmation_invalid");
  CHECK(en.output.find("That code is not valid") != std::string::npos);
  CHECK(es.output.find("Ese código no vale") != std::string::npos);
}

TEST_CASE("every stubbed tool outcome is in the invocation's language")
{
  const eval::StubInput input = stubs();
  const auto tools = eval::stubTools(input);
  for (const Expected& row : kOutcomes) {
    const tools::ToolDescriptor* descriptor = find(tools, row.tool);
    REQUIRE_MESSAGE(descriptor != nullptr, row.tool);
    CHECK(descriptor->handler(callFor({.tool = row.tool, .lang = "en", .target = "objetivo"})).output == row.en);
    CHECK(descriptor->handler(callFor({.tool = row.tool, .lang = "es", .target = "objetivo"})).output == row.es);
  }
}
