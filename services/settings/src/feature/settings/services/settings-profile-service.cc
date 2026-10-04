#include "settings-profile-service.hxx"

#include <errors/response-exception.hxx>
#include <feature/settings/dtos/setting-names.hxx>
#include <feature/settings/services/profile-planner.hxx>
#include <feature/settings/settings-gateway-errors.hxx>
#include <runtime/blocking-task.hxx>
#include <trantor/utils/Logger.h>

#include <utility>

namespace
{
std::string keysWith(const OwnerApplyResult& owner, ProfileKeyStatus status)
{
  std::string text;
  for (const auto& result : owner.results) {
    if (result.status != status)
      continue;
    if (!text.empty())
      text += ", ";
    text += result.key;
    if (result.reason)
      text += ": " + rejectionReasonName(*result.reason);
  }
  return text;
}

std::string summaryOf(const std::vector<OwnerApplyResult>& owners)
{
  std::string summary;
  for (const auto& owner : owners) {
    if (!summary.empty())
      summary += "; ";
    summary += owner.service;
    if (!owner.reachable) {
      summary += " unreachable [" + keysWith(owner, ProfileKeyStatus::Unreachable) + "]";
      continue;
    }
    summary += " applied [" + keysWith(owner, ProfileKeyStatus::Applied) + "]";
    const auto rejected = keysWith(owner, ProfileKeyStatus::Rejected);
    if (!rejected.empty())
      summary += " rejected [" + rejected + "]";
  }
  return summary;
}
}

SettingsProfileService::SettingsProfileService(SettingsProfileInput input)
    : gateway_(input.gateway), catalog_(std::move(input.catalog)), hardware_(input.hardware)
{
}

ProfilesOverview SettingsProfileService::overview() const
{
  const auto& profiles = catalog();
  const auto catalogs = gateway_.catalogsOf(profiles.owners());
  ProfilesOverview overview{.profiles = {}, .recommendation = settings_profile::recommend(profiles, hardware_)};
  overview.profiles.reserve(profiles.profiles.size());
  for (const auto& profile : profiles.profiles)
    overview.profiles.push_back(settings_profile::preview(profile, catalogs));
  return overview;
}

drogon::Task<ProfilesOverview> SettingsProfileService::overviewAsync() const
{
  co_return co_await BlockingTask<ProfilesOverview>([this] { return overview(); });
}

ProfileApplyOutcome SettingsProfileService::apply(const ProfileApplyInput& input) const
{
  const auto* profile = catalog().find(input.profile);
  if (profile == nullptr)
    throw ResponseException(SettingsGatewayErrors::UnknownProfile);

  std::vector<std::string> owners;
  owners.reserve(profile->owners.size());
  for (const auto& owner : profile->owners)
    owners.push_back(owner.owner);

  ProfileApplication application(settings_profile::plan(*profile, gateway_.catalogsOf(owners)));
  for (auto writes = application.pendingWrites(); !writes.empty(); writes = application.pendingWrites())
    application.record(gateway_.write(writes));

  ProfileApplyOutcome outcome{.profile = profile->id, .owners = application.results()};
  LOG_INFO << "User " << input.userId << " applied settings profile " << profile->id << ": "
           << summaryOf(outcome.owners);
  return outcome;
}

drogon::Task<ProfileApplyOutcome> SettingsProfileService::applyAsync(ProfileApplyInput input) const
{
  co_return co_await BlockingTask<ProfileApplyOutcome>([this, input = std::move(input)] { return apply(input); });
}

const ProfileCatalog& SettingsProfileService::catalog() const
{
  if (!catalog_)
    throw ResponseException(SettingsGatewayErrors::ProfilesUnavailable);
  return *catalog_;
}
