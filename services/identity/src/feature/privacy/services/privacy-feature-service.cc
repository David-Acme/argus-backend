#include "privacy-feature-service.hxx"

#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <identity/identity-errors.hxx>
#include <json/value.h>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <sync/identity-change-sink.hxx>
#include <utility>

namespace
{

Json::Value changedFields(const PrivacyChoices& before, const PrivacyChoices& after)
{
  Json::Value changed(Json::arrayValue);
  if (before.presence != after.presence)
    changed.append("presence");
  if (before.faceCameras != after.faceCameras)
    changed.append("faceCameras");
  if (before.voiceLearning != after.voiceLearning)
    changed.append("voiceLearning");
  if (before.cameraAudio != after.cameraAudio)
    changed.append("cameraAudio");
  return changed;
}

struct JournalInput
{
  int64_t actorId{0};
  int64_t subjectId{0};
  UserAction action{UserAction::Update};
  Json::Value data;
  drogon::orm::DbClient* client{nullptr};
};

drogon::Task<void> journal(JournalInput input)
{
  const auto* sink = identity_change::getSink();
  if (sink == nullptr)
    co_return;
  co_await sink->publishAction({.event = {.userId = input.actorId,
                                          .recordId = input.subjectId,
                                          .tableName = TableName::User,
                                          .action = input.action,
                                          .oldData = Json::Value(),
                                          .newData = std::move(input.data),
                                          .ipAddress = ""},
                                .client = input.client});
}

}

drogon::Task<void> PrivacyFeatureService::publishCatalog(PublishInput input)
{
  const auto* sink = identity_change::getSink();
  if (sink == nullptr)
    co_return;
  Json::Value row = input.user.toJson();
  row["privacy"] = privacy_policy::toJson(input.state);
  co_await sink->publishCatalog({.table = TableName::User,
                                 .id = input.user.id,
                                 .deleted = false,
                                 .row = std::move(row),
                                 .client = input.client});
}

drogon::Task<PrivacyView> PrivacyFeatureService::me(int64_t userId) const
{
  if (!co_await userRepository_.findById(userId))
    throw ResponseException(IdentityErrors::UserNotFound);
  const auto household = co_await repository_.household();
  const auto record = co_await repository_.findUser(userId);
  co_return PrivacyView{.state = privacy_policy::stateOf(record, household),
                        .household = household.allowed};
}

drogon::Task<PrivacyView>
PrivacyFeatureService::decide(const PrivacyDecisionInput& input) const
{
  if (input.noticeVersion != kPrivacyNoticeVersion)
    throw ResponseException(IdentityErrors::PrivacyNoticeOutdated);

  PrivacyView view;
  VoiceprintEraseResult erased;
  auto transaction = co_await db_transaction::begin(DbService::identityClient());
  try {
    const auto user = co_await userRepository_.findById(input.userId,
                                                        transaction.get());
    if (!user || !user->isActive)
      throw ResponseException(IdentityErrors::UserNotFound);
    const auto household = co_await repository_.household(transaction.get());
    const auto before = co_await repository_.findUser(input.userId,
                                                      transaction.get());
    const std::optional<UserPrivacySchema> after =
        co_await repository_.upsertUser({.userId = input.userId,
                                         .noticeVersion = input.noticeVersion,
                                         .choices = input.choices,
                                         .client = transaction.get()});
    view.state = privacy_policy::stateOf(after, household);
    view.household = household.allowed;

    if (!view.state.effective.voiceLearning)
      erased = co_await voiceprint_.eraseForConsent(
          {.actorId = input.userId,
           .subjectId = input.userId,
           .client = transaction.get()});

    co_await publishCatalog(
        {.user = *user, .state = view.state, .client = transaction.get()});

    Json::Value data = input.choices.toJson();
    data["event"] = "privacy_consent";
    data["noticeVersion"] = static_cast<Json::Int64>(input.noticeVersion);
    data["changed"] = changedFields(before ? before->choices : PrivacyChoices{},
                                    input.choices);
    data["first"] = !before.has_value();
    data["byOwner"] = false;
    co_await journal({.actorId = input.userId,
                      .subjectId = input.userId,
                      .action = before ? UserAction::Update : UserAction::Create,
                      .data = std::move(data),
                      .client = transaction.get()});

    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(IdentityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  VoiceprintFeatureService::dropFromIndex(erased);
  LOG_INFO << "Privacy choices recorded for user " << input.userId
           << " (notice v" << input.noticeVersion << ")";
  co_return view;
}

drogon::Task<PrivacyDirectory> PrivacyFeatureService::directory() const
{
  const auto household = co_await repository_.household();
  auto states = co_await gate_.statesByUser();
  PrivacyDirectory directory{.household = household.allowed,
                             .visitorRecognition = household.visitorRecognition,
                             .visitorAcknowledgedAt =
                                 household.visitorAcknowledgedAt,
                             .householdUpdatedAt = household.updatedAt,
                             .users = {}};
  for (const auto& user : co_await userRepository_.findAll()) {
    if (!user.isActive)
      continue;
    const auto found = states.find(user.id);
    directory.users.push_back(
        {.userId = user.id,
         .state = found == states.end() ? PrivacyState{} : found->second});
  }
  co_return directory;
}

drogon::Task<PrivacyDirectory>
PrivacyFeatureService::updateHousehold(const HouseholdPrivacyChange& change) const
{
  std::vector<VoiceprintEraseResult> erased;
  auto transaction = co_await db_transaction::begin(DbService::identityClient());
  try {
    const auto before = co_await repository_.household(transaction.get());
    const auto after = co_await repository_.updateHousehold(
        {.presence = change.presence,
         .faceCameras = change.faceCameras,
         .voiceLearning = change.voiceLearning,
         .cameraAudio = change.cameraAudio,
         .visitorRecognition = change.visitorRecognition,
         .updatedBy = change.actorId,
         .client = transaction.get()});

    if (before.allowed != after.allowed || before.visitorRecognition != after.visitorRecognition) {
      auto states = co_await gate_.statesByUser(transaction.get());
      for (const auto& user : co_await userRepository_.findAll(transaction.get())) {
        const auto found = states.find(user.id);
        const PrivacyState state =
            found == states.end() ? PrivacyState{} : found->second;
        if (!state.effective.voiceLearning)
          erased.push_back(co_await voiceprint_.eraseForConsent(
              {.actorId = change.actorId,
               .subjectId = user.id,
               .client = transaction.get()}));
        co_await publishCatalog(
            {.user = user, .state = state, .client = transaction.get()});
      }

      Json::Value data = after.allowed.toJson();
      data["event"] = "household_privacy";
      data["changed"] = changedFields(before.allowed, after.allowed);
      data["visitorRecognition"] = after.visitorRecognition;
      if (before.visitorRecognition != after.visitorRecognition)
        data["changed"].append("visitorRecognition");
      data["byOwner"] = true;
      co_await journal({.actorId = change.actorId,
                        .subjectId = change.actorId,
                        .action = UserAction::Update,
                        .data = std::move(data),
                        .client = transaction.get()});
    }

    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(IdentityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  for (const auto& entry : erased)
    VoiceprintFeatureService::dropFromIndex(entry);
  co_return co_await directory();
}
