#include "voiceprint-feature-service.hxx"

#include <algorithm>
#include <ctime>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <feature/voiceprint/services/embedding/voice-vector.hxx>
#include <feature/voiceprint/services/index/voiceprint-index.hxx>
#include <feature/voiceprint/services/voiceprint/voiceprint-phrases.hxx>
#include <identity/identity-errors.hxx>
#include <json/value.h>
#include <numeric>
#include <runtime/blocking-task.hxx>
#include <shared/services/face/face-service.hxx>
#include <shared/services/token/opaque-token.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <sync/identity-change-sink.hxx>
#include <text/json-util.hxx>
#include <utility>

namespace
{

constexpr size_t kMaxSamples = 10;
constexpr int kDuplicateCandidates = 3;
constexpr int kIdentifyCandidates = 2;

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

std::string activeModel(const IdentityVoiceprintConfig& config)
{
  return SpeakerEmbeddingService::modelIdOf(config.modelPath);
}

std::string phrasesJson(const std::vector<std::string>& phrases)
{
  Json::Value array(Json::arrayValue);
  for (const auto& phrase : phrases)
    array.append(phrase);
  return json_util::toString(array);
}

struct ChallengeMatchInput
{
  const VoiceprintChallengeSchema& challenge;
  const VoiceprintActor& actor;
  int64_t subjectId{0};
  int64_t now{0};
};

bool challengeMatches(const ChallengeMatchInput& input)
{
  const auto& challenge = input.challenge;
  return challenge.userId == input.subjectId &&
         challenge.requesterId == input.actor.userId &&
         challenge.deviceHash == input.actor.deviceHash &&
         !challenge.consumedAt.has_value() && challenge.expiresAt > input.now;
}

struct AuditInput
{
  int64_t actorId{0};
  int64_t subjectId{0};
  UserAction action{UserAction::Create};
  Json::Value data;
  drogon::orm::DbClient* client{nullptr};
};

drogon::Task<void> audit(const AuditInput& input)
{
  const auto* sink = identity_change::getSink();
  if (sink == nullptr)
    co_return;
  const ActionPublishInput publish{.event = {.userId = input.actorId,
                                             .recordId = input.subjectId,
                                             .tableName = TableName::User,
                                             .action = input.action,
                                             .oldData = Json::Value(),
                                             .newData = input.data,
                                             .ipAddress = ""},
                                   .client = input.client};
  co_await sink->publishAction(publish);
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

std::string VoiceprintFeatureService::hashToken(const std::string& token)
{
  auto hash = opaque_token::sha256Hex(token);
  if (!hash)
    throw ResponseException(IdentityErrors::ChangeNotRecorded);
  return std::move(*hash);
}

VoiceprintStatusView VoiceprintFeatureService::viewOf(
    const std::optional<VoiceprintSchema>& voiceprint) const
{
  VoiceprintStatusView view{.available =
                                SpeakerEmbeddingService::instance().isLoaded(),
                            .enrolled = voiceprint.has_value(),
                            .stale = false,
                            .model = activeModel(config_),
                            .sampleCount = 0,
                            .enrolledAt = 0,
                            .method = VoiceprintMethod::Self,
                            .consentVersion =
                                std::string(kVoiceprintConsentVersion),
                            .samplesRequired = config_.samplesRequired,
                            .minSpeechSeconds = config_.minSpeechSeconds};
  if (!voiceprint)
    return view;
  view.stale = voiceprint->model != view.model;
  view.model = voiceprint->model;
  view.sampleCount = voiceprint->sampleCount;
  view.enrolledAt = voiceprint->createdAt;
  view.method = voiceprint->method;
  view.consentVersion = voiceprint->consentVersion;
  return view;
}

drogon::Task<VoiceprintStatusResult>
VoiceprintFeatureService::status(int64_t userId) const
{
  const auto user = co_await userRepository_.findById(userId);
  if (!user)
    co_return VoiceprintStatusResult{.outcome = VoiceprintOutcome::UserNotFound,
                                     .status = viewOf(std::nullopt)};
  const auto voiceprint = co_await voiceprintRepository_.findByUser(userId);
  co_return VoiceprintStatusResult{.outcome = VoiceprintOutcome::Ok,
                                   .status = viewOf(voiceprint)};
}

drogon::Task<VoiceprintFeatureService::SubjectCheck>
VoiceprintFeatureService::manageableSubject(const SubjectAccess& access) const
{
  if (access.subjectId <= 0)
    co_return SubjectCheck{.outcome = VoiceprintOutcome::UserNotFound,
                           .subject = std::nullopt};
  if (access.actor.userId != access.subjectId &&
      access.actor.role != UserRole::Owner)
    co_return SubjectCheck{.outcome = VoiceprintOutcome::Forbidden,
                           .subject = std::nullopt};
  auto subject = co_await userRepository_.findById(access.subjectId);
  if (!subject || (access.requireActive && !subject->isActive))
    co_return SubjectCheck{.outcome = VoiceprintOutcome::UserNotFound,
                           .subject = std::nullopt};
  co_return SubjectCheck{.outcome = VoiceprintOutcome::Ok,
                         .subject = std::move(subject)};
}

drogon::Task<VoiceprintChallengeResult>
VoiceprintFeatureService::createChallenge(
    const VoiceprintChallengeRequest& request) const
{
  const SubjectAccess access{.actor = request.actor,
                             .subjectId = request.subjectId,
                             .requireActive = true};
  const auto check = co_await manageableSubject(access);
  if (check.outcome != VoiceprintOutcome::Ok || !check.subject)
    co_return VoiceprintChallengeResult{.outcome = check.outcome,
                                        .challenge = {}};
  if (!SpeakerEmbeddingService::instance().isLoaded())
    co_return VoiceprintChallengeResult{.outcome =
                                            VoiceprintOutcome::Unavailable,
                                        .challenge = {}};
  const auto existing =
      co_await voiceprintRepository_.findByUser(request.subjectId);
  if (existing && existing->model == activeModel(config_))
    co_return VoiceprintChallengeResult{.outcome =
                                            VoiceprintOutcome::AlreadyEnrolled,
                                        .challenge = {}};

  VoiceLang lang = voiceLangFromString(check.subject->lang);
  if (request.lang && *request.lang != VoiceLang::System)
    lang = *request.lang;
  if (lang == VoiceLang::System)
    lang = VoiceLang::Es;

  auto token = opaque_token::mint();
  if (!token)
    throw ResponseException(IdentityErrors::ChangeNotRecorded);
  const int64_t now = std::time(nullptr);
  auto phrases =
      voiceprint_phrases::pick(lang,
                               static_cast<size_t>(config_.samplesRequired));
  co_await challengeRepository_.purgeExpired(now);
  const VoiceprintChallengeCreateInput
      create{.tokenHash = hashToken(*token),
             .userId = request.subjectId,
             .requesterId = request.actor.userId,
             .deviceHash = request.actor.deviceHash,
             .lang = lang,
             .phrases = phrasesJson(phrases),
             .expiresAt = now + config_.challengeSeconds};
  const auto challenge = co_await challengeRepository_.create(create);
  co_return VoiceprintChallengeResult{.outcome = VoiceprintOutcome::Ok,
                                      .challenge =
                                          {.challengeId = std::move(*token),
                                           .phrases = std::move(phrases),
                                           .expiresAt = challenge.expiresAt,
                                           .lang = lang}};
}

drogon::Task<VoiceprintSampleCheck>
VoiceprintFeatureService::checkSample(EncodedVoice sample) const
{
  VoiceAnalysisInput input{.voice = std::move(sample),
                           .requirement = {.minSpeechSeconds =
                                               config_.minSpeechSeconds,
                                           .minSnrDb = config_.minSnrDb},
                           .extractEmbedding = false};
  const auto analysis =
      co_await SpeakerEmbeddingService::instance().analyzeAsync(
          std::move(input));
  co_return VoiceprintSampleCheck{.outcome = outcomeOf(analysis.status),
                                  .speechSeconds =
                                      analysis.quality.speechSeconds,
                                  .snrDb = analysis.quality.snrDb,
                                  .minSpeechSeconds = config_.minSpeechSeconds,
                                  .minSnrDb = config_.minSnrDb,
                                  .collected = 0,
                                  .required = config_.samplesRequired};
}

drogon::Task<std::optional<VoiceprintChallengeSchema>>
VoiceprintFeatureService::usableChallenge(const ChallengeLookup& lookup) const
{
  if (lookup.challengeId.empty())
    co_return std::nullopt;
  auto challenge = co_await challengeRepository_.findByTokenHash(
      hashToken(lookup.challengeId));
  if (!challenge || !challengeMatches({.challenge = *challenge,
                                       .actor = lookup.actor,
                                       .subjectId = lookup.subjectId,
                                       .now = std::time(nullptr)}))
    co_return std::nullopt;
  co_return challenge;
}

drogon::Task<bool>
VoiceprintFeatureService::faceBelongsTo(const EnrollmentGate& gate) const
{
  if (gate.faceImage.empty())
    co_return false;
  const auto personId =
      co_await FaceService::instance().identifyAsync(gate.faceImage);
  if (!personId)
    co_return false;
  const auto person = co_await personRepository_.findById(*personId);
  co_return person && person->userId && *person->userId == gate.subjectId;
}

drogon::Task<VoiceprintOutcome>
VoiceprintFeatureService::admit(const EnrollmentGate& gate) const
{
  const SubjectAccess access{.actor = gate.actor,
                             .subjectId = gate.subjectId,
                             .requireActive = true};
  const auto check = co_await manageableSubject(access);
  if (check.outcome != VoiceprintOutcome::Ok)
    co_return check.outcome;
  if (!gate.consent || gate.consentVersion != kVoiceprintConsentVersion)
    co_return VoiceprintOutcome::ConsentRequired;
  if (!SpeakerEmbeddingService::instance().isLoaded())
    co_return VoiceprintOutcome::Unavailable;
  const auto existing =
      co_await voiceprintRepository_.findByUser(gate.subjectId);
  if (existing && existing->model == activeModel(config_))
    co_return VoiceprintOutcome::AlreadyEnrolled;
  const ChallengeLookup lookup{.actor = gate.actor,
                               .subjectId = gate.subjectId,
                               .challengeId = gate.challengeId};
  if (!co_await usableChallenge(lookup))
    co_return VoiceprintOutcome::ChallengeInvalid;
  if (gate.actor.userId != gate.subjectId && !co_await faceBelongsTo(gate))
    co_return VoiceprintOutcome::FaceNotVerified;
  co_return VoiceprintOutcome::Ok;
}

drogon::Task<VoiceprintSampleCheck>
VoiceprintFeatureService::stageSample(VoiceprintStageRequest request) const
{
  VoiceprintSampleCheck result{.outcome = VoiceprintOutcome::Ok,
                               .speechSeconds = 0.0F,
                               .snrDb = 0.0F,
                               .minSpeechSeconds = config_.minSpeechSeconds,
                               .minSnrDb = config_.minSnrDb,
                               .collected = 0,
                               .required = config_.samplesRequired};
  const SubjectAccess access{.actor = request.actor,
                             .subjectId = request.subjectId,
                             .requireActive = true};
  const auto check = co_await manageableSubject(access);
  if (check.outcome != VoiceprintOutcome::Ok) {
    result.outcome = check.outcome;
    co_return result;
  }
  if (!SpeakerEmbeddingService::instance().isLoaded()) {
    result.outcome = VoiceprintOutcome::Unavailable;
    co_return result;
  }
  if (request.position < 0 || request.position >= config_.samplesRequired) {
    result.outcome = VoiceprintOutcome::SampleCountInvalid;
    co_return result;
  }
  const ChallengeLookup lookup{.actor = request.actor,
                               .subjectId = request.subjectId,
                               .challengeId = request.challengeId};
  const auto challenge = co_await usableChallenge(lookup);
  if (!challenge) {
    result.outcome = VoiceprintOutcome::ChallengeInvalid;
    co_return result;
  }

  VoiceAnalysisInput input{.voice = std::move(request.sample),
                           .requirement = {.minSpeechSeconds =
                                               config_.minSpeechSeconds,
                                           .minSnrDb = config_.minSnrDb},
                           .extractEmbedding = true};
  auto analysis = co_await SpeakerEmbeddingService::instance().analyzeAsync(
      std::move(input));
  result.outcome = outcomeOf(analysis.status);
  result.speechSeconds = analysis.quality.speechSeconds;
  result.snrDb = analysis.quality.snrDb;
  if (result.outcome == VoiceprintOutcome::Ok) {
    const VoiceprintStageSampleInput stage{.challengeId = challenge->id,
                                           .position = request.position,
                                           .embedding = voice_vector::toBlob(
                                               analysis.embedding),
                                           .speechSeconds =
                                               analysis.quality.speechSeconds};
    co_await challengeRepository_.stageSample(stage);
  }
  const auto staged = co_await challengeRepository_.findSamples(challenge->id);
  result.collected = static_cast<int>(staged.size());
  co_return result;
}

drogon::Task<VoiceprintEnrollResult> VoiceprintFeatureService::finalize(
    const VoiceprintFinalizeRequest& request) const
{
  const EnrollmentGate gate{.actor = request.actor,
                            .subjectId = request.subjectId,
                            .consent = request.consent,
                            .consentVersion = request.consentVersion,
                            .challengeId = request.challengeId,
                            .faceImage = request.faceImage};
  const auto admitted = co_await admit(gate);
  if (admitted != VoiceprintOutcome::Ok)
    co_return VoiceprintEnrollResult{.outcome = admitted,
                                     .status = {},
                                     .failedSample = std::nullopt};

  const ChallengeLookup lookup{.actor = request.actor,
                               .subjectId = request.subjectId,
                               .challengeId = request.challengeId};
  const auto challenge = co_await usableChallenge(lookup);
  if (!challenge)
    co_return VoiceprintEnrollResult{.outcome =
                                         VoiceprintOutcome::ChallengeInvalid,
                                     .status = {},
                                     .failedSample = std::nullopt};
  const auto staged = co_await challengeRepository_.findSamples(challenge->id);
  if (std::cmp_less(staged.size(), config_.samplesRequired))
    co_return VoiceprintEnrollResult{.outcome =
                                         VoiceprintOutcome::SampleCountInvalid,
                                     .status = {},
                                     .failedSample = std::nullopt};

  std::vector<std::vector<float>> embeddings;
  std::vector<int> positions;
  double speechSeconds = 0.0;
  embeddings.reserve(staged.size());
  positions.reserve(staged.size());
  for (const auto& sample : staged) {
    embeddings.push_back(sample.embedding);
    positions.push_back(sample.position);
    speechSeconds += sample.speechSeconds;
  }
  const CommitInput commit{.actor = request.actor,
                           .subjectId = request.subjectId,
                           .embeddings = embeddings,
                           .positions = positions,
                           .speechSeconds = speechSeconds,
                           .consentVersion = request.consentVersion,
                           .challengeId = request.challengeId};
  co_return co_await commitEnrollment(commit);
}

drogon::Task<VoiceprintFeatureService::AnalyzedSamples>
VoiceprintFeatureService::analyzeSamples(
    std::vector<EncodedVoice> samples) const
{
  AnalyzedSamples analyzed;
  analyzed.embeddings.reserve(samples.size());
  for (size_t index = 0; index < samples.size(); ++index) {
    VoiceAnalysisInput input{.voice = std::move(samples[index]),
                             .requirement = {.minSpeechSeconds =
                                                 config_.minSpeechSeconds,
                                             .minSnrDb = config_.minSnrDb},
                             .extractEmbedding = true};
    auto analysis = co_await SpeakerEmbeddingService::instance().analyzeAsync(
        std::move(input));
    if (analysis.status != VoiceAnalysisStatus::Ok) {
      analyzed.outcome = outcomeOf(analysis.status);
      analyzed.failedSample = static_cast<int>(index);
      co_return analyzed;
    }
    analyzed.speechSeconds += analysis.quality.speechSeconds;
    analyzed.embeddings.push_back(std::move(analysis.embedding));
  }
  co_return analyzed;
}

std::optional<int> VoiceprintFeatureService::inconsistentSample(
    const std::vector<std::vector<float>>& embeddings) const
{
  std::optional<int> worst;
  float worstScore = config_.consistencyThreshold;
  for (size_t index = 0; index < embeddings.size(); ++index) {
    std::vector<std::vector<float>> others;
    others.reserve(embeddings.size() - 1);
    for (size_t other = 0; other < embeddings.size(); ++other)
      if (other != index)
        others.push_back(embeddings[other]);
    const float score =
        voice_vector::cosine(embeddings[index], voice_vector::centroid(others));
    if (score < worstScore) {
      worstScore = score;
      worst = static_cast<int>(index);
    }
  }
  return worst;
}

bool VoiceprintFeatureService::voiceTakenByOther(
    const std::vector<float>& centroid, int64_t subjectId) const
{
  const auto hits =
      VoiceprintIndex::instance().nearest(centroid, kDuplicateCandidates);
  return std::ranges::any_of(hits, [&](const VoiceprintNearest& hit) {
    return hit.userId != subjectId &&
           hit.similarity >= config_.identifyThreshold;
  });
}

drogon::Task<VoiceprintEnrollResult>
VoiceprintFeatureService::enroll(VoiceprintEnrollRequest request) const
{
  const EnrollmentGate gate{.actor = request.actor,
                            .subjectId = request.subjectId,
                            .consent = request.consent,
                            .consentVersion = request.consentVersion,
                            .challengeId = request.challengeId,
                            .faceImage = request.faceImage};
  const auto admitted = co_await admit(gate);
  if (admitted == VoiceprintOutcome::AlreadyEnrolled) {
    const auto existing =
        co_await voiceprintRepository_.findByUser(request.subjectId);
    co_return VoiceprintEnrollResult{.outcome = admitted,
                                     .status = viewOf(existing),
                                     .failedSample = std::nullopt};
  }
  if (admitted != VoiceprintOutcome::Ok)
    co_return VoiceprintEnrollResult{.outcome = admitted,
                                     .status = {},
                                     .failedSample = std::nullopt};
  if (std::cmp_less(request.samples.size(), config_.samplesRequired) ||
      request.samples.size() > kMaxSamples)
    co_return VoiceprintEnrollResult{.outcome =
                                         VoiceprintOutcome::SampleCountInvalid,
                                     .status = {},
                                     .failedSample = std::nullopt};

  const auto analyzed = co_await analyzeSamples(std::move(request.samples));
  if (analyzed.outcome != VoiceprintOutcome::Ok)
    co_return VoiceprintEnrollResult{.outcome = analyzed.outcome,
                                     .status = {},
                                     .failedSample = analyzed.failedSample};
  std::vector<int> positions(analyzed.embeddings.size());
  std::iota(positions.begin(), positions.end(), 0);
  const CommitInput commit{.actor = request.actor,
                           .subjectId = request.subjectId,
                           .embeddings = analyzed.embeddings,
                           .positions = positions,
                           .speechSeconds = analyzed.speechSeconds,
                           .consentVersion = request.consentVersion,
                           .challengeId = request.challengeId};
  co_return co_await commitEnrollment(commit);
}

drogon::Task<VoiceprintEnrollResult>
VoiceprintFeatureService::commitEnrollment(const CommitInput& input) const
{
  const auto refuse = [](VoiceprintOutcome outcome) {
    return VoiceprintEnrollResult{.outcome = outcome,
                                  .status = {},
                                  .failedSample = std::nullopt};
  };

  if (const auto odd = inconsistentSample(input.embeddings)) {
    const auto index = static_cast<size_t>(*odd);
    co_return VoiceprintEnrollResult{.outcome =
                                         VoiceprintOutcome::SamplesInconsistent,
                                     .status = {},
                                     .failedSample =
                                         index < input.positions.size()
                                             ? input.positions[index]
                                             : *odd};
  }

  auto centroid = std::make_shared<const std::vector<float>>(
      voice_vector::centroid(input.embeddings));
  if (centroid->empty())
    co_return refuse(VoiceprintOutcome::SampleInvalid);
  const int64_t subjectId = input.subjectId;
  const bool taken = co_await BlockingTask<bool>([this, centroid, subjectId]() {
    return voiceTakenByOther(*centroid, subjectId);
  });
  if (taken)
    co_return refuse(VoiceprintOutcome::VoiceTaken);

  const std::string model = activeModel(config_);
  const VoiceprintMethod method = input.actor.userId != subjectId
                                      ? VoiceprintMethod::OwnerFace
                                      : VoiceprintMethod::Self;
  const size_t sampleCount = input.embeddings.size();
  std::optional<VoiceprintSchema> created;
  std::optional<int64_t> replaced;
  auto transaction =
      co_await db_transaction::begin(DbService::identityClient());
  try {
    const int64_t now = std::time(nullptr);
    const auto challenge =
        co_await challengeRepository_.findByTokenHash(hashToken(
                                                          input.challengeId),
                                                      transaction.get());
    if (!challenge || !challengeMatches({.challenge = *challenge,
                                         .actor = input.actor,
                                         .subjectId = subjectId,
                                         .now = now})) {
      db_transaction::rollback(transaction);
      co_return refuse(VoiceprintOutcome::ChallengeInvalid);
    }
    const VoiceprintChallengeConsumeInput consume{.id = challenge->id,
                                                  .now = now,
                                                  .client = transaction.get()};
    if (!co_await challengeRepository_.tryConsume(consume)) {
      db_transaction::rollback(transaction);
      co_return refuse(VoiceprintOutcome::ChallengeInvalid);
    }
    co_await challengeRepository_.deleteSamples(challenge->id,
                                                transaction.get());

    replaced = co_await voiceprintRepository_.removeByUser(subjectId,
                                                           transaction.get());
    const VoiceprintCreateInput create{.userId = subjectId,
                                       .model = model,
                                       .embedding =
                                           voice_vector::toBlob(*centroid),
                                       .sampleCount =
                                           static_cast<int>(sampleCount),
                                       .speechSeconds = input.speechSeconds,
                                       .method = method,
                                       .consentVersion = input.consentVersion,
                                       .enrolledBy = input.actor.userId,
                                       .client = transaction.get()};
    created = co_await voiceprintRepository_.create(create);

    Json::Value data(Json::objectValue);
    data["event"] = "voiceprint_enroll";
    data["method"] = voiceprintMethodToString(method);
    data["samples"] = static_cast<Json::UInt64>(sampleCount);
    data["model"] = model;
    data["consentVersion"] = input.consentVersion;
    data["replaced"] = replaced.has_value();
    const AuditInput auditInput{.actorId = input.actor.userId,
                                .subjectId = subjectId,
                                .action = UserAction::Create,
                                .data = std::move(data),
                                .client = transaction.get()};
    co_await audit(auditInput);

    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(IdentityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }

  auto& index = VoiceprintIndex::instance();
  if (replaced)
    index.remove(*replaced);
  if (created)
    index.insert({.voiceprintId = created->id,
                  .userId = created->userId,
                  .embedding = created->embedding});
  LOG_INFO << "Voiceprint enrolled for user " << subjectId << " ("
           << voiceprintMethodToString(method) << ", " << sampleCount
           << " samples, " << model << ")";
  co_return VoiceprintEnrollResult{.outcome = VoiceprintOutcome::Ok,
                                   .status = viewOf(created),
                                   .failedSample = std::nullopt};
}

drogon::Task<VoiceprintVerifyResult>
VoiceprintFeatureService::verify(VoiceprintVerifyRequest request) const
{
  VoiceprintVerifyResult result{.outcome = VoiceprintOutcome::Ok,
                                .matched = false,
                                .score = 0.0F,
                                .threshold = config_.verifyThreshold};
  const auto user = co_await userRepository_.findById(request.userId);
  if (!user || !user->isActive) {
    result.outcome = VoiceprintOutcome::UserNotFound;
    co_return result;
  }
  const auto voiceprint =
      co_await voiceprintRepository_.findByUser(request.userId);
  if (!voiceprint) {
    result.outcome = VoiceprintOutcome::NotEnrolled;
    co_return result;
  }
  if (voiceprint->model != activeModel(config_)) {
    result.outcome = VoiceprintOutcome::Stale;
    co_return result;
  }
  VoiceAnalysisInput input{.voice = std::move(request.sample),
                           .requirement = {.minSpeechSeconds =
                                               config_.minVerifySpeechSeconds,
                                           .minSnrDb = config_.minSnrDb},
                           .extractEmbedding = true};
  const auto analysis =
      co_await SpeakerEmbeddingService::instance().analyzeAsync(
          std::move(input));
  result.outcome = outcomeOf(analysis.status);
  if (result.outcome != VoiceprintOutcome::Ok)
    co_return result;
  result.score =
      voice_vector::cosine(voiceprint->embedding, analysis.embedding);
  result.matched = result.score >= config_.verifyThreshold;
  co_return result;
}

drogon::Task<VoiceprintIdentifyResult>
VoiceprintFeatureService::identify(EncodedVoice sample) const
{
  VoiceprintIdentifyResult result{.outcome = VoiceprintOutcome::Ok,
                                  .matched = false,
                                  .userId = 0,
                                  .score = 0.0F,
                                  .threshold = config_.identifyThreshold,
                                  .personId = std::nullopt,
                                  .name = {},
                                  .role = std::nullopt};
  if (!SpeakerEmbeddingService::instance().isLoaded()) {
    result.outcome = VoiceprintOutcome::Unavailable;
    co_return result;
  }
  auto input = std::make_shared<const VoiceAnalysisInput>(
      VoiceAnalysisInput{.voice = std::move(sample),
                         .requirement = {.minSpeechSeconds =
                                             config_.minVerifySpeechSeconds,
                                         .minSnrDb = config_.minSnrDb},
                         .extractEmbedding = true});
  const auto search = co_await BlockingTask<IdentifySearch>([input]() {
    IdentifySearch found;
    found.analysis = SpeakerEmbeddingService::instance().analyze(*input);
    if (found.analysis.status == VoiceAnalysisStatus::Ok)
      found.nearest =
          VoiceprintIndex::instance().nearest(found.analysis.embedding,
                                              kIdentifyCandidates);
    return found;
  });
  result.outcome = outcomeOf(search.analysis.status);
  if (result.outcome != VoiceprintOutcome::Ok || search.nearest.empty())
    co_return result;

  const auto& best = search.nearest.front();
  const float runnerUp =
      search.nearest.size() > 1 ? search.nearest[1].similarity : -1.0F;
  result.score = best.similarity;
  if (best.similarity < config_.identifyThreshold ||
      best.similarity - runnerUp < config_.identifyMargin)
    co_return result;

  const auto user = co_await userRepository_.findById(best.userId);
  if (!user || !user->isActive)
    co_return result;
  const auto persons = co_await personRepository_.findByUser(best.userId);
  result.matched = true;
  result.userId = best.userId;
  result.name = user->name;
  result.role = user->role;
  if (!persons.empty())
    result.personId = persons.front().id;
  co_return result;
}

drogon::Task<VoiceprintDeleteResult>
VoiceprintFeatureService::remove(const VoiceprintDeleteRequest& request) const
{
  const SubjectAccess access{.actor = request.actor,
                             .subjectId = request.subjectId,
                             .requireActive = false};
  const auto check = co_await manageableSubject(access);
  if (check.outcome != VoiceprintOutcome::Ok)
    co_return VoiceprintDeleteResult{.outcome = check.outcome,
                                     .deleted = false};

  std::optional<int64_t> removed;
  auto transaction =
      co_await db_transaction::begin(DbService::identityClient());
  try {
    removed = co_await voiceprintRepository_.removeByUser(request.subjectId,
                                                          transaction.get());
    if (!removed) {
      db_transaction::rollback(transaction);
      co_return VoiceprintDeleteResult{.outcome =
                                           VoiceprintOutcome::NotEnrolled,
                                       .deleted = false};
    }
    Json::Value data(Json::objectValue);
    data["event"] = "voiceprint_delete";
    data["byOwner"] = request.actor.userId != request.subjectId;
    const AuditInput auditInput{.actorId = request.actor.userId,
                                .subjectId = request.subjectId,
                                .action = UserAction::Delete,
                                .data = std::move(data),
                                .client = transaction.get()};
    co_await audit(auditInput);
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(IdentityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }

  if (removed)
    VoiceprintIndex::instance().remove(*removed);
  LOG_INFO << "Voiceprint deleted for user " << request.subjectId;
  co_return VoiceprintDeleteResult{.outcome = VoiceprintOutcome::Ok,
                                   .deleted = true};
}
