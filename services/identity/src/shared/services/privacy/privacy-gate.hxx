#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/repositories/privacy/privacy-repository.hxx>
#include <shared/vocabulary/privacy-choices.hxx>
#include <unordered_map>

namespace privacy_policy
{
[[nodiscard]] PrivacyState stateOf(const std::optional<UserPrivacySchema>& record,
                                    const HouseholdPrivacySchema& household);
[[nodiscard]] Json::Value toJson(const PrivacyState& state);
}

class PrivacyGate
{
public:
  PrivacyGate() = default;

  [[nodiscard]] drogon::Task<PrivacyState>
  stateFor(int64_t userId, drogon::orm::DbClient* client = nullptr) const;

  [[nodiscard]] drogon::Task<PrivacyChoices>
  effectiveFor(int64_t userId, drogon::orm::DbClient* client = nullptr) const;

  [[nodiscard]] drogon::Task<HouseholdPrivacySchema>
  household(drogon::orm::DbClient* client = nullptr) const;

  [[nodiscard]] drogon::Task<std::unordered_map<int64_t, PrivacyState>>
  statesByUser(drogon::orm::DbClient* client = nullptr) const;

private:
  PrivacyRepository repository_;
};
