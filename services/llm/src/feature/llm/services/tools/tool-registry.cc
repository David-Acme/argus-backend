#include "tool-registry.hxx"

#include <feature/llm/services/tools/remote-tool.hxx>

#include <trantor/utils/Logger.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <set>
#include <utility>

namespace
{
bool sameName(std::string_view left, std::string_view right)
{
  return std::ranges::equal(left, right, [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
  });
}
}

ToolRegistry& ToolRegistry::instance()
{
  static ToolRegistry registry;
  return registry;
}

void ToolRegistry::registerTool(tools::ToolDescriptor descriptor)
{
  const std::unique_lock lock(mutex_);
  const std::string name = descriptor.spec.name;
  std::erase_if(local_, [&name](const tools::ToolHandle& tool) { return tool->spec.name == name; });
  local_.push_back(std::make_shared<const tools::ToolDescriptor>(std::move(descriptor)));
  rebuild();
}

void ToolRegistry::addProvider(ToolProvider provider)
{
  const std::unique_lock lock(mutex_);
  providers_.push_back({.provider = std::move(provider), .tools = {}, .listed = false});
}

RefreshOutcome ToolRegistry::refresh(std::string_view only)
{
  std::vector<std::pair<std::string, std::shared_ptr<argus::mcp::McpClient>>> targets;
  {
    const std::shared_lock lock(mutex_);
    for (const auto& state : providers_)
      if (only.empty() || state.provider.id == only)
        targets.emplace_back(state.provider.id, state.provider.client);
  }
  RefreshOutcome outcome;
  for (const auto& [id, client] : targets) {
    const auto listed = client->listTools();
    if (!listed.value) {
      LOG_WARN << "tools: provider " << id << " did not list its tools: " << listed.failure.error.message;
      outcome.failed.push_back(id);
      continue;
    }
    std::vector<tools::ToolHandle> fresh;
    for (const auto& spec : listed.value->tools)
      fresh.push_back(remoteTool({.provider = id, .client = client, .spec = spec}));
    {
      const std::unique_lock lock(mutex_);
      for (auto& state : providers_) {
        if (state.provider.id != id)
          continue;
        if (!state.listed || state.tools.size() != fresh.size())
          LOG_INFO << "tools: provider " << id << " serves " << fresh.size() << " tools";
        state.tools = std::move(fresh);
        state.listed = true;
        break;
      }
      rebuild();
    }
    outcome.refreshed.push_back(id);
  }
  return outcome;
}

void ToolRegistry::rebuild()
{
  std::vector<tools::ToolHandle> merged;
  std::set<std::string, std::less<>> taken;
  const auto take = [&merged, &taken](const tools::ToolHandle& tool) {
    if (!taken.insert(tool->spec.name).second) {
      LOG_WARN << "tools: " << tool->spec.name << " is declared twice; the first declaration stays";
      return;
    }
    merged.push_back(tool);
  };
  for (const auto& tool : local_)
    take(tool);
  for (const auto& state : providers_)
    for (const auto& tool : state.tools)
      take(tool);
  std::ranges::sort(merged, {}, [](const tools::ToolHandle& tool) -> const std::string& { return tool->spec.name; });
  merged_ = std::move(merged);
}

tools::ToolHandle ToolRegistry::find(const std::string& name) const
{
  const std::shared_lock lock(mutex_);
  const auto exact = std::ranges::find_if(
      merged_, [&name](const tools::ToolHandle& tool) { return tool->spec.name == name; });
  if (exact != merged_.end())
    return *exact;
  const auto loose = std::ranges::find_if(
      merged_, [&name](const tools::ToolHandle& tool) { return sameName(tool->spec.name, name); });
  return loose == merged_.end() ? nullptr : *loose;
}

std::vector<tools::ToolHandle> ToolRegistry::all() const
{
  const std::shared_lock lock(mutex_);
  return merged_;
}

std::vector<std::string> ToolRegistry::names() const
{
  const std::shared_lock lock(mutex_);
  std::vector<std::string> out;
  out.reserve(merged_.size());
  for (const auto& tool : merged_)
    out.push_back(tool->spec.name);
  return out;
}

std::vector<std::string> ToolRegistry::unlisted() const
{
  const std::shared_lock lock(mutex_);
  std::vector<std::string> out;
  for (const auto& state : providers_)
    if (!state.listed)
      out.push_back(state.provider.id);
  return out;
}
