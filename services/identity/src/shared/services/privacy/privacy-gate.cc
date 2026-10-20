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

PrivacyState withMinor(PrivacyState state, const std::optional<UserMinorSchema>& minor)
{
  if (!minor)
    return state;
  state.minor = true;
  state.guardianConsent = minor->guardianConsent;
  if (!minor->guardianConsent) {
    state.effective.faceCameras = false;
    state.effective.voiceLearning = false;
  }
  return state;
}

Json::Value toJson(const PrivacyState& state)
{
  Json::Value json = state.effective.toJson();
  json["noticeVersion"] = static_cast<Json::Int64>(state.noticeVersion);
  json["decided"] = state.decided;
  json["minor"] = state.minor;
  json["guardianConsent"] = state.guardianConsent;
  return json;
}

}

drogon::Task<PrivacyState>
PrivacyGate::stateFor(int64_t userId, drogon::orm::DbClient* client) const
{
  const auto record = co_await repository_.findUser(userId, client);
  const auto household = co_await repository_.household(client);
  const auto minor = co_await minorRepository_.find(userId, client);
  co_return privacy_policy::withMinor(privacy_policy::stateOf(record, household), minor);
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
  for (const auto& minor : co_await minorRepository_.findAll(client)) {
    const std::optional<UserMinorSchema> mark = minor;
    auto found = states.find(minor.userId);
    if (found == states.end())
      found = states.emplace(minor.userId, PrivacyState{}).first;
    found->second = privacy_policy::withMinor(found->second, mark);
  }
  co_return states;
}
