#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct ModuleFlag
{
  std::string id;
  bool enabled{true};
};

using ModuleFlags = std::vector<ModuleFlag>;

struct ModuleChange
{
  std::string id;
  bool enabled{true};
};

class ModuleGate
{
public:
  using Listener = std::function<void(const ModuleChange&)>;

  [[nodiscard]] bool enabled(std::string_view module) const;
  [[nodiscard]] std::optional<std::string> disabledModuleOf(std::string_view path) const;
  [[nodiscard]] ModuleFlags known() const;

  std::vector<ModuleChange> apply(const ModuleFlags& flags);
  void onChange(Listener listener);
  void reset();

private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, bool> states_;
  std::vector<Listener> listeners_;
};

ModuleGate& moduleGate();

namespace module_gate
{
inline constexpr std::string_view kModulesField = "modules";
inline constexpr std::string_view kIdField = "id";
inline constexpr std::string_view kEnabledField = "enabled";
inline constexpr std::string_view kLifecycleField = "lifecycle";
inline constexpr std::string_view kActiveLifecycle = "active";

[[nodiscard]] std::optional<ModuleFlags> parseEnabledSet(std::string_view body);
[[nodiscard]] std::string serializeEnabledSet(const ModuleFlags& flags);

[[nodiscard]] std::optional<ModuleFlags> loadStateFile(const std::string& path);
bool saveStateFile(const std::string& path, const ModuleFlags& flags);
}
