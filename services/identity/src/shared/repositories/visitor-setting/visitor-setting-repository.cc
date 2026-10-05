#include "visitor-setting-repository.hxx"

#include <algorithm>
#include <sqlite/db-service.hxx>
#include <string>

using namespace visitor_setting_query;

drogon::Task<VisitorSetting> VisitorSettingRepository::find() const
{
  const auto rows = co_await DbService::client()->execSqlCoro(std::string(FIND));
  VisitorSetting setting;
  if (rows.empty())
    co_return setting;
  const auto& row = rows.front();
  setting.unnamedRetentionDays =
      std::clamp(row["unnamed_retention_days"].as<int64_t>(),
                 kMinUnnamedRetentionDays, kMaxUnnamedRetentionDays);
  if (!row["updated_by"].isNull())
    setting.updatedBy = row["updated_by"].as<int64_t>();
  if (!row["updated_at"].isNull())
    setting.updatedAt = row["updated_at"].as<int64_t>();
  co_return setting;
}

drogon::Task<VisitorSetting>
VisitorSettingRepository::update(const VisitorSettingUpdateInput& input) const
{
  const int64_t days = std::clamp(input.unnamedRetentionDays,
                                  kMinUnnamedRetentionDays,
                                  kMaxUnnamedRetentionDays);
  co_await DbService::client()->execSqlCoro(std::string(UPDATE_RETENTION), days,
                                            input.updatedBy);
  co_return co_await find();
}
