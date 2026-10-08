#pragma once

#include <config/config-service.hxx>
#include <config/voice-config.hxx>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace voice_test_config
{

inline constexpr const char* kPath = "voice-test-config.toml";

inline void writeOpening(std::string_view opening)
{
  {
    std::ofstream file(kPath);
    file << "[voice]\n"
         << "opening = \"" << opening << "\"\n";
    if (!file)
      throw std::runtime_error(std::string("voice-test-config: cannot write ") + kPath);
  }
  ConfigService::load(kPath);
  if (ConfigService::getString("voice.opening") != opening)
    throw std::runtime_error("voice-test-config: opening '" + std::string(opening) +
                             "' did not take");
}

inline void clearConfig()
{
  {
    std::ofstream file(kPath);
    if (!file)
      throw std::runtime_error(std::string("voice-test-config: cannot write ") + kPath);
  }
  ConfigService::load(kPath);
  if (!ConfigService::getString("voice.opening").empty())
    throw std::runtime_error("voice-test-config: the config did not clear");
}

[[nodiscard]] inline std::string currentOpening()
{
  return ConfigService::getString("voice.opening");
}

inline void restoreOpening(std::string_view opening)
{
  if (opening.empty())
    clearConfig();
  else
    writeOpening(opening);
}

inline void installSpokenOpening()
{
  writeOpening(voiceOpeningToString(VoiceOpening::Spoken));
}

inline void removeConfig()
{
  std::remove(kPath);
}

}

class SuiteOpeningConfig
{
public:
  SuiteOpeningConfig() { voice_test_config::installSpokenOpening(); }

  ~SuiteOpeningConfig() { voice_test_config::removeConfig(); }

  SuiteOpeningConfig(const SuiteOpeningConfig&) = delete;
  SuiteOpeningConfig& operator=(const SuiteOpeningConfig&) = delete;
  SuiteOpeningConfig(SuiteOpeningConfig&&) = delete;
  SuiteOpeningConfig& operator=(SuiteOpeningConfig&&) = delete;
};

class OpeningConfig
{
public:
  explicit OpeningConfig(std::string_view opening)
      : previous_(voice_test_config::currentOpening())
  {
    voice_test_config::writeOpening(opening);
  }

  ~OpeningConfig() noexcept(false) { voice_test_config::restoreOpening(previous_); }

  OpeningConfig(const OpeningConfig&) = delete;
  OpeningConfig& operator=(const OpeningConfig&) = delete;
  OpeningConfig(OpeningConfig&&) = delete;
  OpeningConfig& operator=(OpeningConfig&&) = delete;

private:
  std::string previous_;
};

class ClearedVoiceConfig
{
public:
  ClearedVoiceConfig() : previous_(voice_test_config::currentOpening())
  {
    voice_test_config::clearConfig();
  }

  ~ClearedVoiceConfig() noexcept(false) { voice_test_config::restoreOpening(previous_); }

  ClearedVoiceConfig(const ClearedVoiceConfig&) = delete;
  ClearedVoiceConfig& operator=(const ClearedVoiceConfig&) = delete;
  ClearedVoiceConfig(ClearedVoiceConfig&&) = delete;
  ClearedVoiceConfig& operator=(ClearedVoiceConfig&&) = delete;

private:
  std::string previous_;
};
