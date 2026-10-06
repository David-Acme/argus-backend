#pragma once

#include <http/listener-config.hxx>

#include <array>
#include <chrono>
#include <string>
#include <string_view>
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

class SettingsConfig
{
public:
  [[nodiscard]] static ListenerConfig resolveListener();

  [[nodiscard]] static std::vector<SettingsOwnerConfig> resolveOwners();

  [[nodiscard]] static std::vector<std::string> unconfiguredOwners(const std::vector<SettingsOwnerConfig>& owners);

  [[nodiscard]] static FirstRunConfig resolveFirstRun();

  [[nodiscard]] static SettingsTimeouts resolveTimeouts();

  [[nodiscard]] static std::string resolveProfilesPath();
};
