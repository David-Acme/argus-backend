#include "passive-enrollment-service.hxx"

#include <ctime>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <feature/voiceprint/services/embedding/voice-vector.hxx>
#include <feature/voiceprint/services/index/voiceprint-index.hxx>
#include <feature/voiceprint/services/voiceprint/voiceprint-audit.hxx>
#include <feature/voiceprint/services/voiceprint/voiceprint-feature-service.hxx>
#include <identity/identity-errors.hxx>
#include <json/value.h>
#include <runtime/blocking-task.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <unordered_set>
#include <utility>

namespace
{

constexpr int kHouseholdCandidates = 16;

int64_t utcOffsetAt(int64_t now)
{
  const auto seconds = static_cast<std::time_t>(now);
  std::tm local{};
  if (localtime_r(&seconds, &local) == nullptr)
    return 0;
  return local.tm_gmtoff;
}

struct LinkAuditInput
{
  const LinkDecision& decision;
  const std::string& model;
  bool relink{false};
};

Json::Value linkData(const LinkAuditInput& input)
{
  Json::Value data(Json::objectValue);
  data["event"] = input.relink ? "voiceprint_relink" : "voiceprint_link";
  data["source"] = "passive";
  data["automatic"] = true;
  data["occasions"] = input.decision.occasions;
  data["days"] = input.decision.days;
  data["samples"] = static_cast<Json::UInt64>(input.decision.members.size());
  data["model"] = input.model;
  return data;
}

}

const char* passiveCallOutcomeName(PassiveCallOutcome outcome)
{
  switch (outcome) {
    case PassiveCallOutcome::Unavailable:
      return "unavailable";
    case PassiveCallOutcome::NotFound:
      return "not_found";
    case PassiveCallOutcome::Tainted:
      return "tainted";
    case PassiveCallOutcome::Unusable:
      return "unusable";
    case PassiveCallOutcome::Inactive:
      return "inactive";
    case PassiveCallOutcome::OtherVoice:
      return "other_voice";
    case PassiveCallOutcome::Pending:
      return "pending";
    case PassiveCallOutcome::Adopted:
      return "adopted";
    case PassiveCallOutcome::Refreshed:
      return "refreshed";
    case PassiveCallOutcome::Linked:
      return "linked";
    case PassiveCallOutcome::Relinked:
      return "relinked";
    case PassiveCallOutcome::NotConsented:
      return "not_consented";
  }
  return "unknown";
}

PassiveEnrollmentService::PassiveEnrollmentService()
    : PassiveEnrollmentService(IdentityConfig::resolveVoiceprint())
{
}

PassiveEnrollmentService::PassiveEnrollmentService(
    IdentityVoiceprintConfig config)
    : config_(std::move(config)), policy_(config_.passive),
      tracker_(std::make_shared<VoiceCallTracker>(config_.passive))
{
}

PassiveEnrollmentService::Scores
PassiveEnrollmentService::scoresFor(std::span<const float> embedding,
                                    int64_t userId)
{
  Scores scores;
  for (const auto& hit :
       VoiceprintIndex::instance().nearest(embedding, kHouseholdCandidates)) {
    if (hit.userId == userId) {
      if (!scores.own)
        scores.own = hit.similarity;
      continue;
    }
    if (!scores.bestOther)
      scores.bestOther =
          OtherVoiceMatch{.userId = hit.userId, .score = hit.similarity};
  }
  return scores;
}

drogon::Task<VoiceTurnLearning>
PassiveEnrollmentService::learnFromTurn(VoiceTurnInput input) const
{
  VoiceTurnLearning learning;
  const PassiveVoiceConfig& passive = config_.passive;
  if (!passive.enabled || input.userId <= 0 || input.deviceHash.empty() ||
      input.callKey.empty() || !SpeakerEmbeddingService::instance().isLoaded())
    co_return learning;
  if (!(co_await privacyGate_.effectiveFor(input.userId)).voiceLearning)
    co_return learning;
  learning.considered = true;

  struct TurnAnalysis
  {
    VoiceAnalysis analysis;
    Scores scores;
  };
  auto request = std::make_shared<const VoiceAnalysisInput>(VoiceAnalysisInput{
      .voice = std::move(input.sample),
      .requirement = {.minSpeechSeconds = passive.minTurnSpeechSeconds,
                      .minSnrDb = passive.minTurnSnrDb,
                      .maxClippedRatio = passive.maxTurnClippedRatio},
      .extractEmbedding = true,
      .halvesMinSeconds = passive.minHalfSeconds});
  const int64_t userId = input.userId;
  auto turn = co_await BlockingTask<TurnAnalysis>([request, userId]() {
    TurnAnalysis result;
    result.analysis = SpeakerEmbeddingService::instance().analyze(*request);
    if (result.analysis.status == VoiceAnalysisStatus::Ok)
      result.scores = scoresFor(result.analysis.embedding, userId);
    return result;
  }, BlockingLane::Heavy);
  learning.quality = turn.analysis.status;
  learning.speechSeconds = turn.analysis.quality.speechSeconds;
  if (turn.analysis.status != VoiceAnalysisStatus::Ok)
    co_return learning;

  learning.halvesScore = turn.analysis.halvesScore;
  learning.ownScore = turn.scores.own;
  learning.bestOther = turn.scores.bestOther;
  learning.verdict = tracker_->observe(
      {.callKey = std::move(input.callKey),
       .userId = input.userId,
       .deviceHash = std::move(input.deviceHash),
       .embedding = std::move(turn.analysis.embedding),
       .speechSeconds = turn.analysis.quality.speechSeconds,
       .halvesScore = turn.analysis.halvesScore,
       .bestOther = turn.scores.bestOther,
       .ownScore = turn.scores.own,
       .now = input.now});
  co_return learning;
}

drogon::Task<PassiveCallOutcome>
PassiveEnrollmentService::closeCall(const VoiceCallCloseInput& input) const
{
  auto call = tracker_->close(input.callKey);
  if (!call)
    co_return PassiveCallOutcome::NotFound;
  co_return co_await recordCall(*call, input.now);
}

drogon::Task<std::vector<PassiveCallOutcome>>
PassiveEnrollmentService::expireCalls(int64_t now) const
{
  std::vector<PassiveCallOutcome> outcomes;
  for (const auto& call : tracker_->expire(now))
    outcomes.push_back(co_await recordCall(call, now));
  co_return outcomes;
}

drogon::Task<void> PassiveEnrollmentService::purgeExpired(int64_t now) const
{
  co_await sampleRepository_.purgeBefore(
      now - config_.passive.windowSeconds,
      now - config_.passive.adoptedRetentionSeconds);
}

drogon::Task<PassiveCallOutcome>
PassiveEnrollmentService::recordCall(const ClosedCall& call, int64_t now) const
{
  const PassiveVoiceConfig& passive = config_.passive;
  if (!passive.enabled || !SpeakerEmbeddingService::instance().isLoaded())
    co_return PassiveCallOutcome::Unavailable;
  if (!(co_await privacyGate_.effectiveFor(call.userId)).voiceLearning)
    co_return PassiveCallOutcome::NotConsented;

  const auto judgement = policy_.judgeCall({.turns = call.turns,
                                            .speechSeconds = call.speechSeconds,
                                            .tainted = call.tainted});
  if (judgement.verdict != CallVerdict::Usable) {
    co_await deviceRepository_.record({.deviceHash = call.deviceHash,
                                       .userId = call.userId,
                                       .matched = 0,
                                       .conflicting = 0,
                                       .mixed = call.tainted ? 1 : 0,
                                       .now = now});
    LOG_DEBUG << "Voiceprint: a call of user " << call.userId
              << " teaches nothing (" << static_cast<int>(judgement.verdict)
              << ")";
    co_return judgement.verdict == CallVerdict::Tainted
        ? PassiveCallOutcome::Tainted
        : PassiveCallOutcome::Unusable;
  }

  const auto user = co_await userRepository_.findById(call.userId);
  if (!user || !user->isActive)
    co_return PassiveCallOutcome::Inactive;

  const std::string model = VoiceprintFeatureService::activeModel(config_);
  auto profile = co_await profileRepository_.findByUser(call.userId);
  if (profile && profile->model != model)
    profile.reset();

  auto centroid =
      std::make_shared<const std::vector<float>>(judgement.centroid);
  const int64_t userId = call.userId;
  const Scores scores = co_await BlockingTask<Scores>(
      [centroid, userId]() { return scoresFor(*centroid, userId); });
  const std::optional<float> own =
      profile ? std::optional<float>(
                    voice_vector::cosine(*centroid, profile->embedding))
              : std::nullopt;

  if (scores.bestOther &&
      scores.bestOther->score >= passive.otherSpeakerThreshold &&
      (!own || scores.bestOther->score >= *own)) {
    co_await deviceRepository_.record({.deviceHash = call.deviceHash,
                                       .userId = call.userId,
                                       .matched = 0,
                                       .conflicting = profile ? 1 : 0,
                                       .mixed = profile ? 0 : 1,
                                       .now = now});
    LOG_INFO << "Voiceprint: a call of user " << call.userId
             << " sounds like another enrolled person; nothing learned";
    co_return PassiveCallOutcome::OtherVoice;
  }

  const bool matched = own && *own >= passive.otherSpeakerThreshold;
  const auto device = co_await deviceRepository_.record(
      {.deviceHash = call.deviceHash,
       .userId = call.userId,
       .matched = matched ? 1 : 0,
       .conflicting = (profile && !matched) ? 1 : 0,
       .mixed = 0,
       .now = now});
  const bool shared =
      co_await deviceRepository_.otherUsersOn(call.deviceHash, call.userId) > 0;

  VoiceSampleState state = VoiceSampleState::Pending;
  if (profile && own) {
    const auto adoption =
        policy_.judgeAdoption({.ownScore = *own,
                               .bestOther = scores.bestOther,
                               .sharedDevice = shared,
                               .deviceMatched = device.matched,
                               .deviceConflicting = device.conflicting});
    if (adoption == AdoptVerdict::Adopt)
      state = VoiceSampleState::Adopted;
  }

  co_await sampleRepository_.create(
      {.userId = call.userId,
       .model = model,
       .deviceHash = call.deviceHash,
       .embedding = voice_vector::toBlob(*centroid),
       .turns = static_cast<int>(call.turns.size()),
       .speechSeconds = call.speechSeconds,
       .state = state,
       .createdAt = now,
       .client = nullptr});
  co_await sampleRepository_.prune(
      {.userId = call.userId,
       .model = model,
       .maxPending = passive.maxPendingSamples,
       .maxAdopted = passive.maxProfileSamples,
       .pendingBefore = now - passive.windowSeconds,
       .adoptedBefore = now - passive.adoptedRetentionSeconds});

  const CallContext context{
      .call = call, .centroid = *centroid, .model = model, .now = now};
  if (state == VoiceSampleState::Adopted && profile)
    co_return co_await refreshProfile(context, *profile);
  co_return co_await evaluateLink(context, profile);
}

drogon::Task<PassiveCallOutcome>
PassiveEnrollmentService::refreshProfile(const CallContext& context,
                                         const VoiceProfileSchema& profile) const
{
  const PassiveVoiceConfig& passive = config_.passive;
  const int fresh = co_await sampleRepository_.countAdoptedAfter(
      {.userId = profile.userId,
       .model = context.model,
       .after = profile.refreshedAt});
  if (fresh < passive.refreshBatch)
    co_return PassiveCallOutcome::Adopted;

  const auto reservoir = co_await sampleRepository_.findAdopted(
      {.userId = profile.userId,
       .model = context.model,
       .limit = passive.maxProfileSamples});
  std::vector<std::vector<float>> embeddings;
  double speechSeconds = 0.0;
  embeddings.reserve(reservoir.size());
  for (const auto& sample : reservoir) {
    embeddings.push_back(sample.embedding);
    speechSeconds += sample.speechSeconds;
  }
  const RefreshResult refreshed =
      policy_.refresh({.current = profile.embedding, .reservoir = embeddings});
  if (!refreshed.applied)
    co_return PassiveCallOutcome::Adopted;

  std::optional<VoiceProfileSchema> updated;
  auto transaction =
      co_await db_transaction::begin(DbService::identityClient());
  try {
    updated = co_await profileRepository_.refresh(
        {.id = profile.id,
         .embedding = voice_vector::toBlob(refreshed.centroid),
         .sampleCount = std::max(1, refreshed.kept),
         .speechSeconds = std::max(speechSeconds, profile.speechSeconds),
         .refreshedAt = context.now,
         .client = transaction.get()});
    Json::Value data(Json::objectValue);
    data["event"] = "voiceprint_refresh";
    data["automatic"] = true;
    data["samples"] = refreshed.kept;
    data["outliers"] = refreshed.dropped;
    data["model"] = context.model;
    co_await voiceprint_audit::publish({.actorId = profile.userId,
                                        .subjectId = profile.userId,
                                        .action = UserAction::Update,
                                        .data = std::move(data),
                                        .client = transaction.get()});
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(IdentityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  if (!updated)
    co_return PassiveCallOutcome::Adopted;
  VoiceprintIndex::instance().insert({.voiceprintId = updated->id,
                                      .userId = updated->userId,
                                      .embedding = updated->embedding});
  LOG_INFO << "Voiceprint: refreshed user " << profile.userId << " from "
           << refreshed.kept << " sample(s), " << refreshed.dropped
           << " outlier(s) left out";
  co_return PassiveCallOutcome::Refreshed;
}

drogon::Task<PassiveCallOutcome> PassiveEnrollmentService::evaluateLink(
    const CallContext& context,
    const std::optional<VoiceProfileSchema>& profile) const
{
  const PassiveVoiceConfig& passive = config_.passive;
  const int64_t userId = context.call.userId;
  const auto window = co_await sampleRepository_.findSince(
      {.userId = userId,
       .model = context.model,
       .since = context.now - passive.windowSeconds});
  const auto shared = co_await deviceRepository_.sharedFor(userId);
  std::vector<PolicySample> samples;
  samples.reserve(window.size());
  for (const auto& sample : window)
    samples.push_back({.id = sample.id,
                       .embedding = sample.embedding,
                       .createdAt = sample.createdAt,
                       .speechSeconds = static_cast<float>(sample.speechSeconds),
                       .ownDevice = !shared.contains(sample.deviceHash)});
  std::vector<std::vector<float>> others;
  for (const auto& other : co_await profileRepository_.findByModel(context.model))
    if (other.userId != userId)
      others.push_back(other.embedding);

  const LinkDecision decision =
      policy_.evaluateLink({.samples = samples,
                            .otherProfiles = others,
                            .utcOffsetSeconds = utcOffsetAt(context.now)});
  if (decision.verdict != LinkVerdict::Link) {
    LOG_DEBUG << "Voiceprint: user " << userId << " not linked yet ("
              << static_cast<int>(decision.verdict) << ", "
              << decision.occasions << " occasion(s), " << decision.days
              << " day(s), dominance " << decision.dominance << ")";
    co_return PassiveCallOutcome::Pending;
  }
  if (profile && voice_vector::cosine(decision.centroid, profile->embedding) >=
                     passive.otherSpeakerThreshold)
    co_return PassiveCallOutcome::Pending;

  const bool relink = profile.has_value();
  std::optional<VoiceProfileSchema> linked;
  auto transaction =
      co_await db_transaction::begin(DbService::identityClient());
  try {
    linked = co_await profileRepository_.upsert(
        {.userId = userId,
         .model = context.model,
         .embedding = voice_vector::toBlob(decision.centroid),
         .sampleCount = static_cast<int>(decision.members.size()),
         .speechSeconds = decision.speechSeconds,
         .source = VoiceProfileSource::Passive,
         .linkedAt = context.now,
         .refreshedAt = context.now,
         .client = transaction.get()});
    const VoiceSampleAdoptInput members{.userId = userId,
                                        .ids = decision.members,
                                        .client = transaction.get()};
    co_await sampleRepository_.dropAdoptedExcept(members);
    co_await sampleRepository_.adopt(members);
    co_await voiceprint_audit::publish(
        {.actorId = userId,
         .subjectId = userId,
         .action = relink ? UserAction::Update : UserAction::Create,
         .data = linkData({.decision = decision,
                           .model = context.model,
                           .relink = relink}),
         .client = transaction.get()});
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(IdentityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  VoiceprintIndex::instance().insert({.voiceprintId = linked->id,
                                      .userId = linked->userId,
                                      .embedding = linked->embedding});
  LOG_INFO << "Voiceprint: " << (relink ? "relinked" : "linked") << " user "
           << userId << " after " << decision.occasions << " occasion(s) on "
           << decision.days << " day(s), " << decision.members.size()
           << " call(s)";
  co_return relink ? PassiveCallOutcome::Relinked : PassiveCallOutcome::Linked;
}
