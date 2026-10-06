#include "enrollment-feature-service.hxx"

#include <config/config-service.hxx>
#include <config/identity-config.hxx>
#include <trantor/utils/Logger.h>
#include <ctime>
#include <errors/response-exception.hxx>
#include <feature/invitation/services/invitation-feature-service.hxx>
#include <identity/identity-errors.hxx>
#include <runtime/blocking-task.hxx>
#include <shared/services/face/face-service.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <sync/identity-change-sink.hxx>
#include <sync/socket-emit-dto.hxx>
#include <sync/sync-operation.hxx>
#include <sync/table-name.hxx>
#include <voice/voice-lang.hxx>

namespace
{
constexpr int kIndexAttempts = 3;

EnrollmentResult outcomeResult(EnrollmentOutcome outcome)
{
  return EnrollmentResult{.outcome = outcome,
                          .userId = 0,
                          .personId = 0,
                          .name = "",
                          .lastName = "",
                          .lang = "",
                          .role = UserRole::Unknown};
}

EnrollmentResult registeredResult(const UserSchema& user,
                                  int64_t personId,
                                  std::string lang)
{
  EnrollmentResult result = outcomeResult(EnrollmentOutcome::AlreadyRegistered);
  result.userId = user.id;
  result.personId = personId;
  result.name = user.name;
  result.lastName = user.lastName;
  result.lang = std::move(lang);
  result.role = user.role;
  return result;
}

}

EnrollmentOutcome EnrollmentFeatureService::outcomeOf(FaceCheckStatus status)
{
  switch (status) {
  case FaceCheckStatus::Accepted:
    return EnrollmentOutcome::Enrolled;
  case FaceCheckStatus::SpoofSuspected:
    return EnrollmentOutcome::LivenessFailed;
  case FaceCheckStatus::LivenessUnavailable:
    return EnrollmentOutcome::LivenessUnavailable;
  case FaceCheckStatus::MultipleFaces:
  case FaceCheckStatus::PoorQuality:
    return EnrollmentOutcome::FaceQualityInsufficient;
  case FaceCheckStatus::Unavailable:
  case FaceCheckStatus::Undecodable:
  case FaceCheckStatus::NoFace:
    return EnrollmentOutcome::FaceExtractionFailed;
  }
  return EnrollmentOutcome::FaceExtractionFailed;
}

bool EnrollmentFeatureService::indexFace(const FaceInsertInput& input)
{
  for (int attempt = 0; attempt < kIndexAttempts; ++attempt) {
    if (FaceService::instance().faceDb().insert(input))
      return true;
  }
  return false;
}

drogon::Task<EnrollmentFeatureService::Admission>
EnrollmentFeatureService::admit(const EnrollmentInput& input) const
{
  Admission admission;
  admission.initialOwner = !co_await userRepository_.hasAnyUser();
  if (admission.initialOwner) {
    const std::string pairedDevice =
        ConfigService::getString("pairing.owner_device");
    if (!pairedDevice.empty() && pairedDevice != input.deviceHash)
      admission.refusal = EnrollmentOutcome::NotPaired;
    co_return admission;
  }
  if (input.invitationToken.empty()) {
    admission.refusal = EnrollmentOutcome::InvitationRequired;
    co_return admission;
  }
  admission.invitationHash = InvitationFeatureService::hashToken(input.invitationToken);
  admission.invitation =
      co_await invitationRepository_.findByTokenHash(admission.invitationHash);
  const int64_t now = std::time(nullptr);
  const auto& invitation = admission.invitation;
  if (!invitation || invitation->revokedAt || invitation->expiresAt <= now ||
      invitation->redemptionCount >= invitation->maxRedemptions) {
    admission.refusal = EnrollmentOutcome::InvitationInvalid;
    co_return admission;
  }
  admission.role = invitation->role;
  co_return admission;
}

drogon::Task<EnrollmentResult>
EnrollmentFeatureService::signInRegistered(const MemberMatch& match) const
{
  const auto person = co_await personRepository_.findById(match.personId);
  if (!person || !person->userId)
    co_return outcomeResult(EnrollmentOutcome::FaceNotRecognized);
  const auto user = co_await userRepository_.findById(*person->userId);
  if (!user)
    co_return outcomeResult(EnrollmentOutcome::FaceNotRecognized);
  if (!user->isActive)
    co_return outcomeResult(EnrollmentOutcome::AccountDisabled);
  co_return registeredResult(*user, person->id, user->lang);
}

drogon::Task<EnrollmentResult>
EnrollmentFeatureService::registerUser(const EnrollmentInput& input) const
{
  if (!ConfigService::getBool("pairing.paired"))
    co_return outcomeResult(EnrollmentOutcome::NotPaired);

  const auto admission = co_await admit(input);
  if (admission.refusal)
    co_return outcomeResult(*admission.refusal);

  const IdentityFaceConfig faceConfig = IdentityConfig::resolveFace();
  auto face = co_await FaceService::instance().verifyImageAsync(
      {.imageBytes = input.image,
       .policy = {.quality = face_check::biometricGate(),
                  .livenessRequired = faceConfig.livenessRequired,
                  .livenessThreshold = faceConfig.livenessThreshold,
                  .encodePortrait = true}});
  if (face.status != FaceCheckStatus::Accepted)
    co_return outcomeResult(outcomeOf(face.status));

  if (const auto registered = co_await matcher_.match(
          {.embedding = face.embedding,
           .threshold = FaceDB::matchThreshold(),
           .margin = faceConfig.loginMargin}))
    co_return co_await signInRegistered(*registered);

  VoiceLang lang = voiceLangFromString(input.lang);
  if (lang == VoiceLang::System)
    lang = voiceLangFromString(ConfigService::getString("stt.language"));

  const bool isInitialOwner = admission.initialOwner;
  const auto& invitation = admission.invitation;
  const std::string& invitationHash = admission.invitationHash;
  const UserRole role = admission.role;
  const auto name = input.name.empty()
                        ? (isInitialOwner ? "Administrador" : "Usuario")
                        : input.name;
  const std::string embedding(
      reinterpret_cast<const char*>(face.embedding.data()),
      face.embedding.size() * sizeof(float));
  const int64_t now = std::time(nullptr);
  int64_t userId = 0;
  int64_t personId = 0;
  int64_t faceEmbeddingId = 0;

  auto transaction =
      co_await db_transaction::begin(DbService::identityClient());
  try {
    const int64_t userCount =
        co_await enrollmentRepository_.countUsers(transaction.get());
    if (isInitialOwner && userCount > 0) {
      db_transaction::rollback(transaction);
      co_return outcomeResult(EnrollmentOutcome::OwnerAlreadyExists);
    }
    if (!isInitialOwner) {
      const bool consumed = co_await enrollmentRepository_.consumeInvitation(
          {.tokenHash = invitationHash,
           .now = now,
           .client = transaction.get()});
      if (!consumed) {
        db_transaction::rollback(transaction);
        co_return outcomeResult(EnrollmentOutcome::InvitationInvalid);
      }
    }

    userId = co_await enrollmentRepository_.insertUser(
        {.name = name,
         .lastName = "",
         .role = role,
         .lang = voiceLangToString(lang),
         .client = transaction.get()});
    personId = co_await enrollmentRepository_.insertPerson(
        {.userId = userId, .name = name, .client = transaction.get()});
    faceEmbeddingId = co_await enrollmentRepository_.insertFaceEmbedding(
        {.personId = personId,
         .embedding = embedding,
         .quality = face.confidence,
         .client = transaction.get()});

    if (invitation) {
      co_await enrollmentRepository_.insertRedemption(
          {.invitationId = invitation->id,
           .userId = userId,
           .client = transaction.get()});
    }

    if (const auto* sink = identity_change::getSink()) {
      Json::Value personRow(Json::objectValue);
      personRow["id"] = static_cast<Json::Int64>(personId);
      personRow["user_id"] = static_cast<Json::Int64>(userId);
      personRow["name"] = name;
      personRow["alias"] = "";
      co_await sink->publishCatalog({.table = TableName::Person,
                                     .id = personId,
                                     .deleted = false,
                                     .row = personRow,
                                     .client = transaction.get()});
    }

    UserSchema created;
    created.id = userId;
    created.name = name;
    created.lastName = "";
    created.role = role;
    created.lang = voiceLangToString(lang);
    created.isActive = true;
    created.createdAt = now;

    SocketEmitDto emit;
    emit.operation = SyncOperation::Add;
    emit.option = TableName::User;
    emit.obj = created.toJson();
    if (const auto* sink = identity_change::getSink())
      co_await sink->emitModule(
          {.table = TableName::User, .body = emit, .client = transaction.get()});

    if (invitation) {
      const auto consumedInvitation = co_await invitationRepository_.findById(
          invitation->id, transaction.get());
      if (consumedInvitation) {
        if (const auto* sink = identity_change::getSink()) {
          co_await sink->publishModuleAudit({
              .recordId = consumedInvitation->id,
              .tableName = TableName::UserInvitation,
              .before = invitation->toJson(),
              .after = consumedInvitation->toJson(),
              .actorId = userId,
              .client = transaction.get(),
          });
        }

        Json::Value enrollment(Json::objectValue);
        enrollment["event"] = "invitation_enrollment";
        enrollment["invitationId"] = invitation->id;
        enrollment["userId"] = userId;
        if (const auto* sink = identity_change::getSink()) {
          co_await sink->publishAction(
              {.event = {.userId = userId,
                         .recordId = invitation->id,
                         .tableName = TableName::UserInvitation,
                         .action = UserAction::Create,
                         .oldData = Json::Value(),
                         .newData = enrollment,
                         .ipAddress = ""},
               .client = transaction.get()});
        }
      }
    }

    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(IdentityErrors::ChangeNotRecorded);

    co_await privatePortraitService_.store(userId, face.portraitJpeg);

    const bool indexed = co_await BlockingTask<bool>(
        [embedding = std::move(face.embedding), personId, faceEmbeddingId] {
          return indexFace({.embedding = embedding.data(),
                            .personId = personId,
                            .faceEmbeddingId = faceEmbeddingId});
        });
    if (!indexed) {
      LOG_ERROR << "Enrollment: the new face could not be indexed after "
                << kIndexAttempts << " attempts; the next boot indexes it";
      co_return outcomeResult(EnrollmentOutcome::FaceIndexFailed);
    }

    EnrollmentResult result;
    result.outcome = EnrollmentOutcome::Enrolled;
    result.userId = userId;
    result.personId = personId;
    result.name = name;
    result.lastName = "";
    result.lang = created.lang;
    result.role = role;
    co_return result;
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
}
