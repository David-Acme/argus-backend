#include "settings-registry.hxx"

#include "config-service.hxx"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace
{
constexpr std::size_t kMaxTextLength = 512;
constexpr const char* kMarkerId = "settings_profile.id";
constexpr const char* kMarkerOrigin = "settings_profile.origin";
constexpr const char* kMarkerAppliedAt = "settings_profile.applied_at";
constexpr const char* kMarkerKeys = "settings_profile.keys";

std::vector<std::string> splitKeys(const std::string& text)
{
  std::vector<std::string> keys;
  for (const auto part : std::views::split(text, ',')) {
    std::string key(part.begin(), part.end());
    if (!key.empty())
      keys.push_back(std::move(key));
  }
  return keys;
}

std::string joinKeys(const std::vector<std::string>& keys)
{
  std::string text;
  for (const auto& key : keys) {
    if (!text.empty())
      text += ',';
    text += key;
  }
  return text;
}

constexpr std::size_t kMaxMarkerKeysLength = 4096;

bool validMarkerText(const std::string& text, std::size_t limit)
{
  return text.size() <= limit && text.find_first_of(std::string_view("\n\r\0", 3)) == std::string::npos;
}

std::string decimalText(double value)
{
  std::array<char, 32> buffer{};
  const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (error != std::errc{})
    return "0";
  return {buffer.data(), end};
}

std::optional<double> parseDecimal(const std::string& text)
{
  double value = 0;
  const auto* last = text.data() + text.size();
  const auto [end, error] = std::from_chars(text.data(), last, value);
  if (text.empty() || error != std::errc{} || end != last || !std::isfinite(value))
    return std::nullopt;
  return value;
}

std::optional<long long> parseInteger(const std::string& text)
{
  long long value = 0;
  const auto* last = text.data() + text.size();
  const auto [end, error] = std::from_chars(text.data(), last, value);
  if (text.empty() || error != std::errc{} || end != last)
    return std::nullopt;
  return value;
}

bool withinRange(const SettingRange& range, double value)
{
  if (range.min == 0 && range.max == 0)
    return true;
  return value >= range.min && value <= range.max;
}

struct Validation
{
  std::string canonical;
  std::optional<SettingRejectionReason> rejection;
};

Validation validate(const SettingSpec& spec, const std::string& value)
{
  switch (spec.type) {
  case SettingType::Toggle:
    if (value == "true" || value == "false")
      return {.canonical = value, .rejection = std::nullopt};
    return {.canonical = {}, .rejection = SettingRejectionReason::Invalid};
  case SettingType::Integer: {
    const auto parsed = parseInteger(value);
    if (!parsed || *parsed < std::numeric_limits<int>::min() ||
        *parsed > std::numeric_limits<int>::max())
      return {.canonical = {}, .rejection = SettingRejectionReason::Invalid};
    if (!withinRange(spec.range, static_cast<double>(*parsed)))
      return {.canonical = {}, .rejection = SettingRejectionReason::OutOfRange};
    return {.canonical = std::to_string(*parsed), .rejection = std::nullopt};
  }
  case SettingType::Decimal: {
    const auto parsed = parseDecimal(value);
    if (!parsed)
      return {.canonical = {}, .rejection = SettingRejectionReason::Invalid};
    if (!withinRange(spec.range, *parsed))
      return {.canonical = {}, .rejection = SettingRejectionReason::OutOfRange};
    return {.canonical = decimalText(*parsed), .rejection = std::nullopt};
  }
  case SettingType::Choice:
    if (std::ranges::find(spec.choices, value) == spec.choices.end())
      return {.canonical = {}, .rejection = SettingRejectionReason::NotAChoice};
    return {.canonical = value, .rejection = std::nullopt};
  case SettingType::Text:
    if (value.size() > kMaxTextLength ||
        value.find_first_of(std::string_view("\n\r\0", 3)) != std::string::npos)
      return {.canonical = {}, .rejection = SettingRejectionReason::Invalid};
    return {.canonical = value, .rejection = std::nullopt};
  }
  return {.canonical = {}, .rejection = SettingRejectionReason::Invalid};
}

std::string currentValue(const SettingSpec& spec)
{
  if (!ConfigService::hasKey(spec.key))
    return spec.fallback;
  switch (spec.type) {
  case SettingType::Toggle:
    return ConfigService::getBool(spec.key) ? "true" : "false";
  case SettingType::Integer:
    return std::to_string(ConfigService::getInt(spec.key));
  case SettingType::Decimal:
    return decimalText(ConfigService::getDouble(spec.key));
  case SettingType::Choice:
  case SettingType::Text:
    return ConfigService::getString(spec.key);
  }
  return spec.fallback;
}

bool hostOnly(const std::vector<ChoiceState>& states, const std::string& choice)
{
  const auto state = std::ranges::find(states, choice, &ChoiceState::choice);
  return state != states.end() && state->availability == ChoiceAvailability::HostOnly;
}

bool persist(const SettingSpec& spec, const std::string& canonical)
{
  switch (spec.type) {
  case SettingType::Toggle:
    return ConfigService::setBool(spec.key, canonical == "true");
  case SettingType::Integer:
    return ConfigService::setInt(spec.key, std::stoi(canonical));
  case SettingType::Decimal:
    return ConfigService::setDouble(spec.key, parseDecimal(canonical).value_or(0.0));
  case SettingType::Choice:
  case SettingType::Text:
    return ConfigService::setString(spec.key, canonical);
  }
  return false;
}
}

std::string_view profileOriginToString(ProfileOrigin origin)
{
  switch (origin) {
  case ProfileOrigin::None: return "";
  case ProfileOrigin::Recommended: return "recommended";
  case ProfileOrigin::Owner: return "owner";
  case ProfileOrigin::Reverted: return "reverted";
  }
  return "";
}

ProfileOrigin profileOriginFromString(std::string_view text)
{
  if (text == "recommended")
    return ProfileOrigin::Recommended;
  if (text == "owner")
    return ProfileOrigin::Owner;
  if (text == "reverted")
    return ProfileOrigin::Reverted;
  return ProfileOrigin::None;
}

SettingsRegistry::SettingsRegistry(std::vector<SettingSpec> specs) : specs_(std::move(specs))
{
  std::unordered_set<std::string> keys;
  bootValues_.reserve(specs_.size());
  for (const auto& spec : specs_) {
    if (spec.key.empty() || spec.key.find('.') == std::string::npos || !keys.insert(spec.key).second)
      throw std::invalid_argument("Invalid setting key: " + spec.key);
    if (spec.type == SettingType::Choice && spec.choices.empty())
      throw std::invalid_argument("A choice setting needs choices: " + spec.key);
    if (validate(spec, spec.fallback).rejection)
      throw std::invalid_argument("Invalid setting fallback: " + spec.key);
    bootValues_.push_back(currentValue(spec));
  }
}

std::vector<SettingEntry> SettingsRegistry::list() const
{
  std::vector<SettingEntry> entries;
  entries.reserve(specs_.size());
  for (const auto index : std::views::iota(std::size_t{0}, specs_.size())) {
    const auto& spec = specs_[index];
    auto value = currentValue(spec);
    const bool pending = pendingRestart(index, value);
    entries.push_back(
        {.spec = spec, .value = std::move(value), .choiceStates = choiceStatesOf(spec), .pendingRestart = pending});
  }
  return entries;
}

bool SettingsRegistry::pendingRestart(std::size_t index, const std::string& value) const
{
  return specs_[index].apply == SettingApply::Restart && value != bootValues_[index];
}

ProfileMarker SettingsRegistry::profileMarker() const
{
  std::scoped_lock lock(mutex_);
  return {.id = ConfigService::getString(kMarkerId),
          .origin = profileOriginFromString(ConfigService::getString(kMarkerOrigin)),
          .appliedAt = ConfigService::getInt(kMarkerAppliedAt),
          .keys = splitKeys(ConfigService::getString(kMarkerKeys))};
}

bool SettingsRegistry::recordProfile(const ProfileMarker& marker)
{
  const auto keys = joinKeys(marker.keys);
  if (!validMarkerText(marker.id, kMaxTextLength) || !validMarkerText(keys, kMaxMarkerKeysLength) || marker.origin == ProfileOrigin::None ||
      marker.appliedAt < 0 || marker.appliedAt > std::numeric_limits<int>::max())
    return false;
  std::scoped_lock lock(mutex_);
  return ConfigService::setString(kMarkerId, marker.id) &&
         ConfigService::setString(kMarkerOrigin, std::string(profileOriginToString(marker.origin))) &&
         ConfigService::setInt(kMarkerAppliedAt, static_cast<int>(marker.appliedAt)) &&
         ConfigService::setString(kMarkerKeys, keys);
}

void SettingsRegistry::declareCapability(std::string capability)
{
  std::scoped_lock lock(mutex_);
  if (std::ranges::find(capabilities_, capability) == capabilities_.end())
    capabilities_.push_back(std::move(capability));
}

std::vector<std::string> SettingsRegistry::capabilities() const
{
  std::scoped_lock lock(mutex_);
  return capabilities_;
}

std::vector<ChoiceState> SettingsRegistry::choiceStatesOf(const SettingSpec& spec) const
{
  if (spec.type != SettingType::Choice)
    return {};
  ChoiceStates describe;
  {
    std::scoped_lock lock(mutex_);
    describe = describe_;
  }
  if (!describe)
    return {};
  auto states = describe(spec);
  std::erase_if(states, [&spec](const ChoiceState& state) {
    return std::ranges::find(spec.choices, state.choice) == spec.choices.end();
  });
  return states;
}

void SettingsRegistry::describeChoices(ChoiceStates describe)
{
  std::scoped_lock lock(mutex_);
  describe_ = std::move(describe);
}

SettingsUpdateResult SettingsRegistry::update(const std::vector<SettingChange>& changes)
{
  SettingsUpdateResult result;
  std::vector<std::pair<const SettingSpec*, std::string>> accepted;
  accepted.reserve(changes.size());
  for (const auto& change : changes) {
    const auto* spec = find(change.key);
    if (spec == nullptr) {
      result.rejected.push_back({.key = change.key, .reason = SettingRejectionReason::Unknown});
      continue;
    }
    auto validation = validate(*spec, change.value);
    if (validation.rejection) {
      result.rejected.push_back({.key = change.key, .reason = *validation.rejection});
      continue;
    }
    if (hostOnly(choiceStatesOf(*spec), validation.canonical) && validation.canonical != currentValue(*spec)) {
      result.rejected.push_back({.key = change.key, .reason = SettingRejectionReason::NotInstalled});
      continue;
    }
    accepted.emplace_back(spec, std::move(validation.canonical));
  }
  if (!result.rejected.empty())
    return result;

  std::vector<Listener> listeners;
  {
    std::scoped_lock lock(mutex_);
    for (const auto& [spec, canonical] : accepted) {
      if (persist(*spec, canonical))
        result.applied.push_back(spec->key);
      else
        result.rejected.push_back({.key = spec->key, .reason = SettingRejectionReason::WriteFailed});
    }
    listeners = listeners_;
  }
  if (!result.applied.empty())
    for (const auto& listener : listeners)
      listener(result.applied);
  return result;
}

void SettingsRegistry::onChange(Listener listener)
{
  std::scoped_lock lock(mutex_);
  listeners_.push_back(std::move(listener));
}

const SettingSpec* SettingsRegistry::find(const std::string& key) const
{
  const auto match = std::ranges::find(specs_, key, &SettingSpec::key);
  return match == specs_.end() ? nullptr : &*match;
}
