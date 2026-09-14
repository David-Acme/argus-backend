#include "guard-feature-service.hxx"

#include <ctime>
#include <identity/identity-client.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>

GuardFeatureService::GuardFeatureService(IdentityClient* identity)
    : identity_(identity)
{
}

drogon::Task<bool> GuardFeatureService::promotePerson(
    const PromotePersonInput& input) const
{
  if (identity_ == nullptr || input.personId <= 0 ||
      input.accessToken.empty())
    co_return false;
  co_return co_await BlockingTask<bool>(
      [this, input]() {
        return identity_->promotePerson(
            {.personId = input.personId,
             .accessToken = input.accessToken,
             .deviceHash = input.deviceHash});
      });
}

drogon::Task<std::string> GuardFeatureService::mode() const
{
  co_return co_await repository_.state("mode", "home");
}

drogon::Task<std::string> GuardFeatureService::setMode(
    const std::string& mode) const
{
  const std::string normalized = guardModeToString(guardModeFromString(mode));
  co_await repository_.setState(
      {.key = "mode",
       .value = normalized,
       .updatedAt = static_cast<int64_t>(std::time(nullptr))});
  co_return normalized;
}

drogon::Task<Json::Value> GuardFeatureService::incidents(int limit) const
{
  const auto rows = co_await repository_.recentIncidents(limit);
  Json::Value response(Json::arrayValue);
  for (const auto& row : rows)
    response.append(row);
  co_return response;
}

drogon::Task<int64_t> GuardFeatureService::createGuest(
    const CreateExpectedGuestDto& input) const
{
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const int64_t validFrom = input.validFrom > 0 ? input.validFrom : now;
  const int64_t validUntil = input.validUntil > 0
                                 ? input.validUntil
                                 : now + static_cast<int64_t>(input.hours) * 3600;
  co_return co_await repository_.insertGuest(
      {.description = input.description,
       .cameraId = input.cameraId,
       .personId = input.personId,
       .hostUserId = input.hostUserId,
       .oneTime = input.oneTime,
       .validFrom = validFrom,
       .validUntil = validUntil});
}

drogon::Task<Json::Value> GuardFeatureService::guests() const
{
  const auto rows = co_await repository_.listGuests();
  Json::Value response(Json::arrayValue);
  for (const auto& guest : rows) {
    Json::Value row;
    row["id"] = Json::Int64(guest.id);
    row["description"] = guest.description;
    row["cameraId"] = Json::Int64(guest.cameraId);
    row["personId"] = Json::Int64(guest.personId);
    row["hostUserId"] = Json::Int64(guest.hostUserId);
    row["oneTime"] = guest.oneTime;
    row["validFrom"] = Json::Int64(guest.validFrom);
    row["validUntil"] = Json::Int64(guest.validUntil);
    response.append(row);
  }
  co_return response;
}

drogon::Task<bool> GuardFeatureService::removeGuest(int64_t id) const
{
  co_return co_await repository_.removeGuest(id);
}
