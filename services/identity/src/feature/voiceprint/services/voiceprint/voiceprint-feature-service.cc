#include "voiceprint-feature-service.hxx"

#include <cmath>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <feature/voiceprint/services/embedding/speaker-embedding-service.hxx>
#include <feature/voiceprint/services/embedding/voice-vector.hxx>
#include <feature/voiceprint/services/index/voiceprint-index.hxx>
#include <feature/voiceprint/services/voiceprint/voiceprint-audit.hxx>
#include <identity/identity-errors.hxx>
#include <json/value.h>
#include <memory>
#include <numeric>
#include <runtime/blocking-task.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <utility>

namespace
{

constexpr int kIdentifyCandidates = 2;
constexpr float kProfileNormFloor = 1e-6F;

bool profileUsable(std::span<const float> profile, size_t dims)
{
  if (dims == 0 || profile.size() != dims)
    return false;
  const float norm =
      std::sqrt(std::inner_product(profile.begin(), profile.end(),
                                   profile.begin(), 0.0F));
  return norm > kProfileNormFloor;
}

VoiceprintOutcome outcomeOf(VoiceAnalysisStatus status)
{
  switch (status) {
    case VoiceAnalysisStatus::Ok:
      return VoiceprintOutcome::Ok;
    case VoiceAnalysisStatus::Unavailable:
      return VoiceprintOutcome::Unavailable;
    case VoiceAnalysisStatus::TooShort:
      return VoiceprintOutcome::SampleTooShort;
    case VoiceAnalysisStatus::TooNoisy:
      return VoiceprintOutcome::SampleTooNoisy;
    case VoiceAnalysisStatus::Clipped:
      return VoiceprintOutcome::SampleClipped;
    case VoiceAnalysisStatus::Invalid:
      break;
  }
  return VoiceprintOutcome::SampleInvalid;
}

struct IdentifySearch
{
  VoiceAnalysis analysis;
  std::vector<VoiceprintNearest> nearest;
};

}

VoiceprintFeatureService::VoiceprintFeatureService()
    : VoiceprintFeatureService(IdentityConfig::resolveVoiceprint())
{
}

VoiceprintFeatureService::VoiceprintFeatureService(
    IdentityVoiceprintConfig config)
    : config_(std::move(config))
{
}

std::string
VoiceprintFeatureService::activeModel(const IdentityVoiceprintConfig& config)
{
  return SpeakerEmbeddingService::modelIdOf(config.modelPath);
}

drogon::Task<VoiceprintIdentifyResult>
VoiceprintFeatureService::identify(VoiceprintObserveInput input) const
{
  VoiceprintIdentifyResult result{.outcome = VoiceprintOutcome::Ok,
                                  .matched = false,
                                  .userId = 0,
                                  .score = 0.0F,
                                  .threshold = config_.identifyThreshold,
                                  .personId = std::nullopt,
                                  .name = {},
                                  .role = std::nullopt,
                                  .holderScore = std::nullopt,
                                  .holderProfile = std::nullopt,
                                  .verdict = std::nullopt};
  if (!SpeakerEmbeddingService::instance().isLoaded()) {
    result.outcome = VoiceprintOutcome::Unavailable;
    co_return result;
  }
  if (VoiceprintIndex::instance().size() == 0 ||
      !(co_await privacyGate_.household()).allowed.voiceLearning)
    co_return result;
  auto analysis = std::make_shared<const VoiceAnalysisInput>(
      VoiceAnalysisInput{.voice = std::move(input.sample),
                         .requirement = {.minSpeechSeconds =
                                             config_.minVerifySpeechSeconds,
                                         .minSnrDb = config_.minSnrDb,
                                         .maxClippedRatio =
                                             speech_quality::kMaxClippedRatio},
                         .extractEmbedding = true,
                         .halvesMinSeconds = std::nullopt});
  const auto search = co_await BlockingTask<IdentifySearch>([analysis]() {
    IdentifySearch found;
    found.analysis = SpeakerEmbeddingService::instance().analyze(*analysis);
    if (found.analysis.status == VoiceAnalysisStatus::Ok)
      found.nearest =
          VoiceprintIndex::instance().nearest(found.analysis.embedding,
                                              kIdentifyCandidates);
    return found;
  }, BlockingLane::Heavy);
  result.outcome = outcomeOf(search.analysis.status);
  if (result.outcome != VoiceprintOutcome::Ok || search.nearest.empty())
    co_return result;

  const auto& best = search.nearest.front();
  const float runnerUp =
      search.nearest.size() > 1 ? search.nearest[1].similarity : -1.0F;
  result.score = best.similarity;
  const bool confident = best.similarity >= config_.identifyThreshold &&
                         best.similarity - runnerUp >= config_.identifyMargin;
  if (confident) {
    const auto user = co_await userRepository_.findById(best.userId);
    if (user && user->isActive &&
        (co_await privacyGate_.effectiveFor(best.userId)).voiceLearning) {
      const auto persons = co_await personRepository_.findByUser(best.userId);
      result.matched = true;
      result.userId = best.userId;
      result.name = user->name;
      result.role = user->role;
      if (!persons.empty())
        result.personId = persons.front().id;
    }
  }
  co_await applyHolderVerdict({.embedding = search.analysis.embedding,
                               .holderId = input.holderId,
                               .confidentUserId =
                                   confident ? std::make_optional(best.userId)
                                             : std::nullopt},
                              result);
  co_return result;
}

drogon::Task<void> VoiceprintFeatureService::applyHolderVerdict(
    const VoiceprintHolderCheck& check, VoiceprintIdentifyResult& result) const
{
  if (!check.holderId ||
      !(co_await privacyGate_.effectiveFor(*check.holderId)).voiceLearning)
    co_return;
  const auto profile = co_await profileRepository_.findByUser(*check.holderId);
  const bool profiled =
      profile.has_value() && profile->model == activeModel(config_);
  result.holderProfile = profiled;
  if (!profiled || !profileUsable(profile->embedding, check.embedding.size())) {
    result.verdict = VoiceprintVerdict::Unknown;
    co_return;
  }
  const float holderScore =
      voice_vector::cosine(check.embedding, profile->embedding);
  result.holderScore = holderScore;
  if (check.confidentUserId) {
    result.verdict = *check.confidentUserId == *check.holderId
                         ? VoiceprintVerdict::Holder
                         : (result.matched ? VoiceprintVerdict::OtherKnown
                                           : VoiceprintVerdict::Unknown);
    co_return;
  }
  if (holderScore >= config_.identifyThreshold)
    result.verdict = VoiceprintVerdict::Holder;
  else if (holderScore < config_.unfamiliarCeiling)
    result.verdict = VoiceprintVerdict::Unfamiliar;
  else
    result.verdict = VoiceprintVerdict::Unknown;
  co_return;
}

drogon::Task<VoiceprintDirectory> VoiceprintFeatureService::directory() const
{
  VoiceprintDirectory directory{
      .available = SpeakerEmbeddingService::instance().isLoaded(),
      .recognized = {}};
  const auto profiles =
      co_await profileRepository_.findByModel(activeModel(config_));
  directory.recognized.reserve(profiles.size());
  for (const auto& profile : profiles)
    directory.recognized.push_back({.userId = profile.userId,
                                    .since = profile.linkedAt,
                                    .updatedAt = profile.refreshedAt});
  co_return directory;
}

drogon::Task<VoiceprintForgetResult>
VoiceprintFeatureService::forget(const VoiceprintForgetRequest& request) const
{
  if (!co_await userRepository_.findById(request.subjectId))
    throw ResponseException(IdentityErrors::UserNotFound);

  VoiceprintForgetResult result;
  std::optional<int64_t> removed;
  bool nothingLearned = false;
  auto transaction =
      co_await db_transaction::begin(DbService::identityClient());
  try {
    removed = co_await profileRepository_.removeByUser(request.subjectId,
                                                       transaction.get());
    result.hadProfile = removed.has_value();
    result.samples = co_await sampleRepository_.removeByUser(
        request.subjectId, transaction.get());
    co_await deviceRepository_.removeByUser(request.subjectId,
                                            transaction.get());
    if (!result.hadProfile && result.samples == 0) {
      db_transaction::rollback(transaction);
      nothingLearned = true;
    }
    else {
    Json::Value data(Json::objectValue);
    data["event"] = "voiceprint_forget";
    data["hadProfile"] = result.hadProfile;
    data["samples"] = static_cast<Json::UInt64>(result.samples);
    data["byOwner"] = true;
    co_await voiceprint_audit::publish({.actorId = request.actorId,
                                        .subjectId = request.subjectId,
                                        .action = UserAction::Delete,
                                        .data = std::move(data),
                                        .client = transaction.get()});
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(IdentityErrors::ChangeNotRecorded);
    }
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  if (nothingLearned)
    throw ResponseException(IdentityErrors::VoiceprintNotFound);

  if (removed)
    co_await BlockingTask<void>(
        [profile = *removed] { VoiceprintIndex::instance().remove(profile); });
  LOG_INFO << "Voiceprint forgotten for user " << request.subjectId
           << (result.hadProfile ? " (profile and " : " (")
           << result.samples << " learning sample(s))";
  co_return result;
}

drogon::Task<VoiceprintEraseResult>
VoiceprintFeatureService::eraseForConsent(const VoiceprintEraseInput& input) const
{
  VoiceprintEraseResult erased;
  erased.removedProfile =
      co_await profileRepository_.removeByUser(input.subjectId, input.client);
  erased.samples =
      co_await sampleRepository_.removeByUser(input.subjectId, input.client);
  co_await deviceRepository_.removeByUser(input.subjectId, input.client);
  if (!erased.removedProfile && erased.samples == 0)
    co_return erased;

  Json::Value data(Json::objectValue);
  data["event"] = "voiceprint_forget";
  data["hadProfile"] = erased.removedProfile.has_value();
  data["samples"] = static_cast<Json::UInt64>(erased.samples);
  data["byOwner"] = input.byOwner;
  data["reason"] = std::string(input.reason);
  co_await voiceprint_audit::publish({.actorId = input.actorId,
                                      .subjectId = input.subjectId,
                                      .action = UserAction::Delete,
                                      .data = std::move(data),
                                      .client = input.client});
  co_return erased;
}

drogon::Task<void> VoiceprintFeatureService::dropFromIndex(VoiceprintEraseResult erased)
{
  if (!erased.removedProfile)
    co_return;
  co_await BlockingTask<void>([profile = *erased.removedProfile] {
    VoiceprintIndex::instance().remove(profile);
  });
}
