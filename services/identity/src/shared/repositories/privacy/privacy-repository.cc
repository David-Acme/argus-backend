#include "privacy-repository.hxx"

#include <sqlite/db-service.hxx>
#include <string>
#include <vector>

using namespace privacy_query;

namespace
{
int64_t flag(bool value)
{
  return value ? 1 : 0;
}
}

drogon::Task<std::optional<UserPrivacySchema>>
PrivacyRepository::findUser(int64_t userId, drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::identityClient();
  auto* resolved = client ? client : pooled.get();
  const auto rows = co_await resolved->execSqlCoro(std::string{FIND_USER}, userId);
  if (rows.empty())
    co_return std::nullopt;
  co_return UserPrivacySchema(rows.front());
}

drogon::Task<std::vector<UserPrivacySchema>>
PrivacyRepository::findAllUsers(drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::identityClient();
  auto* resolved = client ? client : pooled.get();
  const auto rows = co_await resolved->execSqlCoro(std::string{FIND_ALL_USERS});
  std::vector<UserPrivacySchema> records;
  records.reserve(rows.size());
  for (const auto& row : rows)
    records.emplace_back(row);
  co_return records;
}

drogon::Task<UserPrivacySchema>
PrivacyRepository::upsertUser(const UserPrivacyUpsertInput& input) const
{
  const auto pooled = DbService::identityClient();
  auto* client = input.client ? input.client : pooled.get();
  co_await client->execSqlCoro(std::string{UPSERT_USER}, input.userId,
                               input.noticeVersion,
                               flag(input.choices.presence),
                               flag(input.choices.faceCameras),
                               flag(input.choices.voiceLearning),
                               flag(input.choices.cameraAudio));
  const auto stored = co_await findUser(input.userId, client);
  co_return stored.value_or(UserPrivacySchema{});
}

drogon::Task<HouseholdPrivacySchema>
PrivacyRepository::household(drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::identityClient();
  auto* resolved = client ? client : pooled.get();
  auto rows = co_await resolved->execSqlCoro(std::string{FIND_HOUSEHOLD});
  if (rows.empty()) {
    co_await resolved->execSqlCoro(std::string{ENSURE_HOUSEHOLD});
    rows = co_await resolved->execSqlCoro(std::string{FIND_HOUSEHOLD});
  }
  if (rows.empty())
    co_return HouseholdPrivacySchema{};
  co_return HouseholdPrivacySchema(rows.front());
}

drogon::Task<HouseholdPrivacySchema>
PrivacyRepository::updateHousehold(const HouseholdPrivacyUpdateInput& input) const
{
  const auto pooled = DbService::identityClient();
  auto* client = input.client ? input.client : pooled.get();
  co_await household(client);

  std::string sql{UPDATE_HOUSEHOLD_PREFIX};
  std::vector<std::string> args;
  const auto add = [&](std::string_view column, const std::optional<bool>& value) {
    if (!value)
      return;
    if (!args.empty())
      sql += ", ";
    sql += column;
    args.emplace_back(*value ? "1" : "0");
  };
  add(UPDATE_COL_PRESENCE, input.presence);
  add(UPDATE_COL_FACE_CAMERAS, input.faceCameras);
  add(UPDATE_COL_VOICE_LEARNING, input.voiceLearning);
  add(UPDATE_COL_CAMERA_AUDIO, input.cameraAudio);
  add(UPDATE_COL_VISITOR_RECOGNITION, input.visitorRecognition);
  if (input.visitorRecognition.value_or(false)) {
    sql += ", ";
    sql += UPDATE_COL_VISITOR_ACK;
    args.push_back(std::to_string(input.updatedBy));
    args.push_back(std::to_string(kPrivacyNoticeVersion));
  }
  if (args.empty())
    co_return co_await household(client);

  sql += UPDATE_HOUSEHOLD_SUFFIX;
  args.push_back(std::to_string(input.updatedBy));
  const auto& argsRef = args;
  co_await client->execSqlCoro(sql, argsRef);
  co_return co_await household(client);
}
