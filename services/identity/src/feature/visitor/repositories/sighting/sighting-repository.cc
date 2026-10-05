#include "sighting-repository.hxx"

#include <shared/vocabulary/face-model.hxx>

#include <cstring>
#include <string>

using namespace sighting_query;

namespace
{
std::vector<float> floatsOf(const std::string& blob)
{
  std::vector<float> values(blob.size() / sizeof(float));
  std::memcpy(values.data(), blob.data(), values.size() * sizeof(float));
  return values;
}

std::vector<char> blobOf(const std::vector<float>& values)
{
  std::vector<char> blob(values.size() * sizeof(float));
  std::memcpy(blob.data(), values.data(), blob.size());
  return blob;
}

SightingPerson personOf(const drogon::orm::Row& row)
{
  SightingPerson person;
  person.id = row["id"].as<int64_t>();
  if (!row["user_id"].isNull())
    person.userId = row["user_id"].as<int64_t>();
  person.name = row["name"].as<std::string>();
  person.category = personCategoryFromString(row["category"].as<std::string>())
                        .value_or(PersonCategory::None);
  person.known = row["status"].as<std::string>() == "known";
  person.visitCount = row["visit_count"].as<int64_t>();
  if (!row["visitor_number"].isNull())
    person.visitorNumber = row["visitor_number"].as<int64_t>();
  return person;
}
}

std::unordered_map<int64_t, SightingPerson>
SightingRepository::findPersons(std::span<const int64_t> ids) const
{
  std::unordered_map<int64_t, SightingPerson> persons;
  if (ids.empty())
    return persons;
  std::string sql(FIND_PERSONS_PREFIX);
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (i > 0)
      sql += ',';
    sql += std::to_string(ids[i]);
  }
  sql += ')';
  for (const auto& row : client_.execSqlSync(sql)) {
    auto person = personOf(row);
    persons.emplace(person.id, std::move(person));
  }
  return persons;
}

std::vector<SightingSample> SightingRepository::findSamples(int64_t personId) const
{
  std::vector<SightingSample> samples;
  for (const auto& row : client_.execSqlSync(std::string(FIND_SAMPLES), personId,
                                             std::string(kFaceModelId)))
    samples.push_back({.id = row["id"].as<int64_t>(),
                       .embedding = floatsOf(row["embedding"].as<std::string>()),
                       .quality = row["quality"].as<float>(),
                       .cropKey = row["crop_key"].as<std::string>()});
  return samples;
}

SightingPerson SightingRepository::createVisitor(int64_t at) const
{
  const auto inserted =
      client_.execSqlSync(std::string(INSERT_VISITOR), at, at);
  SightingPerson person;
  person.id = static_cast<int64_t>(inserted.insertId());
  const auto number =
      client_.execSqlSync(std::string(FIND_VISITOR_NUMBER), person.id);
  if (!number.empty() && !number.front()["visitor_number"].isNull())
    person.visitorNumber = number.front()["visitor_number"].as<int64_t>();
  return person;
}

int64_t SightingRepository::insertSample(const SightingSampleInsertInput& input) const
{
  const auto result = client_.execSqlSync(
      std::string(INSERT_SAMPLE), input.personId, blobOf(input.embedding),
      static_cast<double>(input.quality), std::string(kFaceModelId),
      input.cameraId);
  return static_cast<int64_t>(result.insertId());
}

void SightingRepository::deleteSample(int64_t sampleId) const
{
  client_.execSqlSync(std::string(DELETE_SAMPLE), sampleId);
}

void SightingRepository::setSampleCrop(int64_t sampleId,
                                       const std::string& cropKey) const
{
  client_.execSqlSync(std::string(SET_SAMPLE_CROP), cropKey, sampleId);
}

bool SightingRepository::recordVisit(const SightingVisitInput& input) const
{
  const auto open = client_.execSqlSync(std::string(FIND_OPEN_VISIT),
                                        input.personId, input.cameraId,
                                        input.at - input.gapSeconds);
  if (!open.empty()) {
    client_.execSqlSync(std::string(EXTEND_VISIT), input.at,
                        open.front()["id"].as<int64_t>());
    client_.execSqlSync(std::string(TOUCH_PERSON), input.at, input.personId);
    return false;
  }
  client_.execSqlSync(std::string(INSERT_VISIT), input.personId, input.cameraId,
                      input.at, input.at);
  client_.execSqlSync(std::string(COUNT_VISIT), input.at, input.personId);
  return true;
}
