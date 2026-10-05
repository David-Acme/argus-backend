#include "biometric-erase-service.hxx"

#include <errors/response-exception.hxx>
#include <identity/identity-errors.hxx>
#include <runtime/blocking-task.hxx>
#include <shared/services/face/face-service.hxx>
#include <shared/services/object-deletion/object-deletion-worker.hxx>
#include <shared/services/privacy/privacy-gate.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <sync/identity-change-sink.hxx>
#include <trantor/utils/Logger.h>

drogon::Task<ResponseBiometricEraseDto>
BiometricEraseService::erase(const BiometricEraseRequest& request) const
{
  if (request.actorRole != UserRole::Owner)
    throw ResponseException(IdentityErrors::BiometricEraseOwnerOnly);

  ErasedBiometrics erased;
  VoiceprintEraseResult voice;
  auto transaction = co_await db_transaction::begin(DbService::identityClient());
  try {
    const auto user =
        co_await userRepository_.findById(request.subjectId, transaction.get());
    if (!user)
      throw ResponseException(404, IdentityErrors::UserNotFound);

    erased = co_await repository_.erase(
        {.userId = user->id, .client = transaction.get()});
    voice = co_await voiceprint_.eraseForConsent({.actorId = request.actorId,
                                                  .subjectId = user->id,
                                                  .reason = kVoiceEraseBiometrics,
                                                  .byOwner = true,
                                                  .client = transaction.get()});
    co_await pendingRepository_.enqueue(
        {.objectKeys = erased.objectKeys, .client = transaction.get()});

    if (const auto* sink = identity_change::getSink()) {
      if (erased.hadPrivacy) {
        Json::Value row = user->toJson();
        row["privacy"] = privacy_policy::toJson(PrivacyState{});
        co_await sink->publishCatalog({.table = TableName::User,
                                       .id = user->id,
                                       .deleted = false,
                                       .row = std::move(row),
                                       .client = transaction.get()});
      }
      Json::Value data(Json::objectValue);
      data["event"] = "biometrics_erase";
      data["faces"] = static_cast<Json::UInt64>(erased.embeddingIds.size());
      data["portraits"] = static_cast<Json::UInt64>(erased.portraits);
      data["voiceProfile"] = voice.removedProfile.has_value();
      data["voiceSamples"] = static_cast<Json::UInt64>(voice.samples);
      data["privacyChoices"] = erased.hadPrivacy;
      data["byOwner"] = true;
      co_await sink->publishAction({.event = {.userId = request.actorId,
                                              .recordId = user->id,
                                              .tableName = TableName::User,
                                              .action = UserAction::Delete,
                                              .oldData = Json::Value(),
                                              .newData = std::move(data),
                                              .ipAddress = ""},
                                    .client = transaction.get()});
    }

    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(IdentityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }

  co_await BlockingTask<void>([ids = erased.embeddingIds] {
    FaceService::instance().faceDb().removeEmbeddings(ids);
  });
  const bool hadVoice = voice.removedProfile.has_value();
  co_await VoiceprintFeatureService::dropFromIndex(voice);
  if (!erased.objectKeys.empty())
    ObjectDeletionWorker::instance().kick();
  LOG_INFO << "Biometrics erased for user " << request.subjectId << " ("
           << erased.embeddingIds.size() << " face embedding(s), "
           << erased.portraits << " portrait object(s))";
  co_return ResponseBiometricEraseDto{.faces = erased.embeddingIds.size(),
                                      .portraits = erased.portraits,
                                      .voiceProfile = hadVoice,
                                      .voiceSamples = voice.samples};
}
