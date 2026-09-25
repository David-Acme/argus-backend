#pragma once

#include <memory>
#include <mutex>
#include <shared/vocabulary/tool-contracts.hxx>
#include <string>
#include <unordered_map>
#include <vector>

class ToolRegistry
{
public:
  static ToolRegistry& instance();

  ToolRegistry() = default;

  void registerTool(tools::ToolDescriptor descriptor);
  const tools::ToolDescriptor* find(const std::string& name) const;
  std::vector<std::string> names() const;

private:
  std::unordered_map<std::string, tools::ToolDescriptor> tools_;
  mutable std::mutex mutex_;
};
