#include "voice-device-repository.hxx"

#include <sqlite/db-service.hxx>

using namespace voice_device_query;

drogon::Task<VoiceDeviceSchema>
VoiceDeviceRepository::record(const VoiceDeviceRecordInput& input) const
{
  const auto client = DbService::identityClient();
  const auto rows = co_await client->execSqlCoro(
      RECORD, input.deviceHash, input.userId, input.matched, input.conflicting,
      input.mixed, input.now);
  co_return VoiceDeviceSchema(rows.front());
}

drogon::Task<std::unordered_set<std::string>>
VoiceDeviceRepository::sharedFor(int64_t userId) const
{
  const auto client = DbService::identityClient();
  const auto rows = co_await client->execSqlCoro(SHARED_FOR_USER, userId);
  std::unordered_set<std::string> shared;
  for (const auto& row : rows)
    shared.insert(row["device_hash"].as<std::string>());
  co_return shared;
}

drogon::Task<int>
VoiceDeviceRepository::otherUsersOn(const std::string& deviceHash,
                                    int64_t userId) const
{
  const auto client = DbService::identityClient();
  const auto rows =
      co_await client->execSqlCoro(OTHER_USERS_ON_DEVICE, deviceHash, userId);
  co_return rows.empty() ? 0 : rows.front()["total"].as<int>();
}

drogon::Task<size_t>
VoiceDeviceRepository::removeByUser(int64_t userId,
                                    drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::identityClient();
  auto* resolved = client ? client : pooled.get();
  const auto result = co_await resolved->execSqlCoro(DELETE_BY_USER, userId);
  co_return result.affectedRows();
}
