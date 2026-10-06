#pragma once

#include <config/settings-config.hxx>
#include <settings/component-vocabulary.hxx>
#include <settings/owner-pin-host.hxx>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class SettingsClient;

enum class OwnerReach : std::uint8_t
{
  Answered,
  Unsupported,
  Unreachable
};

struct OwnerStates
{
  OwnerReach reach{OwnerReach::Unreachable};
  std::vector<ComponentStatus> states;
};

template <typename T>
struct OwnerReply
{
  OwnerReach reach{OwnerReach::Unreachable};
  std::optional<T> value;
};

class ComponentOwners
{
public:
  ComponentOwners() = default;
  virtual ~ComponentOwners() = default;
  ComponentOwners(const ComponentOwners&) = delete;
  ComponentOwners& operator=(const ComponentOwners&) = delete;
  ComponentOwners(ComponentOwners&&) = delete;
  ComponentOwners& operator=(ComponentOwners&&) = delete;

  [[nodiscard]] virtual OwnerStates states(const std::string& owner, const std::vector<ComponentSpec>& specs) const = 0;
  [[nodiscard]] virtual OwnerReply<ComponentStatus> install(const std::string& owner, const ComponentSpec& spec) const = 0;
  [[nodiscard]] virtual OwnerReply<ComponentStatus> cancel(const std::string& owner, const ComponentSpec& spec) const = 0;
  [[nodiscard]] virtual OwnerReply<ComponentStatus> remove(const std::string& owner, const ComponentSpec& spec) const = 0;
  [[nodiscard]] virtual OwnerReply<ModuleDataSummary> dataSummary(const std::string& owner,
                                                                  const std::string& moduleId) const = 0;
  [[nodiscard]] virtual OwnerReply<ModuleDataPurge> purgeData(const std::string& owner, const std::string& moduleId) const = 0;
  [[nodiscard]] virtual OwnerReply<PinVerdict> verifyPin(const std::string& owner, const OwnerPinCheck& check) const = 0;
  [[nodiscard]] virtual OwnerReply<ModuleImpactReport> impact(const std::string&, const std::string&) const
  {
    return {.reach = OwnerReach::Unsupported, .value = std::nullopt};
  }
  [[nodiscard]] virtual OwnerReply<RoleReassignmentOutcome> reassignRoles(const std::string&,
                                                                          const RoleReassignmentBatch&) const
  {
    return {.reach = OwnerReach::Unsupported, .value = std::nullopt};
  }
  [[nodiscard]] virtual OwnerReply<ModuleRequestOutcome> requestModule(const std::string&,
                                                                       const ModuleRequestInput&) const
  {
    return {.reach = OwnerReach::Unsupported, .value = std::nullopt};
  }
};

struct SettingsComponentOwnersInput
{
  std::vector<SettingsOwnerConfig> owners;
  std::chrono::milliseconds timeout{5000};
};

class SettingsComponentOwners final : public ComponentOwners
{
public:
  explicit SettingsComponentOwners(const SettingsComponentOwnersInput& input);
  ~SettingsComponentOwners() override;
  SettingsComponentOwners(const SettingsComponentOwners&) = delete;
  SettingsComponentOwners& operator=(const SettingsComponentOwners&) = delete;
  SettingsComponentOwners(SettingsComponentOwners&&) = delete;
  SettingsComponentOwners& operator=(SettingsComponentOwners&&) = delete;

  [[nodiscard]] OwnerStates states(const std::string& owner, const std::vector<ComponentSpec>& specs) const override;
  [[nodiscard]] OwnerReply<ComponentStatus> install(const std::string& owner, const ComponentSpec& spec) const override;
  [[nodiscard]] OwnerReply<ComponentStatus> cancel(const std::string& owner, const ComponentSpec& spec) const override;
  [[nodiscard]] OwnerReply<ComponentStatus> remove(const std::string& owner, const ComponentSpec& spec) const override;
  [[nodiscard]] OwnerReply<ModuleDataSummary> dataSummary(const std::string& owner,
                                                          const std::string& moduleId) const override;
  [[nodiscard]] OwnerReply<ModuleDataPurge> purgeData(const std::string& owner, const std::string& moduleId) const override;
  [[nodiscard]] OwnerReply<PinVerdict> verifyPin(const std::string& owner, const OwnerPinCheck& check) const override;
  [[nodiscard]] OwnerReply<ModuleImpactReport> impact(const std::string& owner,
                                                      const std::string& moduleId) const override;
  [[nodiscard]] OwnerReply<RoleReassignmentOutcome> reassignRoles(const std::string& owner,
                                                                  const RoleReassignmentBatch& batch) const override;
  [[nodiscard]] OwnerReply<ModuleRequestOutcome> requestModule(const std::string& owner,
                                                               const ModuleRequestInput& input) const override;

private:
  [[nodiscard]] const SettingsClient* client(const std::string& owner) const;

  std::map<std::string, std::unique_ptr<SettingsClient>> clients_;
};
