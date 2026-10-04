#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
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
  WriteFailed,
  NotInstalled
};

enum class ChoiceAvailability : std::uint8_t
{
  Installed,
  Installable,
  Installing,
  HostOnly,
  Failed
};

enum class ProfileOrigin : std::uint8_t
{
  None,
  Recommended,
  Owner,
  Reverted
};

[[nodiscard]] std::string_view profileOriginToString(ProfileOrigin origin);
[[nodiscard]] ProfileOrigin profileOriginFromString(std::string_view text);

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
  std::string unit{};
};

struct ChoiceState
{
  std::string choice;
  ChoiceAvailability availability{ChoiceAvailability::Installed};
  double sizeMb{0};
  std::string hostCommand{};
};

struct SettingEntry
{
  SettingSpec spec;
  std::string value;
  std::vector<ChoiceState> choiceStates{};
  bool pendingRestart{false};
};

struct ProfileMarker
{
  std::string id;
  ProfileOrigin origin{ProfileOrigin::None};
  std::int64_t appliedAt{0};
  std::vector<std::string> keys{};
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
  using ChoiceStates = std::function<std::vector<ChoiceState>(const SettingSpec&)>;

  explicit SettingsRegistry(std::vector<SettingSpec> specs);

  [[nodiscard]] std::vector<SettingEntry> list() const;
  SettingsUpdateResult update(const std::vector<SettingChange>& changes);
  void onChange(Listener listener);
  void describeChoices(ChoiceStates describe);

  [[nodiscard]] ProfileMarker profileMarker() const;
  bool recordProfile(const ProfileMarker& marker);

  void declareCapability(std::string capability);
  [[nodiscard]] std::vector<std::string> capabilities() const;

private:
  [[nodiscard]] const SettingSpec* find(const std::string& key) const;
  [[nodiscard]] std::vector<ChoiceState> choiceStatesOf(const SettingSpec& spec) const;
  [[nodiscard]] bool pendingRestart(std::size_t index, const std::string& value) const;

  std::vector<SettingSpec> specs_;
  std::vector<std::string> bootValues_;
  mutable std::mutex mutex_;
  std::vector<Listener> listeners_;
  ChoiceStates describe_;
  std::vector<std::string> capabilities_;
};
