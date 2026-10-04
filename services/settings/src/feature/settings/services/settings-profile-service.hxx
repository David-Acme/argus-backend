#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/settings/services/settings-gateway-service.hxx>
#include <feature/settings/services/settings-profile.hxx>

#include <cstdint>
#include <optional>
#include <string>

struct SettingsProfileInput
{
  const SettingsGatewayService& gateway;
  std::optional<ProfileCatalog> catalog;
  HardwareFacts hardware;
};

struct ProfileApplyInput
{
  std::string profile;
  int64_t userId{0};
};

class SettingsProfileService
{
public:
  explicit SettingsProfileService(SettingsProfileInput input);

  [[nodiscard]] ProfilesOverview overview() const;
  [[nodiscard]] drogon::Task<ProfilesOverview> overviewAsync() const;

  [[nodiscard]] ProfileApplyOutcome apply(const ProfileApplyInput& input) const;
  [[nodiscard]] drogon::Task<ProfileApplyOutcome> applyAsync(ProfileApplyInput input) const;

private:
  [[nodiscard]] const ProfileCatalog& catalog() const;

  const SettingsGatewayService& gateway_;
  std::optional<ProfileCatalog> catalog_;
  HardwareFacts hardware_;
};
