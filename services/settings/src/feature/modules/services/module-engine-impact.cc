#include "module-engine.hxx"

#include <errors/error-list.hxx>
#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
#include <feature/modules/module-errors.hxx>
#include <feature/modules/services/module-resolver.hxx>
#include <runtime/blocking-task.hxx>

#include <algorithm>
#include <array>
#include <ctime>
#include <map>
#include <ranges>
#include <set>
#include <thread>
#include <utility>

namespace
{
constexpr std::string_view kIdentityOwner = "identity";
constexpr std::string_view kNotificationOwner = "notification";
constexpr std::array<UserRole, 3> kAssignable = {UserRole::Resident, UserRole::Guard, UserRole::Guest};
constexpr std::int64_t kMillisPerSecond = 1000;
constexpr std::size_t kDayLength = 16;

struct OwnerImpact
{
  std::string owner;
  OwnerReach reach{OwnerReach::Unreachable};
  ModuleImpactReport report;
};

std::string dayOf(std::int64_t unixMs)
{
  const auto seconds = static_cast<std::time_t>(unixMs / kMillisPerSecond);
  std::tm local{};
  localtime_r(&seconds, &local);
  std::array<char, kDayLength> text{};
  const auto written = std::strftime(text.data(), text.size(), "%Y-%m-%d", &local);
  return {text.data(), written};
}

std::vector<std::string> impactOwnersOf(const CatalogModule& module)
{
  std::vector<std::string> owners = module.dataOwners;
  if (!module.roles.empty() && std::ranges::find(owners, kIdentityOwner) == owners.end())
    owners.emplace_back(kIdentityOwner);
  return owners;
}
}

std::optional<ErrorDefinition> ModuleEngine::impactRefusalLocked(const CatalogModule& module, ImpactAction action) const
{
  if (module.kind == ModuleKind::Core)
    return ModuleErrors::CoreModule;
  if (openJobLocked(module.id))
    return ModuleErrors::JobRunning;
  if (action == ImpactAction::Disable && !enabled_.contains(module.id))
    return std::nullopt;
  if (!module_resolver::enabledDependents(catalog_, module.id, enabled_).empty())
    return ModuleErrors::RequiredBy;
  return std::nullopt;
}

std::vector<std::string> ModuleEngine::assignableRolesLocked(const CatalogModule& leaving) const
{
  std::vector<std::string> roles;
  for (const UserRole role : kAssignable) {
    const std::string name = userRoleToString(role);
    const bool allowed = std::ranges::all_of(catalog_.modules, [&](const CatalogModule& module) {
      if (std::ranges::find(module.roles, name) == module.roles.end())
        return true;
      return module.id != leaving.id && (module.kind == ModuleKind::Core || enabled_.contains(module.id));
    });
    if (allowed)
      roles.push_back(name);
  }
  return roles;
}

bool ModuleEngine::settingsOwnerVisible(const std::string& owner) const
{
  const std::scoped_lock lock(mutex_);
  const auto brings = [&owner](const CatalogModule& module) {
    return std::ranges::find(module.settingsOwners, owner) != module.settingsOwners.end();
  };
  const auto found = std::ranges::find_if(catalog_.modules, brings);
  return found == catalog_.modules.end() || found->kind == ModuleKind::Core || enabled_.contains(found->id);
}

ModuleImpactView ModuleEngine::impact(const ImpactCommand& command) const
{
  CatalogModule module;
  ModuleImpactView view;
  {
    const std::scoped_lock lock(mutex_);
    requireSettledLocked();
    module = moduleOrThrow(command.moduleId);
    view.refusal = impactRefusalLocked(module, command.action);
    if (command.action == ImpactAction::Uninstall)
      view.reassignRoles = assignableRolesLocked(module);
    view.filesBytes = presentBytesLocked(module);
  }
  view.moduleId = module.id;
  view.action = command.action;
  view.keepsRunning = module.keepsRunning;

  const auto owners = impactOwnersOf(module);
  std::vector<OwnerImpact> reports(owners.size());
  {
    std::vector<std::jthread> workers;
    workers.reserve(owners.size() + 1);
    workers.emplace_back([this, &view, &module] { view.data = moduleData(module.id); });
    for (const auto index : std::views::iota(std::size_t{0}, owners.size()))
      workers.emplace_back([this, &owners, &reports, &module, index] {
        auto reply = owners_.impact(owners[index], module.id);
        reports[index] = {.owner = owners[index],
                          .reach = reply.reach,
                          .report = reply.value.value_or(ModuleImpactReport{})};
      });
  }

  for (const auto& entry : reports) {
    if (entry.reach == OwnerReach::Unreachable)
      view.unreachable.push_back(entry.owner);
    view.roleHolders.insert(view.roleHolders.end(), entry.report.roleHolders.begin(), entry.report.roleHolders.end());
    view.invitations.insert(view.invitations.end(), entry.report.invitations.begin(), entry.report.invitations.end());
  }
  for (const auto& effect : module.effects) {
    StopView stop{.kind = effect, .count = std::nullopt};
    for (const auto& entry : reports)
      for (const auto& item : entry.report.stops)
        if (item.kind == effect)
          stop.count = stop.count.value_or(0) + item.count;
    view.stops.push_back(std::move(stop));
  }
  if (view.roleHolders.empty())
    view.roleEffect = "none";
  else
    view.roleEffect = command.action == ImpactAction::Disable ? "inactive" : "reassign_required";
  return view;
}

ModuleRequestView ModuleEngine::request(const RequestCommand& command) const
{
  CatalogModule module;
  bool active = false;
  bool queued = false;
  {
    const std::scoped_lock lock(mutex_);
    requireSettledLocked();
    module = moduleOrThrow(command.moduleId);
    active = module.kind == ModuleKind::Core || enabled_.contains(module.id);
    queued = openJobLocked(module.id).has_value();
  }
  if (module.kind == ModuleKind::ComingSoon)
    throw ResponseException(ModuleErrors::ComingSoon);
  if (active)
    throw ResponseException(ModuleErrors::AlreadyEnabled);
  if (queued)
    throw ResponseException(ModuleErrors::JobRunning);
  if (command.role == UserRole::Owner)
    throw ResponseException(ModuleErrors::OwnerRequest);
  const auto reply = owners_.requestModule(std::string(kNotificationOwner),
                                           {.moduleId = module.id,
                                            .moduleName = {.es = module.name.es, .en = module.name.en},
                                            .userId = command.userId,
                                            .day = dayOf(clock_())});
  if (reply.reach != OwnerReach::Answered || !reply.value)
    throw ResponseException(ModuleErrors::RequestUnavailable);
  return {.moduleId = module.id, .duplicate = reply.value->duplicate};
}

void ModuleEngine::settleRoles(const UninstallCommand& command)
{
  CatalogModule module;
  std::vector<std::string> assignable;
  {
    const std::scoped_lock lock(mutex_);
    module = moduleOrThrow(command.moduleId);
    assignable = assignableRolesLocked(module);
  }
  if (module.roles.empty())
    return;
  const auto reply = owners_.impact(std::string(kIdentityOwner), module.id);
  if (reply.reach == OwnerReach::Unsupported)
    return;
  if (reply.reach != OwnerReach::Answered || !reply.value)
    throw ResponseException(ModuleErrors::RolesUnverifiable);
  const auto& holders = reply.value->roleHolders;
  if (holders.empty())
    return;

  std::map<std::int64_t, std::string> chosen;
  for (const auto& entry : command.reassign)
    chosen[entry.userId] = entry.role;
  const bool complete = std::ranges::all_of(holders, [&](const ImpactRoleHolder& holder) {
    return chosen.contains(holder.userId);
  });
  if (!complete) {
    std::vector<ResponseError> entries;
    entries.reserve(holders.size());
    for (const auto& holder : holders)
      entries.push_back(error_list::entry(error_list::kRoleHolder, std::to_string(holder.userId) + ":" + holder.role));
    throw ResponseException(ModuleErrors::RolesHeld.status, error_list::headed(ModuleErrors::RolesHeld, std::move(entries)));
  }

  RoleReassignmentBatch batch{.actorUserId = command.userId, .reassignments = {}};
  for (const auto& holder : holders) {
    const auto& role = chosen.at(holder.userId);
    if (std::ranges::find(assignable, role) == assignable.end())
      throw ValidationException({{"reassign", {"Role " + role + " cannot be given while " + module.id + " is removed"}}});
    batch.reassignments.push_back({.userId = holder.userId, .role = role});
  }
  const auto applied = owners_.reassignRoles(std::string(kIdentityOwner), batch);
  if (applied.reach != OwnerReach::Answered || !applied.value)
    throw ResponseException(ModuleErrors::RolesUnverifiable);
  if (applied.value->status != ReassignStatus::Applied)
    throw ResponseException(ModuleErrors::ReassignRefused.withMessage(applied.value->reason.empty()
                                                                           ? std::string_view(ModuleErrors::ReassignRefused.message)
                                                                           : std::string_view(applied.value->reason)));
}

drogon::Task<ModuleImpactView> ModuleEngine::impactAsync(ImpactCommand command) const
{
  co_return co_await BlockingTask<ModuleImpactView>(
      [this, command = std::move(command)] { return impact(command); });
}

drogon::Task<ModuleRequestView> ModuleEngine::requestAsync(RequestCommand command) const
{
  co_return co_await BlockingTask<ModuleRequestView>(
      [this, command = std::move(command)] { return request(command); });
}
