#pragma once

#include <auth/user-role.hxx>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

inline constexpr std::string_view kCoreModule = "core";

struct ModuleText
{
  std::string es;
  std::string en;

  bool operator==(const ModuleText&) const = default;
};

struct ModuleIntroText
{
  std::string what;
  std::vector<std::string> examples;

  bool operator==(const ModuleIntroText&) const = default;
};

struct ModuleIntro
{
  ModuleIntroText es;
  ModuleIntroText en;

  bool operator==(const ModuleIntro&) const = default;
};

struct ModuleFlag
{
  std::string id;
  bool enabled{true};
  std::string lifecycle{};
  std::int64_t dataPurgedAt{0};
  std::vector<std::string> roles{};
  ModuleText name{};
  ModuleText summary{};
  ModuleIntro intro{};
  std::string kind{};

  bool operator==(const ModuleFlag&) const = default;
};

using ModuleFlags = std::vector<ModuleFlag>;

class ModuleSnapshot
{
public:
  ModuleSnapshot() = default;
  explicit ModuleSnapshot(ModuleFlags modules) : modules_(std::move(modules)) {}

  [[nodiscard]] const ModuleFlags& modules() const { return modules_; }

  [[nodiscard]] bool enabled(std::string_view module) const
  {
    if (module == kCoreModule)
      return true;
    const auto found = std::ranges::find(modules_, module, &ModuleFlag::id);
    return found == modules_.end() || found->enabled;
  }

  [[nodiscard]] std::optional<std::string_view> moduleOfRole(UserRole role) const
  {
    if (!userRoleKnown(role))
      return std::nullopt;
    const std::string name = userRoleToString(role);
    for (const auto& module : modules_) {
      if (std::ranges::find(module.roles, name) != module.roles.end())
        return std::string_view(module.id);
    }
    return std::nullopt;
  }

  [[nodiscard]] bool roleActive(UserRole role) const
  {
    if (!userRoleKnown(role))
      return false;
    if (role == UserRole::Owner)
      return true;
    const auto module = moduleOfRole(role);
    return !module || enabled(*module);
  }

  [[nodiscard]] std::vector<std::string> activeModules() const
  {
    std::vector<std::string> active;
    for (const auto& module : modules_) {
      if (module.enabled)
        active.push_back(module.id);
    }
    return active;
  }

private:
  ModuleFlags modules_;
};
