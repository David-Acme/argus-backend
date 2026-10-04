#include "settings-profile.hxx"

#include <config/settings-config.hxx>

#include <algorithm>

std::string_view cpuIsaName(CpuIsa isa)
{
  switch (isa) {
  case CpuIsa::Baseline: return "baseline";
  case CpuIsa::Avx2: return "avx2";
  case CpuIsa::Avx512: return "avx512";
  case CpuIsa::Neon: return "neon";
  }
  return "baseline";
}

std::string_view recommendationReasonName(RecommendationReason reason)
{
  switch (reason) {
  case RecommendationReason::Meets: return "meets";
  case RecommendationReason::Cores: return "cores";
  case RecommendationReason::Ram: return "ram";
  case RecommendationReason::Isa: return "isa";
  }
  return "meets";
}

std::string_view profileKeyStatusName(ProfileKeyStatus status)
{
  switch (status) {
  case ProfileKeyStatus::Applied: return "applied";
  case ProfileKeyStatus::Unchanged: return "unchanged";
  case ProfileKeyStatus::Rejected: return "rejected";
  case ProfileKeyStatus::Unreachable: return "unreachable";
  }
  return "rejected";
}

const SettingsProfile* ProfileCatalog::find(std::string_view id) const
{
  const auto match = std::ranges::find(profiles, id, &SettingsProfile::id);
  return match == profiles.end() ? nullptr : &*match;
}

std::vector<std::string> ProfileCatalog::owners() const
{
  std::vector<std::string> names;
  for (const auto owner : kSettingsOwnerOrder) {
    const bool used = std::ranges::any_of(profiles, [owner](const SettingsProfile& profile) {
      return std::ranges::find(profile.owners, owner, &ProfileOwnerChanges::owner) != profile.owners.end();
    });
    if (used)
      names.emplace_back(owner);
  }
  return names;
}
