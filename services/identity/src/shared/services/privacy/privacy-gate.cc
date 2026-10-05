#include "privacy-gate.hxx"

namespace privacy_policy
{

PrivacyState stateOf(const std::optional<UserPrivacySchema>& record,
                     const HouseholdPrivacySchema& household)
{
  PrivacyState state;
  if (!record || record->noticeVersion <= 0)
    return state;
  state.noticeVersion = record->noticeVersion;
  state.decided = true;
  state.decidedAt = record->decidedAt;
  state.updatedAt = record->updatedAt;
  state.choices = record->choices;
  state.effective = record->choices.both(household.allowed);
  return state;
}

Json::Value toJson(const PrivacyState& state)
{
  Json::Value json = state.effective.toJson();
  json["noticeVersion"] = static_cast<Json::Int64>(state.noticeVersion);
  json["decided"] = state.decided;
  return json;
}

}

drogon::Task<PrivacyState>
PrivacyGate::stateFor(int64_t userId, drogon::orm::DbClient* client) const
{
  const auto record = co_await repository_.findUser(userId, client);
  const auto household = co_await repository_.household(client);
  co_return privacy_policy::stateOf(record, household);
}

drogon::Task<PrivacyChoices>
PrivacyGate::effectiveFor(int64_t userId, drogon::orm::DbClient* client) const
{
  co_return (co_await stateFor(userId, client)).effective;
}

drogon::Task<HouseholdPrivacySchema>
PrivacyGate::household(drogon::orm::DbClient* client) const
{
  co_return co_await repository_.household(client);
}

drogon::Task<std::unordered_map<int64_t, PrivacyState>>
PrivacyGate::statesByUser(drogon::orm::DbClient* client) const
{
  const auto household = co_await repository_.household(client);
  std::unordered_map<int64_t, PrivacyState> states;
  for (const auto& row : co_await repository_.findAllUsers(client)) {
    const std::optional<UserPrivacySchema> record = row;
    states.emplace(row.userId, privacy_policy::stateOf(record, household));
  }
  co_return states;
}
