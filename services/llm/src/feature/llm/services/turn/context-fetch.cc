#include "context-fetch.hxx"

#include <feature/llm/services/tools/tool-executor.hxx>
#include <feature/llm/services/turn/tool-effects.hxx>

#include <text/iso-time.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <exception>

namespace turn
{

namespace
{

struct FacetTool
{
  ContextFacet facet;
  std::string_view tool;
};

constexpr std::array<FacetTool, 7> kFacetTools{{{.facet = ContextFacet::Agenda, .tool = "calendar.list_events"},
                                                {.facet = ContextFacet::Reminders, .tool = "reminder.list"},
                                                {.facet = ContextFacet::Tasks, .tool = "task.list"},
                                                {.facet = ContextFacet::Projects, .tool = "project.list"},
                                                {.facet = ContextFacet::Modules, .tool = "modules.list"},
                                                {.facet = ContextFacet::Camera, .tool = "camera.status"},
                                                {.facet = ContextFacet::Guard, .tool = "guard.status"}}};

constexpr int64_t kDaySeconds = 24LL * 3600;
constexpr int kListLimit = 5;
constexpr std::string_view kEllipsis = "…";

std::string_view trimmed(std::string_view line)
{
  const size_t first = line.find_first_not_of(" \t\r");
  if (first == std::string_view::npos)
    return {};
  const size_t last = line.find_last_not_of(" \t\r");
  return line.substr(first, last - first + 1);
}

std::string clipped(std::string_view line, size_t maxChars)
{
  if (line.size() <= maxChars)
    return std::string(line);
  size_t cut = line.rfind(' ', maxChars);
  if (cut == std::string_view::npos || cut == 0) {
    cut = maxChars;
    while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0U) == 0x80U)
      --cut;
  }
  std::string out(trimmed(line.substr(0, cut)));
  while (!out.empty() && (out.back() == ',' || out.back() == ';' || out.back() == ':'))
    out.pop_back();
  out += kEllipsis;
  return out;
}

int64_t startOfLocalDay(int64_t now)
{
  const auto moment = static_cast<std::time_t>(now);
  std::tm local{};
  localtime_r(&moment, &local);
  local.tm_hour = 0;
  local.tm_min = 0;
  local.tm_sec = 0;
  local.tm_isdst = -1;
  return static_cast<int64_t>(std::mktime(&local));
}

struct ArgumentsInput
{
  const argus::mcp::ToolSpec& spec;
  const ContextFetchLimits& limits;
  int64_t now{0};
};

Json::Value argumentsFor(const ArgumentsInput& input)
{
  Json::Value arguments(Json::objectValue);
  const Json::Value& properties = input.spec.inputSchema["properties"];
  const auto declares = [&properties](const char* name) { return properties.isObject() && properties.isMember(name); };
  if (declares("from") && declares("to")) {
    const int64_t from = startOfLocalDay(input.now);
    arguments["from"] = iso_time::format(from);
    arguments["to"] = iso_time::format(from + (1 + std::max(0, input.limits.agendaDays)) * kDaySeconds);
  }
  if (declares("limit"))
    arguments["limit"] = kListLimit;
  return arguments;
}

std::optional<tools::ToolHandle> offeredTool(const std::vector<tools::ToolHandle>& offered, std::string_view name)
{
  const auto found = std::ranges::find_if(offered, [name](const tools::ToolHandle& tool) {
    return tool && tool->spec.name == name;
  });
  if (found == offered.end())
    return std::nullopt;
  return *found;
}

bool readable(const ToolAudience& audience, const argus::mcp::ToolSpec& spec)
{
  return isReadOnlyTool(spec.name) && tool_access::runnable(audience, spec) && tool_access::surfaceAllows(audience, spec);
}

}

std::optional<std::string_view> fetchToolFor(ContextFacet facet)
{
  const auto found = std::ranges::find(kFacetTools, facet, &FacetTool::facet);
  if (found == kFacetTools.end())
    return std::nullopt;
  return found->tool;
}

std::vector<std::string> factLines(const FactLinesInput& input)
{
  std::vector<std::string> out;
  if (!input.result.ok || input.maxLines <= 0 || input.maxChars <= 0)
    return out;
  const auto limit = static_cast<size_t>(input.maxLines);
  const auto width = static_cast<size_t>(input.maxChars);
  const auto take = [&out, limit, width](std::string_view line) {
    const std::string_view body = trimmed(line);
    if (!body.empty() && out.size() < limit)
      out.push_back(clipped(body, width));
  };
  std::string_view text = input.result.output;
  while (!text.empty() && out.size() < limit) {
    const size_t end = text.find('\n');
    take(text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
  }
  const Json::Value& facts = input.result.data["facts"];
  if (out.empty() && facts.isArray())
    for (const Json::Value& fact : facts)
      if (fact.isString())
        take(fact.asString());
  return out;
}

ContextFetch toolContextFetch(const ToolContextFetchInput& input)
{
  const ToolExecutor* executor = &input.executor;
  const ContextFetchLimits limits = input.limits;
  return [executor, limits](const ContextFetchInput& args) {
    std::vector<ContextFact> facts;
    const auto started = std::chrono::steady_clock::now();
    const int64_t now = args.context.now.value_or(static_cast<int64_t>(std::time(nullptr)));
    for (const ContextFacet facet : args.facets) {
      const auto name = fetchToolFor(facet);
      if (!name || std::ranges::find(args.ran, *name) != args.ran.end())
        continue;
      const auto tool = offeredTool(args.offered, *name);
      if (!tool || !readable(args.audience, (*tool)->spec))
        continue;
      const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
      if (spent.count() >= limits.budgetMs) {
        LOG_INFO << "LfmAdapter: the context reads ran out of their " << limits.budgetMs << " ms; "
                 << std::string(*name) << " was not read";
        break;
      }
      tools::ToolCall call{.name = (*tool)->spec.name,
                           .arguments = argumentsFor({.spec = (*tool)->spec, .limits = limits, .now = now}),
                           .context = args.context};
      call.context.channel = std::string(kContextFetchChannel);
      call.context.decided = false;
      call.context.emitAction = {};
      call.context.now = now;
      try {
        const tools::ToolResult result = executor->execute(call, args.audience);
        for (std::string& line :
             factLines({.result = result, .maxLines = limits.maxLines, .maxChars = limits.maxChars}))
          facts.push_back({.facet = std::string(contextFacetToString(facet)), .text = std::move(line)});
      }
      catch (const std::exception& error) {
        LOG_WARN << "LfmAdapter: the context read " << call.name << " failed: " << error.what();
      }
    }
    return facts;
  };
}

}
