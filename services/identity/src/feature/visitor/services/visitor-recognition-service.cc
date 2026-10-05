#include "visitor-recognition-service.hxx"

#include <feature/visitor/repositories/sighting/sighting-repository.hxx>
#include <array>
#include <runtime/blocking-task.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>

#include <algorithm>
#include <condition_variable>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <drogon/drogon.h>
#include <exception>
#include <numeric>
#include <tuple>
#include <utility>

namespace
{
float cosine(const std::vector<float>& a, const std::vector<float>& b)
{
  if (a.size() != b.size())
    return -1.0F;
  return std::inner_product(a.begin(), a.end(), b.begin(), 0.0F);
}

class CommitWait
{
public:
  void settle(bool committed)
  {
    {
      const std::scoped_lock lock(mutex_);
      committed_ = committed;
    }
    settled_.notify_all();
  }

  [[nodiscard]] bool wait()
  {
    std::unique_lock lock(mutex_);
    settled_.wait(lock, [this] { return committed_.has_value(); });
    return committed_.value_or(false);
  }

private:
  std::mutex mutex_;
  std::condition_variable settled_;
  std::optional<bool> committed_;
};

PersonPool poolOf(const SightingPerson& person)
{
  if (person.userId)
    return PersonPool::Household;
  return person.name.empty() ? PersonPool::Unnamed : PersonPool::Named;
}

}

VisitorRecognitionService::VisitorRecognitionService()
    : thresholds_(visitor_policy::defaults()), strand_(BlockingLane::Light)
{
}

drogon::Task<SightingResult> VisitorRecognitionService::observe(SightingInput input)
{
  const bool enabled = (co_await privacyGate_.household()).visitorRecognition;
  const auto analysis = co_await FaceService::instance().analyzeImageAsync(
      {.imageBytes = std::move(input.image), .encodeFace = enabled});
  if (!analysis)
    co_return SightingResult{};

  const int64_t at = input.observedAt > 0
                         ? input.observedAt
                         : static_cast<int64_t>(std::time(nullptr));
  auto recorded = co_await BlockingTask<Recorded>(
      [this, &analysis, cameraId = input.cameraId, at, enabled] {
        return record({.analysis = *analysis,
                       .cameraId = cameraId,
                       .observedAt = at,
                       .enabled = enabled});
      },
      strand_);

  if (recorded.sampleId > 0 && !analysis->faceJpeg.empty()) {
    const auto key = co_await cropStore_.put(
        {.personId = recorded.result.decision.personId,
         .sampleId = recorded.sampleId,
         .jpeg = analysis->faceJpeg});
    if (key)
      co_await visitorRepository_.setSampleCrop(
          {.sampleId = recorded.sampleId, .cropKey = *key});
  }
  for (const auto& crop : recorded.obsoleteCrops)
    co_await cropStore_.remove(crop);
  if (!recorded.householdEchoes.empty())
    co_await forgetEchoes(std::move(recorded.householdEchoes));
  co_return recorded.result;
}

VisitorRecognitionService::Recorded
VisitorRecognitionService::record(const RecordInput& input) const
{
  Recorded recorded;
  SightingResult& result = recorded.result;
  result.faceFound = true;
  result.quality = face_quality::judge(input.analysis.quality,
                                       thresholds_.learnGate);
  auto& faceDb = FaceService::instance().faceDb();
  const auto neighbours = faceDb.nearest(
      {.query = input.analysis.embedding.data(), .topK = kNeighbours});

  const auto client = DbService::client();
  const SightingRepository lookup(*client);
  std::vector<int64_t> ids;
  ids.reserve(neighbours.size());
  for (const auto& neighbour : neighbours)
    ids.push_back(neighbour.personId);
  const auto persons = lookup.findPersons(ids);

  std::vector<PoolNeighbour> pooled;
  pooled.reserve(neighbours.size());
  for (const auto& neighbour : neighbours) {
    const auto found = persons.find(neighbour.personId);
    if (found == persons.end())
      continue;
    pooled.push_back({.personId = neighbour.personId,
                      .score = neighbour.score,
                      .pool = poolOf(found->second)});
  }

  result.decision = visitor_policy::decide({.neighbours = pooled,
                                            .quality = input.analysis.quality,
                                            .recognitionEnabled = input.enabled,
                                            .thresholds = thresholds_});
  const SightingOutcome outcome = result.decision.outcome;
  if (outcome == SightingOutcome::Household) {
    const auto& person = persons.at(result.decision.personId);
    result.userId = person.userId;
    result.known = true;
    result.named = true;
    for (const auto& neighbour : pooled)
      if (neighbour.pool == PersonPool::Unnamed &&
          neighbour.score >= thresholds_.visitorMatch)
        recorded.householdEchoes.push_back(neighbour.personId);
    return recorded;
  }
  if (outcome != SightingOutcome::Visitor &&
      outcome != SightingOutcome::NewVisitor)
    return recorded;

  struct IndexChange
  {
    int64_t added{0};
    int64_t removed{0};
  } index;

  SightingPerson person;
  const auto commit = std::make_shared<CommitWait>();
  {
  const auto transaction = client->newTransaction(
      [commit](bool committed) { commit->settle(committed); },
      drogon::orm::TransactionType::Immediate);
  const SightingRepository repository(*transaction);
  if (outcome == SightingOutcome::NewVisitor) {
    person = repository.createVisitor(input.observedAt);
    result.created = true;
    result.decision.personId = person.id;
  }
  else {
    person = persons.at(result.decision.personId);
    const std::array<int64_t, 1> matched{person.id};
    if (!repository.findPersons(matched).contains(person.id))
      return recorded;
  }

  if (result.decision.learn) {
    const auto samples = result.created
                             ? std::vector<SightingSample>{}
                             : repository.findSamples(person.id);
    std::vector<float> similarities;
    std::vector<float> qualities;
    similarities.reserve(samples.size());
    qualities.reserve(samples.size());
    for (const auto& sample : samples) {
      similarities.push_back(cosine(sample.embedding, input.analysis.embedding));
      qualities.push_back(sample.quality);
    }
    const float quality = face_quality::score(input.analysis.quality);
    const auto admission = visitor_policy::admit({.similarities = similarities,
                                                  .existingQualities = qualities,
                                                  .quality = quality,
                                                  .thresholds = thresholds_});
    if (admission.kind == SampleAdmissionKind::Replace) {
      const auto& worst = samples.at(admission.replaceIndex);
      repository.deleteSample(worst.id);
      index.removed = worst.id;
      if (!worst.cropKey.empty())
        recorded.obsoleteCrops.push_back(worst.cropKey);
    }
    if (admission.kind == SampleAdmissionKind::Add ||
        admission.kind == SampleAdmissionKind::Replace) {
      index.added = repository.insertSample({.personId = person.id,
                                             .embedding = input.analysis.embedding,
                                             .quality = quality,
                                             .cameraId = input.cameraId});
      recorded.sampleId = index.added;
    }
  }

  result.newVisit = repository.recordVisit({.personId = person.id,
                                            .cameraId = input.cameraId,
                                            .at = input.observedAt,
                                            .gapSeconds = kVisitGapSeconds});
  result.visits = person.visitCount + (result.newVisit ? 1 : 0);
  result.visitorNumber = person.visitorNumber;
  result.category = person.category;
  result.named = !person.name.empty();
  result.known = person.known;

  }

  if (!commit->wait()) {
    LOG_WARN << "Visitor recognition: a sighting did not commit; the index and "
                "the stored crops are left untouched";
    Recorded discarded;
    discarded.result.faceFound = true;
    discarded.result.quality = result.quality;
    return discarded;
  }

  if (index.removed > 0) {
    const std::array<int64_t, 1> removed{index.removed};
    faceDb.removeEmbeddings(removed);
  }
  if (index.added > 0)
    std::ignore = faceDb.insert({.embedding = input.analysis.embedding.data(),
                   .personId = person.id,
                   .faceEmbeddingId = index.added});
  return recorded;
}

drogon::Task<void> VisitorRecognitionService::forgetEchoes(std::vector<int64_t> personIds)
{
  VisitorRemoved removed;
  auto transaction = co_await db_transaction::begin(DbService::identityClient());
  try {
    removed = co_await visitorRepository_.remove(
        {.personIds = personIds, .client = transaction.get()});
    if (!co_await db_transaction::Commit(std::move(transaction)))
      co_return;
  }
  catch (const std::exception& error) {
    db_transaction::rollback(transaction);
    LOG_WARN << "Visitor recognition: could not forget a household echo: "
             << error.what();
    co_return;
  }
  co_await BlockingTask<void>([ids = std::move(removed.sampleIds)] {
    FaceService::instance().faceDb().removeEmbeddings(ids);
  });
  for (const auto& crop : removed.cropKeys)
    co_await cropStore_.remove(crop);
  LOG_INFO << "Visitor recognition: forgot " << personIds.size()
           << " unnamed visitor(s) that turned out to be a household member";
}
