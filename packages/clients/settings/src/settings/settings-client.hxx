#pragma once

#include <config/settings-registry.hxx>
#include <settings/component-vocabulary.hxx>

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

  [[nodiscard]] std::optional<std::vector<ComponentStatus>> componentStates(
      const std::vector<ComponentSpec>& components) const;
  [[nodiscard]] std::optional<ComponentStatus> installComponent(const ComponentSpec& component) const;
  [[nodiscard]] std::optional<ComponentStatus> cancelComponent(const ComponentSpec& component) const;
  [[nodiscard]] std::optional<ComponentStatus> removeComponent(const ComponentSpec& component) const;
  [[nodiscard]] std::optional<ModuleDataSummary> moduleDataSummary(const std::string& moduleId) const;
  [[nodiscard]] std::optional<ModuleDataPurge> purgeModuleData(const std::string& moduleId) const;
  [[nodiscard]] std::optional<PinVerdict> verifyOwnerPin(std::int64_t userId, const std::string& pin) const;
  [[nodiscard]] std::optional<ModuleImpactReport> moduleImpact(const std::string& moduleId) const;
  [[nodiscard]] std::optional<RoleReassignmentOutcome> reassignRoles(const RoleReassignmentBatch& batch) const;
  [[nodiscard]] std::optional<ModuleRequestOutcome> requestModule(const ModuleRequestInput& input) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class ModulesClient
{
public:
  explicit ModulesClient(SettingsClientConfig config);
  ~ModulesClient();
  ModulesClient(const ModulesClient&) = delete;
  ModulesClient& operator=(const ModulesClient&) = delete;

  [[nodiscard]] ModuleStatesReply moduleStates() const;
  [[nodiscard]] OwnerCatalogReply ownerCatalog() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
