#include "visitor-repository.hxx"

#include <stdexcept>

#include <shared/vocabulary/face-model.hxx>
#include <sqlite/db-service.hxx>

#include <cstring>
#include <limits>
#include <sstream>
#include <string>

using namespace visitor_query;

namespace
{
constexpr int64_t kUnbounded = std::numeric_limits<int64_t>::max();

std::string idList(std::span<const int64_t> ids)
{
  std::string out;
  for (const int64_t id : ids) {
    if (!out.empty())
      out += ',';
    out += std::to_string(id);
  }
  return out;
}

std::vector<int64_t> cameraIdsOf(const drogon::orm::Field& field)
{
  std::vector<int64_t> ids;
  if (field.isNull())
    return ids;
  std::stringstream stream(field.as<std::string>());
  std::string part;
  while (std::getline(stream, part, ','))
    if (!part.empty())
      ids.push_back(std::stoll(part));
  return ids;
}

std::vector<float> floatsOf(const std::string& blob)
{
  std::vector<float> values(blob.size() / sizeof(float));
  std::memcpy(values.data(), blob.data(), values.size() * sizeof(float));
  return values;
}

VisitorRow visitorOf(const drogon::orm::Row& row)
{
  VisitorRow visitor;
  visitor.id = row["id"].as<int64_t>();
  visitor.name = row["name"].as<std::string>();
  visitor.category = personCategoryFromString(row["category"].as<std::string>())
                         .value_or(PersonCategory::None);
  visitor.note = row["note"].as<std::string>();
  visitor.known = row["status"].as<std::string>() == "known";
  if (!row["visitor_number"].isNull())
    visitor.visitorNumber = row["visitor_number"].as<int64_t>();
  visitor.visitCount = row["visit_count"].as<int64_t>();
  visitor.firstSeenAt = row["first_seen_at"].as<int64_t>();
  visitor.lastSeenAt = row["last_seen_at"].as<int64_t>();
  visitor.sampleCount = row["sample_count"].as<int64_t>();
  if (!row["cover_sample_id"].isNull())
    visitor.coverSampleId = row["cover_sample_id"].as<int64_t>();
  visitor.cameraIds = cameraIdsOf(row["camera_ids"]);
  return visitor;
}

drogon::orm::DbClient& clientOr(drogon::orm::DbClient* client,
                                const drogon::orm::DbClientPtr& pooled)
{
  return client != nullptr ? *client : *pooled;
}
}

drogon::Task<std::vector<VisitorRow>>
VisitorRepository::list(const VisitorListInput& input) const
{
  const auto& page = input.page;
  const bool fromTop = page.after.lastSeenAt == 0 && page.after.id == 0;
  const auto rows = co_await DbService::client()->execSqlCoro(
      std::string(LIST), std::string(kFaceModelId), input.namedOnly ? 1 : 0,
      page.limit, std::string(visitorListFilterToString(page.filter)),
      page.search, fromTop ? kUnbounded : page.after.lastSeenAt,
      fromTop ? kUnbounded : page.after.id);
  std::vector<VisitorRow> visitors;
  visitors.reserve(rows.size());
  for (const auto& row : rows)
    visitors.push_back(visitorOf(row));
  co_return visitors;
}

drogon::Task<std::optional<VisitorRow>>
VisitorRepository::find(int64_t id, drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::client();
  const auto rows = co_await clientOr(client, pooled)
                        .execSqlCoro(std::string(FIND), id,
                                     std::string(kFaceModelId));
  if (rows.empty())
    co_return std::nullopt;
  co_return visitorOf(rows.front());
}

drogon::Task<std::vector<VisitorSampleRow>>
VisitorRepository::samples(int64_t personId, drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::client();
  const auto rows = co_await clientOr(client, pooled)
                        .execSqlCoro(std::string(SAMPLES), personId,
                                     std::string(kFaceModelId));
  std::vector<VisitorSampleRow> samples;
  samples.reserve(rows.size());
  for (const auto& row : rows) {
    VisitorSampleRow sample;
    sample.id = row["id"].as<int64_t>();
    sample.quality = row["quality"].as<double>();
    if (!row["camera_id"].isNull())
      sample.cameraId = row["camera_id"].as<int64_t>();
    sample.hasCrop = row["has_crop"].as<int64_t>() != 0;
    sample.createdAt = row["created_at"].as<int64_t>();
    sample.embedding = floatsOf(row["embedding"].as<std::string>());
    samples.push_back(std::move(sample));
  }
  co_return samples;
}

drogon::Task<std::vector<VisitorVisitRow>>
VisitorRepository::visits(int64_t personId, int64_t limit) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(
      std::string(VISITS), personId, limit);
  std::vector<VisitorVisitRow> visits;
  visits.reserve(rows.size());
  for (const auto& row : rows)
    visits.push_back({.id = row["id"].as<int64_t>(),
                      .cameraId = row["camera_id"].as<int64_t>(),
                      .startedAt = row["started_at"].as<int64_t>(),
                      .lastSeenAt = row["last_seen_at"].as<int64_t>(),
                      .sightings = row["sightings"].as<int64_t>()});
  co_return visits;
}

drogon::Task<std::vector<int64_t>> VisitorRepository::visitTimes(int64_t personId) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(
      std::string(VISIT_TIMES), personId);
  std::vector<int64_t> times;
  times.reserve(rows.size());
  for (const auto& row : rows)
    times.push_back(row["started_at"].as<int64_t>());
  co_return times;
}

drogon::Task<bool> VisitorRepository::update(int64_t id,
                                             const VisitorUpdateInput& input) const
{
  std::string sql(UPDATE_PREFIX);
  std::vector<std::string> values;
  const auto add = [&](std::string_view column, std::string value) {
    if (!values.empty())
      sql += ", ";
    sql += column;
    values.push_back(std::move(value));
  };
  if (input.name) {
    add(UPDATE_COL_NAME, *input.name);
    add(UPDATE_COL_STATUS, input.name->empty() ? "candidate" : "known");
  }
  if (input.category)
    add(UPDATE_COL_CATEGORY, std::string(personCategoryToString(*input.category)));
  if (input.note)
    add(UPDATE_COL_NOTE, *input.note);
  if (values.empty())
    co_return true;
  sql += UPDATE_SUFFIX;
  values.push_back(std::to_string(id));
  const auto& valuesRef = values;
  const auto result = co_await DbService::client()->execSqlCoro(sql, valuesRef);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<VisitorSampleOwner>>
VisitorRepository::sampleOwners(std::span<const int64_t> sampleIds,
                                drogon::orm::DbClient* client) const
{
  std::vector<VisitorSampleOwner> owners;
  if (sampleIds.empty())
    co_return owners;
  const auto pooled = DbService::client();
  const auto rows = co_await clientOr(client, pooled)
                        .execSqlCoro(std::string(SAMPLE_OWNERS_PREFIX) +
                                         idList(sampleIds) + ")",
                                     std::string(kFaceModelId));
  owners.reserve(rows.size());
  for (const auto& row : rows)
    owners.push_back({.id = row["id"].as<int64_t>(),
                      .personId = row["person_id"].as<int64_t>(),
                      .cropKey = row["crop_key"].as<std::string>(),
                      .embedding = floatsOf(row["embedding"].as<std::string>())});
  co_return owners;
}

drogon::Task<void> VisitorRepository::merge(const VisitorMergeInput& input) const
{
  const std::string sources = idList(input.sourceIds);
  co_await input.client->execSqlCoro(
      std::string(MOVE_ALL_SAMPLES_PREFIX) + sources + ")", input.targetId);
  co_await input.client->execSqlCoro(
      std::string(MOVE_VISITS_PREFIX) + sources + ")", input.targetId);
  co_await input.client->execSqlCoro(
      std::string(DELETE_PERSONS_PREFIX) + sources + ")");
  co_await refreshCounts(input.targetId, input.client);
}

drogon::Task<int64_t> VisitorRepository::createVisitor(drogon::orm::DbClient* client) const
{
  const auto number = co_await client->execSqlCoro(std::string(NEXT_VISITOR_NUMBER));
  if (number.empty())
    throw std::runtime_error("the visitor number counter is missing");
  const auto result = co_await client->execSqlCoro(
      std::string(INSERT_VISITOR), number.front()["last_number"].as<int64_t>());
  co_return static_cast<int64_t>(result.insertId());
}

drogon::Task<void> VisitorRepository::split(const VisitorSplitInput& input) const
{
  const std::string ids = idList(input.sampleIds);
  co_await input.client->execSqlCoro(
      std::string(MOVE_SAMPLES_PREFIX) + ids + ")", input.newPersonId);
  co_await input.client->execSqlCoro(
      std::string(VISITS_FROM_SAMPLES_PREFIX) + ids + ")", input.newPersonId);
  co_await refreshCounts(input.newPersonId, input.client);
}

drogon::Task<VisitorRemoved>
VisitorRepository::remove(const VisitorRemoveInput& input) const
{
  VisitorRemoved removed;
  const std::string ids = idList(input.personIds);
  const auto rows = co_await input.client->execSqlCoro(
      std::string(SAMPLE_IDS_OF_PREFIX) + ids + ")");
  for (const auto& row : rows) {
    removed.sampleIds.push_back(row["id"].as<int64_t>());
    auto key = row["crop_key"].as<std::string>();
    if (!key.empty())
      removed.cropKeys.push_back(std::move(key));
  }
  co_await input.client->execSqlCoro(
      std::string(DELETE_PERSONS_PREFIX) + ids + ")");
  co_return removed;
}

drogon::Task<void> VisitorRepository::refreshCounts(int64_t personId,
                                                    drogon::orm::DbClient* client) const
{
  co_await client->execSqlCoro(std::string(REFRESH_COUNTS), personId);
}

drogon::Task<void>
VisitorRepository::deleteSamples(std::span<const int64_t> sampleIds) const
{
  if (sampleIds.empty())
    co_return;
  co_await DbService::client()->execSqlCoro(std::string(DELETE_SAMPLES_PREFIX) +
                                            idList(sampleIds) + ")");
}

drogon::Task<void>
VisitorRepository::setSampleCrop(const VisitorSampleCropInput& input) const
{
  co_await DbService::client()->execSqlCoro(std::string(SET_SAMPLE_CROP),
                                            input.cropKey, input.sampleId);
}

drogon::Task<std::optional<std::string>>
VisitorRepository::sampleCrop(int64_t personId, int64_t sampleId) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(
      std::string(SAMPLE_CROP), sampleId, personId);
  if (rows.empty())
    co_return std::nullopt;
  auto key = rows.front()["crop_key"].as<std::string>();
  if (key.empty())
    co_return std::nullopt;
  co_return key;
}
