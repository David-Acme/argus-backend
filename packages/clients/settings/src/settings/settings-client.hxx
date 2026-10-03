#pragma once

#include <config/settings-registry.hxx>

#include <chrono>
#include <memory>
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
};

struct SettingsUpdateReply
{
  std::vector<std::string> applied;
  std::vector<SettingRejection> rejected;
  SettingsCatalog catalog;
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

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
