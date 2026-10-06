#pragma once

#include <http/listener-config.hxx>

#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

inline constexpr std::array<std::string_view, 8> kSettingsOwnerOrder{
    "llm", "voice", "tts", "stt", "vlm", "guard", "camera", "notification"};

struct SettingsOwnerConfig
{
  std::string name;
  std::string target;
  std::string credential;
  std::string configFile{};
};

struct FirstRunConfig
{
  bool enabled{true};
  std::chrono::seconds interval{30};
};

struct SettingsTimeouts
{
  std::chrono::milliseconds list{1500};
  std::chrono::milliseconds update{5000};
};

struct ModulesConfig
{
  std::string catalogPath{"modules.json"};
  std::string dbPath{"settings.db"};
  std::string schemaPath{"database/schema.sql"};
  std::string modelsDir{"../../models"};
  std::chrono::milliseconds pollInterval{1000};
  std::chrono::seconds idleRefresh{30};
  std::chrono::seconds healthTimeout{180};
  std::chrono::seconds seedWait{300};
};

struct ModulesRpcConfig
{
  std::string address;
  std::vector<std::pair<std::string, std::string>> callers;
};

inline constexpr std::array<std::string_view, 9> kModuleStateCallers{
    "auth", "camera", "guard", "identity", "notification", "productivity", "sync", "voice", "llm"};

class SettingsConfig
{
public:
  [[nodiscard]] static ListenerConfig resolveListener();

  [[nodiscard]] static std::vector<SettingsOwnerConfig> resolveOwners();

  [[nodiscard]] static std::vector<std::string> unconfiguredOwners(const std::vector<SettingsOwnerConfig>& owners);

  [[nodiscard]] static FirstRunConfig resolveFirstRun();

  [[nodiscard]] static SettingsTimeouts resolveTimeouts();

  [[nodiscard]] static std::string resolveProfilesPath();

  [[nodiscard]] static ModulesConfig resolveModules();

  [[nodiscard]] static ModulesRpcConfig resolveModulesRpc();
};
