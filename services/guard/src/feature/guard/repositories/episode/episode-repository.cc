#include "episode-repository.hxx"

#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <exception>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

using namespace episode_query;

namespace
{
EpisodeRow episodeFromRow(const drogon::orm::Row& row)
{
  return {.id = row["id"].as<int64_t>(),
          .state = row["state"].as<std::string>(),
          .checks = row["checks"].as<int>(),
          .cameraId = row["best_camera_id"].as<int64_t>(),
          .firstSeen = row["first_seen"].as<int64_t>(),
          .lastSeen = row["last_seen"].as<int64_t>(),
          .notifyCount = row["notify_count"].as<int>(),
          .notifyHighestRank = row["notify_highest_rank"].as<int>(),
          .subject = row["subject"].as<std::string>(),
          .people = row["people"].as<int>(),
          .reasons = row["reasons"].as<std::string>(),
          .reasonsRank = row["reasons_rank"].as<int>(),
          .groupId = row["group_id"].as<int64_t>(),
          .reviewLabel = row["review_label"].as<std::string>(),
          .reviewedAt = row["reviewed_at"].as<int64_t>(),
          .lastReason = row["last_reason"].isNull()
                            ? std::string{}
                            : row["last_reason"].as<std::string>(),
          .spoke = row["spoke"].as<int>() != 0,
          .sounded = row["sounded"].as<int>() != 0,
          .environmentId = row["environment_id"].as<int64_t>(),
          .retainUntil = row["retain_until"].as<int64_t>()};
}

constexpr std::string_view kTamperOnsetPrefix = "tamper_onset_";
}

drogon::Task<std::vector<EpisodeRow>>
EpisodeRepository::list(const EpisodeListInput& input) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(
      std::string(LIST_EPISODES), input.before, input.before,
      input.environmentId, input.environmentId, input.limit);
  std::vector<EpisodeRow> episodes;
  episodes.reserve(rows.size());
  for (const auto& row : rows)
    episodes.push_back(episodeFromRow(row));
  co_return episodes;
}

drogon::Task<std::optional<EpisodeRow>>
EpisodeRepository::find(int64_t encounterId) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(
      std::string(FIND_EPISODE), encounterId);
  if (rows.empty())
    co_return std::nullopt;
  co_return episodeFromRow(rows.front());
}

drogon::Task<std::vector<EpisodeTimelineRow>>
EpisodeRepository::timeline(int64_t encounterId) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(
      std::string(EPISODE_TIMELINE), encounterId, encounterId, encounterId);
  std::vector<EpisodeTimelineRow> entries;
  entries.reserve(rows.size());
  for (const auto& row : rows)
    entries.push_back({.entry = row["entry"].as<std::string>(),
                       .at = row["at"].as<int64_t>(),
                       .what = row["what"].as<std::string>(),
                       .detail = row["detail"].as<std::string>(),
                       .reasons = row["reasons"].as<std::string>(),
                       .notified = row["notified"].as<int>() != 0});
  co_return entries;
}

drogon::Task<std::vector<TamperRow>>
EpisodeRepository::tamper(const EpisodeListInput& input) const
{
  const auto client = DbService::client();
  const auto rows = co_await client->execSqlCoro(
      std::string(LIST_TAMPER), input.before, input.before,
      input.environmentId, input.environmentId, input.limit);
  const auto onsets = co_await client->execSqlCoro(std::string(OPEN_TAMPER_KEYS));
  std::unordered_set<int64_t> openCameras;
  for (const auto& onset : onsets) {
    const auto key = onset["key"].as<std::string>();
    try {
      openCameras.insert(std::stoll(key.substr(kTamperOnsetPrefix.size())));
    }
    catch (const std::exception&) {
      LOG_WARN << "Guard episodes: unreadable tamper onset key " << key;
    }
  }
  std::vector<TamperRow> tampered;
  tampered.reserve(rows.size());
  std::unordered_set<int64_t> seen;
  for (const auto& row : rows) {
    const int64_t cameraId = row["camera_id"].as<int64_t>();
    const Json::Value event =
        json_util::fromString(row["event_json"].as<std::string>());
    tampered.push_back(
        {.incidentId = row["id"].as<int64_t>(),
         .cameraId = cameraId,
         .cameraName = row["camera_name"].as<std::string>(),
         .danger = row["danger"].as<std::string>(),
         .status = event.isObject() ? event.get("status", "").asString()
                                    : std::string{},
         .createdAt = row["created_at"].as<int64_t>(),
         .open = openCameras.contains(cameraId) && seen.insert(cameraId).second,
         .environmentId = row["environment_id"].as<int64_t>()});
  }
  co_return tampered;
}

drogon::Task<bool>
EpisodeRepository::recordInsight(const EpisodeInsightInput& input) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(UPDATE_INSIGHT), input.subject, input.people, input.rank,
      input.reasons, input.rank, input.environmentId, input.environmentId,
      input.encounterId);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> EpisodeRepository::linkGroup(int64_t encounterId,
                                                int64_t groupId) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(LINK_GROUP), groupId, encounterId);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> EpisodeRepository::review(const EpisodeReviewInput& input) const
{
  std::shared_ptr<drogon::orm::Transaction> transaction;
  try {
    transaction = co_await db_transaction::begin(DbService::client());
  }
  catch (const std::exception& error) {
    LOG_WARN << "Guard episode review could not open a transaction: "
             << error.what();
    co_return false;
  }
  const std::string label = feedbackLabelToString(input.label);
  try {
    const auto updated = co_await transaction->execSqlCoro(
        std::string(REVIEW_EPISODE), label, input.at, input.encounterId);
    if (updated.affectedRows() == 0) {
      transaction->rollback();
      co_return false;
    }
    co_await transaction->execSqlCoro(std::string(REVIEW_DECISIONS), label, input.at,
                                      input.encounterId);
  }
  catch (const std::exception& error) {
    transaction->rollback();
    LOG_WARN << "Guard episode review failed: " << error.what();
    co_return false;
  }
  co_return co_await db_transaction::Commit(std::move(transaction));
}

drogon::Task<std::optional<CameraNotification>>
EpisodeRepository::recentCameraNotification(
    const CameraNotificationLookupInput& input) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(
      std::string(RECENT_CAMERA_NOTIFICATION), input.cameraId,
      input.excludeEncounterId, input.since);
  if (rows.empty())
    co_return std::nullopt;
  co_return CameraNotification{
      .encounterId = rows.front()["encounter_id"].as<int64_t>(),
      .rank = rows.front()["rank"].as<int>()};
}

drogon::Task<std::vector<DigestRow>>
EpisodeRepository::digest(const DigestWindowInput& input) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(
      std::string(DIGEST_WINDOW), input.from, input.to, input.environmentId);
  std::vector<DigestRow> lines;
  lines.reserve(rows.size());
  for (const auto& row : rows)
    lines.push_back({.cameraId = row["camera_id"].as<int64_t>(),
                     .cameraName = row["camera_name"].as<std::string>(),
                     .notified = row["notified"].as<int64_t>(),
                     .held = row["held"].as<int64_t>(),
                     .routine = row["routine"].as<int64_t>()});
  co_return lines;
}

drogon::Task<bool> EpisodeRepository::retain(const EpisodeRetainInput& input) const
{
  auto transaction = co_await db_transaction::begin(DbService::client());
  try {
    const auto updated = co_await transaction->execSqlCoro(
        std::string(RETAIN_EPISODE), input.retainUntil, input.encounterId);
    if (updated.affectedRows() == 0) {
      transaction->rollback();
      co_return false;
    }
    co_await transaction->execSqlCoro(std::string(RETAIN_EVIDENCE), input.retainUntil,
                                      input.standardExpiry, input.encounterId);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return co_await db_transaction::Commit(std::move(transaction));
}
