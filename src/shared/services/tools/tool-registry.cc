#include "tool-registry.hxx"

ToolRegistry& ToolRegistry::instance()
{
  static ToolRegistry registry;
  return registry;
}

void ToolRegistry::registerTool(tools::ToolDescriptor descriptor)
{
  std::lock_guard lock(mutex_);
  tools_[descriptor.name] = std::move(descriptor);
}

const tools::ToolDescriptor* ToolRegistry::find(const std::string& name) const
{
  std::lock_guard lock(mutex_);
  const auto it = tools_.find(name);
  return it == tools_.end() ? nullptr : &it->second;
}

std::vector<std::string> ToolRegistry::names() const
{
  std::lock_guard lock(mutex_);
  std::vector<std::string> out;
  out.reserve(tools_.size());
  for (const auto& [name, descriptor] : tools_)
    out.push_back(name);
  return out;
}
