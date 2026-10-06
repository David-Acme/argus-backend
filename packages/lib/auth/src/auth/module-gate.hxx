#pragma once

#include <auth/module-snapshot.hxx>
#include <auth/user-role.hxx>

#include <drogon/HttpTypes.h>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct ModuleChange
{
  std::string id;
  bool enabled{true};
};

class ModuleGate
{
public:
  using Listener = std::function<void(const ModuleChange&)>;
  using StateListener = std::function<void()>;

  [[nodiscard]] bool enabled(std::string_view module) const;
  [[nodiscard]] std::optional<std::string> disabledModuleOf(std::string_view path) const;
  [[nodiscard]] std::optional<std::string> disabledModuleOf(std::string_view path,
                                                            drogon::HttpMethod method) const;
  [[nodiscard]] bool roleActive(UserRole role) const;
  [[nodiscard]] ModuleFlags known() const;
  [[nodiscard]] ModuleSnapshot snapshot() const;
  [[nodiscard]] std::shared_ptr<const ModuleSnapshot> current() const;

  std::vector<ModuleChange> apply(const ModuleFlags& flags);
  void onChange(Listener listener);
  void onStateChange(StateListener listener);
  void reset();

private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, ModuleFlag> states_;
  std::shared_ptr<const ModuleSnapshot> current_{std::make_shared<const ModuleSnapshot>()};
  std::vector<Listener> listeners_;
  std::vector<StateListener> stateListeners_;
};

ModuleGate& moduleGate();

namespace module_gate
{
inline constexpr std::string_view kModulesField = "modules";
inline constexpr std::string_view kIdField = "id";
inline constexpr std::string_view kEnabledField = "enabled";
inline constexpr std::string_view kLifecycleField = "lifecycle";
inline constexpr std::string_view kActiveLifecycle = "active";
inline constexpr std::string_view kDataPurgedAtField = "dataPurgedAt";
inline constexpr std::string_view kRolesField = "roles";
inline constexpr std::string_view kNameField = "name";
inline constexpr std::string_view kSummaryField = "summary";
inline constexpr std::string_view kIntroField = "intro";
inline constexpr std::string_view kKindField = "kind";
inline constexpr std::string_view kWhatField = "what";
inline constexpr std::string_view kExamplesField = "examples";
inline constexpr std::string_view kEsField = "es";
inline constexpr std::string_view kEnField = "en";

[[nodiscard]] std::optional<ModuleFlags> parseEnabledSet(std::string_view body);
[[nodiscard]] std::string serializeEnabledSet(const ModuleFlags& flags);

[[nodiscard]] std::optional<ModuleFlags> loadStateFile(const std::string& path);
bool saveStateFile(const std::string& path, const ModuleFlags& flags);
}
