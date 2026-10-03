#pragma once

#include <config/settings-config.hxx>
#include <config/settings-registry.hxx>
#include <drogon/utils/coroutine.h>
#include <settings/settings-client.hxx>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SettingsGatewayInput
{
  std::vector<SettingsOwnerConfig> owners;
  SettingsTimeouts timeouts;
};

struct OwnerCatalog
{
  std::string service;
  bool reachable{false};
  std::vector<SettingEntry> settings;
};

struct SettingsUpdateInput
{
  std::string owner;
  std::vector<SettingChange> changes;
  int64_t userId{0};
};

struct SettingsUpdateOutcome
{
  std::vector<std::string> applied;
  OwnerCatalog catalog;
};

class SettingsGatewayService
{
public:
  explicit SettingsGatewayService(const SettingsGatewayInput& input);

  [[nodiscard]] std::vector<OwnerCatalog> catalogs() const;
  [[nodiscard]] drogon::Task<std::vector<OwnerCatalog>> catalogsAsync() const;

  [[nodiscard]] SettingsUpdateOutcome update(const SettingsUpdateInput& input) const;
  [[nodiscard]] drogon::Task<SettingsUpdateOutcome> updateAsync(SettingsUpdateInput input) const;

private:
  struct OwnerLink
  {
    std::string name;
    std::unique_ptr<SettingsClient> reader;
    std::unique_ptr<SettingsClient> writer;
  };

  [[nodiscard]] static OwnerCatalog fetch(const OwnerLink& owner);
  [[nodiscard]] const OwnerLink* find(const std::string& name) const;

  std::vector<OwnerLink> owners_;
};
