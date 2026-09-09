#include "tool-registry.hxx"

#include <algorithm>
#include <cctype>

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
  if (it != tools_.end())
    return &it->second;
  // A case mismatch must not silently answer prose over a save (bench f8-b4).
  const std::string lower = [name] {
    std::string out;
    out.reserve(name.size());
    for (char c : name)
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
  }();
  for (const auto& [key, descriptor] : tools_) {
    if (key.size() == lower.size() &&
        std::equal(key.begin(), key.end(), lower.begin(),
                   [](char a, char b) {
                     return std::tolower(static_cast<unsigned char>(a)) ==
                            static_cast<unsigned char>(b);
                   }))
      return &descriptor;
  }
  return nullptr;
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
