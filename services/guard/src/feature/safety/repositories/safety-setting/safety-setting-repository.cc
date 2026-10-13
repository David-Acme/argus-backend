#include "safety-setting-repository.hxx"

#include <sqlite/db-service.hxx>

#include <string>

using namespace safety_setting_query;

drogon::Task<SafetySetting> SafetySettingRepository::find() const
{
  const auto rows =
      co_await DbService::client()->execSqlCoro(std::string(SELECT_SETTING));
  if (rows.empty())
    co_return SafetySetting{};
  const auto& row = rows.front();
  co_return SafetySetting{.duressEnabled = row["duress_enabled"].as<int>() != 0,
                          .updatedAt = row["updated_at"].as<int64_t>(),
                          .updatedBy = row["updated_by"].as<int64_t>(),
                          .phraseKey = row["duress_phrase_key"].as<std::string>()};
}

drogon::Task<void>
SafetySettingRepository::update(const SafetySettingUpdateInput& input) const
{
  const auto owned = DbService::client();
  drogon::orm::DbClient* client =
      input.client != nullptr ? input.client : owned.get();
  co_await client->execSqlCoro(std::string(UPSERT_SETTING),
                               input.duressEnabled ? 1 : 0, input.now,
                               input.updatedBy);
}

drogon::Task<void>
SafetySettingRepository::setPhrase(const SafetyPhraseUpdateInput& input) const
{
  const auto owned = DbService::client();
  drogon::orm::DbClient* client =
      input.client != nullptr ? input.client : owned.get();
  co_await client->execSqlCoro(std::string(UPSERT_PHRASE), input.phraseKey,
                               input.now, input.updatedBy);
}
