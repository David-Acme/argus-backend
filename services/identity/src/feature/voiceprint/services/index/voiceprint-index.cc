#include "voiceprint-index.hxx"

#include <drogon/drogon.h>
#include <sqlite3.h>
#include <utility>

VoiceprintIndex& VoiceprintIndex::instance()
{
  static VoiceprintIndex index(VecDb::instance());
  return index;
}

bool VoiceprintIndex::init(const VoiceprintIndexInit& input)
{
  const std::scoped_lock lock(vecDb_.mutex());
  sqlite3* db = vecDb_.handle();
  if (db == nullptr || input.dims <= 0) {
    LOG_WARN << "VoiceprintIndex: vector database unavailable";
    return false;
  }
  if (!repository_.ensureVecTable(db, input.dims) || !repository_.clearVec(db))
    return false;
  dims_ = input.dims;
  size_ = 0;
  size_t skipped = 0;
  for (const auto& row : repository_.findForModel(db, input.model)) {
    if (std::cmp_not_equal(row.embedding.size(), input.dims) ||
        !repository_.insertVec(db, {.voiceprintId = row.voiceprintId,
                                    .userId = row.userId,
                                    .embedding = row.embedding})) {
      ++skipped;
      continue;
    }
    ++size_;
  }
  LOG_INFO << "VoiceprintIndex: " << size_.load()
           << " voiceprint(s) indexed for " << input.model
           << (skipped > 0 ? ", " : "")
           << (skipped > 0 ? std::to_string(skipped) + " skipped" : "");
  return true;
}

bool VoiceprintIndex::insert(const VoiceprintIndexInsert& input)
{
  const std::scoped_lock lock(vecDb_.mutex());
  sqlite3* db = vecDb_.handle();
  if (db == nullptr || dims_ <= 0 ||
      std::cmp_not_equal(input.embedding.size(), dims_))
    return false;
  const bool replaced =
      repository_.deleteVec(db, input.voiceprintId) && sqlite3_changes(db) > 0;
  const bool inserted =
      repository_.insertVec(db, {.voiceprintId = input.voiceprintId,
                                 .userId = input.userId,
                                 .embedding = input.embedding});
  if (inserted && !replaced)
    ++size_;
  else
    LOG_WARN << "VoiceprintIndex: could not index voiceprint "
             << input.voiceprintId;
  return inserted;
}

void VoiceprintIndex::remove(int64_t voiceprintId)
{
  const std::scoped_lock lock(vecDb_.mutex());
  sqlite3* db = vecDb_.handle();
  if (db == nullptr)
    return;
  if (repository_.deleteVec(db, voiceprintId) && sqlite3_changes(db) > 0 &&
      size_.load() > 0)
    --size_;
}

std::vector<VoiceprintNearest>
VoiceprintIndex::nearest(std::span<const float> query, int count)
{
  std::vector<VoiceprintNearest> out;
  const std::scoped_lock lock(vecDb_.mutex());
  sqlite3* db = vecDb_.handle();
  if (db == nullptr || dims_ <= 0 || size_.load() == 0 ||
      std::cmp_not_equal(query.size(), dims_))
    return out;
  for (const auto& hit :
       repository_.searchVec(db, {.query = query, .count = count}))
    out.push_back({.voiceprintId = hit.voiceprintId,
                   .userId = hit.userId,
                   .similarity = 1.0F - hit.distance});
  return out;
}

size_t VoiceprintIndex::size() const
{
  return size_.load();
}
