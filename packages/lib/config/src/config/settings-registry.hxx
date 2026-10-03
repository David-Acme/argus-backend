#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

enum class SettingType : std::uint8_t
{
  Toggle,
  Integer,
  Decimal,
  Choice,
  Text
};

enum class SettingLevel : std::uint8_t
{
  Basic,
  Advanced
};

enum class SettingApply : std::uint8_t
{
  Live,
  NextSession,
  Restart
};

enum class SettingRejectionReason : std::uint8_t
{
  Unknown,
  Invalid,
  OutOfRange,
  NotAChoice,
  WriteFailed
};

struct SettingRange
{
  double min{0};
  double max{0};
  double step{0};
};

struct SettingSpec
{
  std::string key;
  std::string group;
  SettingType type{SettingType::Toggle};
  SettingLevel level{SettingLevel::Advanced};
  SettingApply apply{SettingApply::Restart};
  SettingRange range{};
  std::vector<std::string> choices{};
  std::string fallback{};
};

struct SettingEntry
{
  SettingSpec spec;
  std::string value;
};

struct SettingChange
{
  std::string key;
  std::string value;
};

struct SettingRejection
{
  std::string key;
  SettingRejectionReason reason{SettingRejectionReason::Invalid};
};

struct SettingsUpdateResult
{
  std::vector<std::string> applied;
  std::vector<SettingRejection> rejected;
};

class SettingsRegistry
{
public:
  using Listener = std::function<void(const std::vector<std::string>&)>;

  explicit SettingsRegistry(std::vector<SettingSpec> specs);

  [[nodiscard]] std::vector<SettingEntry> list() const;
  SettingsUpdateResult update(const std::vector<SettingChange>& changes);
  void onChange(Listener listener);

private:
  [[nodiscard]] const SettingSpec* find(const std::string& key) const;

  std::vector<SettingSpec> specs_;
  std::mutex mutex_;
  std::vector<Listener> listeners_;
};
