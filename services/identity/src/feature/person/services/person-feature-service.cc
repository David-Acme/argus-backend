#include "person-feature-service.hxx"

#include <errors/response-exception.hxx>
#include <identity/identity-errors.hxx>
#include <shared/vocabulary/person-status.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <sync/identity-change-sink.hxx>

namespace
{
bool trustedPerson(const PersonSchema& person)
{
  return person.status == PersonStatus::Known &&
         person.category != PersonCategory::Watchlist;
}
}

drogon::Task<bool> PersonFeatureService::touch(int64_t personId, int64_t at) const
{
  const auto person = co_await personRepository_.update(
      personId, {.name = std::nullopt,
                 .alias = std::nullopt,
                 .observation = std::nullopt,
                 .lastSeenAt = at});
  co_return person.id > 0;
}

drogon::Task<int> PersonFeatureService::tag(const PersonTagRequest& request) const
{
  const int added = co_await tagRepository_.addMany(
      {.personId = request.personId, .tags = request.tags, .source = request.source});
  if (!request.observation.empty())
    co_await personRepository_.update(request.personId,
                                      {.name = std::nullopt,
                                       .alias = std::nullopt,
                                       .observation = request.observation,
                                       .lastSeenAt = std::nullopt});
  co_return added;
}

drogon::Task<std::vector<std::string>> PersonFeatureService::tags(int64_t personId) const
{
  co_return co_await tagRepository_.findByPerson(personId);
}

drogon::Task<HouseholdMatch>
PersonFeatureService::describeMatch(const HouseholdMatchRequest& request) const
{
  HouseholdMatch match;
  const auto person = co_await personRepository_.findById(request.personId);
  if (!person)
    co_return match;
  match.trusted = trustedPerson(*person);
  if (!person->userId)
    co_return match;
  const auto user = co_await userRepository_.findById(*person->userId);
  if (user && !user->isActive)
    match.accountDisabled = true;
  if (!user || !user->isActive)
    co_return match;
  match.role = user->role;
  if (!request.forCamera ||
      (co_await privacyGate_.effectiveFor(user->id)).faceCameras) {
    match.userId = user->id;
    match.name = user->name;
    match.lastName = user->lastName;
  }
  co_return match;
}

drogon::Task<std::optional<PersonDescription>>
PersonFeatureService::describe(int64_t personId) const
{
  const auto person = co_await personRepository_.findById(personId);
  if (!person)
    co_return std::nullopt;
  PersonDescription description;
  description.personId = person->id;
  description.named =
      !person->userId ||
      (co_await privacyGate_.effectiveFor(*person->userId)).faceCameras;
  if (person->userId && description.named)
    description.userId = person->userId;
  if (description.named) {
    description.name = person->name;
    description.alias = person->alias;
    description.observation = person->observation;
  }
  description.trusted = trustedPerson(*person);
  if (person->userId) {
    const auto user = co_await userRepository_.findById(*person->userId);
    if (user && user->isActive)
      description.role = user->role;
  }
  else {
    description.visitor = true;
    description.category = person->category;
    description.visits = person->visitCount;
    description.firstSeenAt = person->firstSeenAt;
    description.lastSeenAt = person->lastSeenAt;
    description.visitorNumber = person->visitorNumber;
    const auto times = co_await visitorRepository_.visitTimes(person->id);
    description.pattern = visit_pattern::summarize(times);
  }
  description.tags = co_await tagRepository_.findByPerson(personId);
  co_return description;
}

drogon::Task<std::optional<bool>>
PersonFeatureService::promote(const PersonPromoteRequest& request) const
{
  bool promoted = false;
  auto transaction = co_await db_transaction::begin(DbService::identityClient());
  try {
    const auto before =
        co_await personRepository_.findById(request.personId, transaction.get());
    if (!before) {
      db_transaction::rollback(transaction);
      co_return std::nullopt;
    }
    promoted = co_await personRepository_.promote(request.personId, transaction.get());
    if (promoted) {
      const auto after =
          co_await personRepository_.findById(request.personId, transaction.get());
      const auto* sink = identity_change::getSink();
      if (after && sink != nullptr && after->userId)
        co_await sink->publishModuleAudit({.recordId = request.personId,
                                           .tableName = TableName::Person,
                                           .before = before->toJson(),
                                           .after = after->toJson(),
                                           .actorId = request.actorId,
                                           .client = transaction.get()});
      else if (after && sink != nullptr) {
        Json::Value data(Json::objectValue);
        data["event"] = "visitor_promote";
        co_await sink->publishAction({.event = {.userId = request.actorId,
                                                .recordId = request.personId,
                                                .tableName = TableName::Person,
                                                .action = UserAction::Update,
                                                .oldData = Json::Value(),
                                                .newData = std::move(data),
                                                .ipAddress = ""},
                                      .client = transaction.get()});
      }
    }
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(IdentityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return promoted;
}
