#pragma once

#include <feature/settings/services/settings-profile.hxx>

#include <optional>
#include <string>
#include <vector>

struct PlannedKey
{
  ProfileKeyResult result;
  bool send{false};
};

struct OwnerPlan
{
  std::string service;
  bool reachable{false};
  std::vector<PlannedKey> keys;
  std::optional<OwnerCatalog> catalog;
  std::optional<ProfileMarker> marker{};
  bool markerRecorded{false};
};

namespace settings_profile
{
[[nodiscard]] std::vector<SettingChange> effectiveChanges(const ProfileOwnerChanges& owner,
                                                          const OwnerCatalog* catalog);

[[nodiscard]] bool sameValue(const SettingEntry& entry, const std::string& target);

[[nodiscard]] ProfilePreview preview(const SettingsProfile& profile, const std::vector<OwnerCatalog>& catalogs);

[[nodiscard]] std::vector<OwnerPlan> plan(const SettingsProfile& profile, const std::vector<OwnerCatalog>& catalogs);

[[nodiscard]] Recommendation recommend(const ProfileCatalog& catalog, const HardwareFacts& hardware);

[[nodiscard]] OwnerPlan firstRunPlan(const SettingsProfile& profile, const OwnerCatalog& catalog);

[[nodiscard]] OwnerPlan revertPlan(const ProfileCatalog& profiles, const OwnerCatalog& catalog);

[[nodiscard]] std::optional<FirstRunState> firstRunState(const std::vector<OwnerCatalog>& catalogs);
}

class ProfileApplication
{
public:
  explicit ProfileApplication(std::vector<OwnerPlan> plans);

  [[nodiscard]] std::vector<OwnerWrite> pendingWrites() const;
  [[nodiscard]] std::vector<OwnerWrite> markerWrites() const;
  void record(const std::vector<OwnerWriteResult>& results);
  [[nodiscard]] std::vector<OwnerApplyResult> results() const;

private:
  std::vector<OwnerPlan> plans_;
};
