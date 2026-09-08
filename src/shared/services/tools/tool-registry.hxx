#pragma once

#include <memory>
#include <mutex>
#include <shared/contracts/tool-contracts.hxx>
#include <string>
#include <unordered_map>
#include <vector>

// Boot-time catalog of tool descriptors (COGNITIVE_MEMORY_PLAN.md §6); services self-register at init.
class ToolRegistry
{
public:
  static ToolRegistry& instance();

  // Public so isolated registries can exist alongside the process-wide singleton.
  ToolRegistry() = default;

  void registerTool(tools::ToolDescriptor descriptor);
  const tools::ToolDescriptor* find(const std::string& name) const;
  std::vector<std::string> names() const;

private:
  std::unordered_map<std::string, tools::ToolDescriptor> tools_;
  mutable std::mutex mutex_;
};
