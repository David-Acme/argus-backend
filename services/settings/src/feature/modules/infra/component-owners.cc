#include "component-owners.hxx"

#include <errors/response-exception.hxx>
#include <settings/settings-client.hxx>
#include <trantor/utils/Logger.h>

#include <functional>

namespace
{
template <typename T>
OwnerReply<T> attempt(const std::string& owner, const std::function<std::optional<T>()>& call)
{
  try {
    auto value = call();
    if (!value)
      return {.reach = OwnerReach::Unsupported, .value = std::nullopt};
    return {.reach = OwnerReach::Answered, .value = std::move(value)};
  }
  catch (const std::exception& error) {
    LOG_WARN << "Modules: the call to " << owner << " failed: " << error.what();
    return {.reach = OwnerReach::Unreachable, .value = std::nullopt};
  }
}
}

SettingsComponentOwners::SettingsComponentOwners(const SettingsComponentOwnersInput& input)
{
  for (const auto& owner : input.owners)
    clients_.emplace(owner.name, std::make_unique<SettingsClient>(SettingsClientConfig{
                                     .target = owner.target, .credential = owner.credential, .timeout = input.timeout}));
}

SettingsComponentOwners::~SettingsComponentOwners() = default;

const SettingsClient* SettingsComponentOwners::client(const std::string& owner) const
{
  const auto found = clients_.find(owner);
  return found == clients_.end() ? nullptr : found->second.get();
}

OwnerStates SettingsComponentOwners::states(const std::string& owner, const std::vector<ComponentSpec>& specs) const
{
  const auto* link = client(owner);
  if (link == nullptr)
    return {.reach = OwnerReach::Unsupported, .states = {}};
  auto reply = attempt<std::vector<ComponentStatus>>(owner, [link, &specs] { return link->componentStates(specs); });
  return {.reach = reply.reach, .states = reply.value.value_or(std::vector<ComponentStatus>{})};
}

OwnerReply<ComponentStatus> SettingsComponentOwners::install(const std::string& owner, const ComponentSpec& spec) const
{
  const auto* link = client(owner);
  if (link == nullptr)
    return {.reach = OwnerReach::Unsupported, .value = std::nullopt};
  return attempt<ComponentStatus>(owner, [link, &spec] { return link->installComponent(spec); });
}

OwnerReply<ComponentStatus> SettingsComponentOwners::cancel(const std::string& owner, const ComponentSpec& spec) const
{
  const auto* link = client(owner);
  if (link == nullptr)
    return {.reach = OwnerReach::Unsupported, .value = std::nullopt};
  return attempt<ComponentStatus>(owner, [link, &spec] { return link->cancelComponent(spec); });
}

OwnerReply<ComponentStatus> SettingsComponentOwners::remove(const std::string& owner, const ComponentSpec& spec) const
{
  const auto* link = client(owner);
  if (link == nullptr)
    return {.reach = OwnerReach::Unsupported, .value = std::nullopt};
  return attempt<ComponentStatus>(owner, [link, &spec] { return link->removeComponent(spec); });
}

OwnerReply<ModuleDataSummary> SettingsComponentOwners::dataSummary(const std::string& owner,
                                                                   const std::string& moduleId) const
{
  const auto* link = client(owner);
  if (link == nullptr)
    return {.reach = OwnerReach::Unsupported, .value = std::nullopt};
  return attempt<ModuleDataSummary>(owner, [link, &moduleId] { return link->moduleDataSummary(moduleId); });
}

OwnerReply<ModuleDataPurge> SettingsComponentOwners::purgeData(const std::string& owner, const std::string& moduleId) const
{
  const auto* link = client(owner);
  if (link == nullptr)
    return {.reach = OwnerReach::Unsupported, .value = std::nullopt};
  return attempt<ModuleDataPurge>(owner, [link, &moduleId] { return link->purgeModuleData(moduleId); });
}

OwnerReply<PinVerdict> SettingsComponentOwners::verifyPin(const std::string& owner, const OwnerPinCheck& check) const
{
  const auto* link = client(owner);
  if (link == nullptr)
    return {.reach = OwnerReach::Unsupported, .value = std::nullopt};
  return attempt<PinVerdict>(owner, [link, &check] { return link->verifyOwnerPin(check.userId, check.pin); });
}
