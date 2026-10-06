#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tool-stubs.hxx"

#include <feature/llm/services/tools/tool-executor.hxx>
#include <feature/llm/services/tools/tool-policy.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <feature/memory/services/memory/memory-tool-descriptors.hxx>

#include <string>
#include <vector>

namespace
{
struct Stub
{
  std::string name;
  std::string module;
  argus::mcp::ToolPolicy policy;
};

tools::ToolHandle handleOf(const Stub& input)
{
  return std::make_shared<const tools::ToolDescriptor>(
      tool_stubs::stub({.name = input.name,
                        .capability = "agenda.read",
                        .handler = nullptr,
                        .module = input.module,
                        .schema = argus::mcp::schema::emptyObject(),
                        .destructive = false,
                        .policy = input.policy}));
}

std::vector<tools::ToolHandle> agenda()
{
  return {handleOf({.name = "calendar.create_event",
                    .module = "productivity",
                    .policy = {.spanish = "Para agendar usa calendar.create_event.", .english = "To schedule use calendar.create_event."}}),
          handleOf({.name = "calendar.list_events",
                    .module = "productivity",
                    .policy = {.spanish = "Para ver la agenda usa calendar.list_events.", .english = ""}}),
          handleOf({.name = "task.create",
                    .module = "productivity",
                    .policy = {.spanish = "Para ver la agenda usa calendar.list_events.", .english = "To add a task use task.create."}})};
}

std::size_t countOf(const std::string& text, const std::string& part)
{
  std::size_t found = 0;
  for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size()))
    ++found;
  return found;
}
}

TEST_CASE("each offered tool's policy line is said once, in the user's language, with the two generic rules")
{
  const auto tools = agenda();
  const std::string spanish = tool_policy::generated({.tools = tools, .lang = "es"});
  CHECK(spanish.find("Para agendar usa calendar.create_event.") != std::string::npos);
  CHECK(countOf(spanish, "Para ver la agenda usa calendar.list_events.") == 1);
  CHECK(spanish.find("Si una herramienta responde que el módulo está apagado, díselo al usuario y ofrécele activarlo.") != std::string::npos);
  CHECK(spanish.find("Nunca digas que agendaste") != std::string::npos);
  CHECK(spanish.find("To schedule") == std::string::npos);

  const std::string english = tool_policy::generated({.tools = tools, .lang = "en"});
  CHECK(english.find("To schedule use calendar.create_event.") != std::string::npos);
  CHECK(english.find("To add a task use task.create.") != std::string::npos);
  CHECK(english.find("Para ver la agenda usa calendar.list_events.") != std::string::npos);
  CHECK(english.find("If a tool answers that the module is off, tell the user and offer to turn it on.") != std::string::npos);
  CHECK(english.find("Never say you scheduled") != std::string::npos);
}

TEST_CASE("only the tools offered this turn contribute and a tool with no policy adds nothing")
{
  auto tools = agenda();
  tools.erase(tools.begin());
  tools.push_back(handleOf({.name = "probe.silent", .module = "productivity", .policy = {}}));
  const std::string text = tool_policy::generated({.tools = tools, .lang = "es"});
  CHECK(text.find("Para agendar") == std::string::npos);
  CHECK(text.find("calendar.list_events") != std::string::npos);
  CHECK(tool_policy::generated({.tools = {}, .lang = "es"}).empty());
  const std::vector<tools::ToolHandle> silent{handleOf({.name = "probe.silent", .module = "core", .policy = {}})};
  CHECK(tool_policy::generated({.tools = silent, .lang = "es"}).empty());
}

TEST_CASE("the module-off rule needs a tool that belongs to a module")
{
  const std::vector<tools::ToolHandle> core{
      handleOf({.name = "modules.list", .module = "core", .policy = {.spanish = "Para ver los módulos usa modules.list.", .english = ""}})};
  const std::string text = tool_policy::generated({.tools = core, .lang = "es"});
  CHECK(text.find("Para ver los módulos usa modules.list.") != std::string::npos);
  CHECK(text.find("apagado") == std::string::npos);
  CHECK(text.find("Nunca digas que agendaste") != std::string::npos);
}

TEST_CASE("the system prompt keeps the memory policy first, then the generated lines, then the client actions")
{
  const auto tools = agenda();
  const std::string plain = tool_policy::systemPrompt({.tools = tools, .lang = "es", .clientActions = false});
  CHECK(plain.rfind("Eres Argus. Usa memory.remember", 0) == 0);
  CHECK(plain.find("Si no hace falta ninguna, responde brevemente. Para agendar usa calendar.create_event.") != std::string::npos);
  CHECK(plain.find("app.show_camera") == std::string::npos);

  const std::string call = tool_policy::systemPrompt({.tools = tools, .lang = "es", .clientActions = true});
  CHECK(call.size() > plain.size());
  CHECK(call.rfind(plain, 0) == 0);
  CHECK(call.find("app.show_camera") > call.find("calendar.create_event"));

  const std::string bare = tool_policy::systemPrompt({.tools = {}, .lang = "es", .clientActions = false});
  CHECK(bare.find("Nunca digas que agendaste") == std::string::npos);
  CHECK(bare.rfind("Eres Argus.", 0) == 0);
}

TEST_CASE("the core descriptors that carry a policy reach the prompt for a role that may use them")
{
  ToolRegistry registry;
  for (auto descriptor : memoryToolDescriptors())
    registry.registerTool(std::move(descriptor));
  const ToolExecutor executor(registry);
  const auto offered = executor.offered({.role = UserRole::Resident, .modules = ModuleSnapshot()});
  const std::string text = tool_policy::generated({.tools = offered, .lang = "es"});
  CHECK(text.find("llama a reminder.list") != std::string::npos);
  CHECK(text.find("apagado") == std::string::npos);
}
