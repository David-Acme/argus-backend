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
};

namespace settings_profile
{
[[nodiscard]] bool sameValue(const SettingEntry& entry, const std::string& target);

[[nodiscard]] ProfilePreview preview(const SettingsProfile& profile, const std::vector<OwnerCatalog>& catalogs);

[[nodiscard]] std::vector<OwnerPlan> plan(const SettingsProfile& profile, const std::vector<OwnerCatalog>& catalogs);

[[nodiscard]] Recommendation recommend(const ProfileCatalog& catalog, const HardwareFacts& hardware);
}

class ProfileApplication
{
public:
  explicit ProfileApplication(std::vector<OwnerPlan> plans);

  [[nodiscard]] std::vector<OwnerWrite> pendingWrites() const;
  void record(const std::vector<OwnerWriteResult>& results);
  [[nodiscard]] std::vector<OwnerApplyResult> results() const;

private:
  std::vector<OwnerPlan> plans_;
};
