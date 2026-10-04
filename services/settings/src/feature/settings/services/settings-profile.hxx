#pragma once

#include <config/settings-registry.hxx>
#include <feature/settings/services/settings-gateway-service.hxx>
#include <runtime/hardware-profile.hxx>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class CpuIsa : std::uint8_t
{
  Baseline,
  Avx2,
  Avx512,
  Neon
};

enum class RecommendationReason : std::uint8_t
{
  Meets,
  Cores,
  Ram,
  Isa
};

enum class ProfileKeyStatus : std::uint8_t
{
  Applied,
  Unchanged,
  Rejected,
  Unreachable
};

[[nodiscard]] std::string_view cpuIsaName(CpuIsa isa);
[[nodiscard]] std::string_view recommendationReasonName(RecommendationReason reason);
[[nodiscard]] std::string_view profileKeyStatusName(ProfileKeyStatus status);

struct HardwareFacts
{
  int cores{1};
  int threads{1};
  double ramGb{0};
  CpuIsa isa{CpuIsa::Baseline};
  VideoAccel gpu{VideoAccel::None};

  [[nodiscard]] bool vectorIsa() const { return isa != CpuIsa::Baseline; }
};

struct ProfileOwnerChanges
{
  std::string owner;
  std::vector<SettingChange> changes;
};

struct SettingsProfile
{
  std::string id;
  std::string labelKey;
  std::vector<ProfileOwnerChanges> owners;
};

struct RecommendationRule
{
  std::string profile;
  int minCores{0};
  double minRamGb{0};
  bool vectorIsa{false};
};

struct ProfileCatalog
{
  std::vector<SettingsProfile> profiles;
  std::vector<RecommendationRule> rules;
  std::string fallback;

  [[nodiscard]] const SettingsProfile* find(std::string_view id) const;
  [[nodiscard]] std::vector<std::string> owners() const;
};

struct ProfileChange
{
  std::string key;
  std::optional<std::string> from;
  std::string to;
  std::optional<SettingApply> apply;
  std::optional<ChoiceState> install;
  bool changed{true};
};

struct OwnerPreview
{
  std::string service;
  bool reachable{false};
  std::vector<ProfileChange> changes;
};

struct ProfilePreview
{
  std::string id;
  std::string labelKey;
  bool current{false};
  std::vector<OwnerPreview> owners;
};

struct Recommendation
{
  std::string profile;
  RecommendationReason reason{RecommendationReason::Meets};
  HardwareFacts hardware;
  std::optional<RecommendationRule> rule;
  std::optional<RecommendationRule> missed;
  std::vector<RecommendationRule> rules;
  std::string fallback;
};

struct ProfilesOverview
{
  std::vector<ProfilePreview> profiles;
  Recommendation recommendation;
};

struct ProfileKeyResult
{
  std::string key;
  std::optional<std::string> from;
  std::string to;
  ProfileKeyStatus status{ProfileKeyStatus::Unchanged};
  std::optional<SettingRejectionReason> reason;
};

struct OwnerApplyResult
{
  std::string service;
  bool reachable{false};
  std::vector<ProfileKeyResult> results;
  std::optional<OwnerCatalog> catalog;
};

struct ProfileApplyOutcome
{
  std::string profile;
  std::vector<OwnerApplyResult> owners;
};
