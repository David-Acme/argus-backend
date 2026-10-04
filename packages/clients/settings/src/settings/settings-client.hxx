#pragma once

#include <config/settings-registry.hxx>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct SettingsClientConfig
{
  std::string target;
  std::string credential;
  std::chrono::milliseconds timeout{5000};
};

struct SettingsCatalog
{
  std::string service;
  std::vector<SettingEntry> settings;
  std::string configPath{};
  std::optional<ProfileMarker> profile{};
  std::vector<std::string> capabilities{};
};

struct SettingsUpdateReply
{
  std::vector<std::string> applied;
  std::vector<SettingRejection> rejected;
  SettingsCatalog catalog;
  bool profileRecorded{false};
};

class SettingsClient
{
public:
  explicit SettingsClient(SettingsClientConfig config);
  ~SettingsClient();
  SettingsClient(const SettingsClient&) = delete;
  SettingsClient& operator=(const SettingsClient&) = delete;

  [[nodiscard]] SettingsCatalog list() const;
  [[nodiscard]] SettingsUpdateReply update(const std::vector<SettingChange>& changes) const;
  [[nodiscard]] SettingsUpdateReply update(const std::vector<SettingChange>& changes,
                                           const std::optional<ProfileMarker>& profile) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
