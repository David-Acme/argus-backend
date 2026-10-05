#include "visitor-feature-service.hxx"

#include <feature/visitor/services/visit-pattern.hxx>
#include <errors/response-exception.hxx>
#include <identity/identity-errors.hxx>
#include <runtime/blocking-task.hxx>
#include <shared/services/face/face-service.hxx>
#include <shared/services/token/opaque-token.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <sync/identity-change-sink.hxx>

#include <algorithm>
#include <array>
#include <tuple>
#include <ctime>
#include <openssl/evp.h>
#include <ranges>
#include <stdexcept>

namespace
{
constexpr int64_t kVisitsInDetail = 200;

int64_t now()
{
  return static_cast<int64_t>(std::time(nullptr));
}

std::string base64(const std::string& input)
{
  std::string output(((input.size() + 2) / 3) * 4, '\0');
  const auto written = EVP_EncodeBlock(
      reinterpret_cast<unsigned char*>(output.data()),
      reinterpret_cast<const unsigned char*>(input.data()),
      static_cast<int>(input.size()));
  if (written < 0)
    throw ResponseException(IdentityErrors::VisitorCropUnavailable);
  output.resize(static_cast<std::size_t>(written));
  return output;
}

struct IndexMove
{
  std::vector<int64_t> ids;
  std::vector<std::vector<float>> embeddings;
  int64_t personId{0};
};

drogon::Task<void> reindex(IndexMove move)
{
  co_await BlockingTask<void>([move = std::move(move)] {
    auto& faceDb = FaceService::instance().faceDb();
    faceDb.removeEmbeddings(move.ids);
    for (std::size_t i = 0; i < move.ids.size(); ++i)
      std::ignore = faceDb.insert({.embedding = move.embeddings[i].data(),
                                   .personId = move.personId,
                                   .faceEmbeddingId = move.ids[i]});
  });
}

drogon::Task<void> unindex(std::vector<int64_t> ids)
{
  co_await BlockingTask<void>([ids = std::move(ids)] {
    FaceService::instance().faceDb().removeEmbeddings(ids);
  });
}
}

void VisitorFeatureService::requireOwner(const VisitorRequester& requester)
{
  if (requester.role != UserRole::Owner)
    throw ResponseException(IdentityErrors::VisitorNotFound);
}

drogon::Task<VisitorRow>
VisitorFeatureService::requireVisible(const VisitorRequest& request) const
{
  auto visitor = co_await repository_.find(request.personId);
  if (!visitor)
    throw ResponseException(IdentityErrors::VisitorNotFound);
  if (request.requester.role != UserRole::Owner && visitor->name.empty())
    throw ResponseException(IdentityErrors::VisitorNotFound);
  co_return std::move(*visitor);
}

drogon::Task<ResponseVisitorDetailDto>
VisitorFeatureService::detailOf(int64_t personId) const
{
  auto visitor = co_await repository_.find(personId);
  if (!visitor)
    throw ResponseException(IdentityErrors::VisitorNotFound);
  ResponseVisitorDetailDto detail;
  detail.visitor = std::move(*visitor);
  detail.samples = co_await repository_.samples(personId);
  for (auto& sample : detail.samples)
    sample.embedding.clear();
  detail.visits = co_await repository_.visits(personId, kVisitsInDetail);
  const auto times = co_await repository_.visitTimes(personId);
  detail.pattern = visit_pattern::summarize(times);
  co_return detail;
}

drogon::Task<void> VisitorFeatureService::journal(const JournalInput& input) const
{
  const auto* sink = identity_change::getSink();
  if (sink == nullptr)
    co_return;
  Json::Value data = input.details;
  data["event"] = input.event;
  co_await sink->publishAction({.event = {.userId = input.actorId,
                                          .recordId = input.personId,
                                          .tableName = TableName::Person,
                                          .action = UserAction::Update,
                                          .oldData = Json::Value(),
                                          .newData = data,
                                          .ipAddress = ""},
                                .client = nullptr});
}

drogon::Task<ResponseVisitorListDto>
VisitorFeatureService::list(const VisitorListRequest& request) const
{
  const bool namedOnly =
      request.namedOnly || request.requester.role != UserRole::Owner;
  ResponseVisitorListDto response;
  response.visitors =
      co_await repository_.list({.namedOnly = namedOnly, .limit = 500});
  response.recognitionEnabled =
      (co_await privacyGate_.household()).visitorRecognition;
  co_return response;
}

drogon::Task<ResponseVisitorDetailDto>
VisitorFeatureService::detail(const VisitorRequest& request) const
{
  co_await requireVisible(request);
  auto detail = co_await detailOf(request.personId);
  if (request.requester.role != UserRole::Owner)
    detail.visits.clear();
  co_return detail;
}

drogon::Task<ResponseVisitorDetailDto>
VisitorFeatureService::update(const VisitorUpdateRequest& request) const
{
  requireOwner(request.requester);
  const auto before = co_await requireVisible(
      {.requester = request.requester, .personId = request.personId});
  co_await repository_.update(request.personId,
                              {.name = request.body.name,
                               .category = request.body.category,
                               .note = request.body.note});
  Json::Value details(Json::objectValue);
  details["named"] = request.body.name ? !request.body.name->empty()
                                       : !before.name.empty();
  if (request.body.category)
    details["category"] = std::string(personCategoryToString(*request.body.category));
  co_await journal({.actorId = request.requester.userId,
                    .personId = request.personId,
                    .event = "visitor_update",
                    .details = details});
  co_return co_await detailOf(request.personId);
}

drogon::Task<void> VisitorFeatureService::trimSamples(int64_t personId) const
{
  auto samples = co_await repository_.samples(personId);
  if (samples.size() <= kMaxSamplesAfterMerge)
    co_return;
  std::vector<int64_t> ids;
  std::vector<std::string> crops;
  for (const auto& sample : samples | std::views::drop(kMaxSamplesAfterMerge))
    ids.push_back(sample.id);
  const auto owners = co_await repository_.sampleOwners(ids, nullptr);
  for (const auto& owner : owners)
    if (!owner.cropKey.empty())
      crops.push_back(owner.cropKey);
  co_await repository_.deleteSamples(ids);
  co_await unindex(ids);
  for (const auto& crop : crops)
    co_await cropStore_.remove(crop);
}

drogon::Task<ResponseVisitorDetailDto>
VisitorFeatureService::merge(const VisitorMergeRequest& request) const
{
  requireOwner(request.requester);
  if (std::ranges::find(request.body.sourceIds, request.personId) !=
      request.body.sourceIds.end())
    throw ResponseException(IdentityErrors::VisitorMergeInvalid);
  co_await requireVisible(
      {.requester = request.requester, .personId = request.personId});
  IndexMove move{.ids = {}, .embeddings = {}, .personId = request.personId};
  for (const int64_t source : request.body.sourceIds) {
    if (!co_await repository_.find(source))
      throw ResponseException(IdentityErrors::VisitorMergeInvalid);
    for (auto& sample : co_await repository_.samples(source)) {
      move.ids.push_back(sample.id);
      move.embeddings.push_back(std::move(sample.embedding));
    }
  }

  auto transaction = co_await db_transaction::begin(DbService::identityClient());
  try {
    co_await repository_.merge({.targetId = request.personId,
                                .sourceIds = request.body.sourceIds,
                                .client = transaction.get()});
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(IdentityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_await reindex(std::move(move));
  co_await trimSamples(request.personId);
  Json::Value details(Json::objectValue);
  details["merged"] = static_cast<Json::UInt64>(request.body.sourceIds.size());
  co_await journal({.actorId = request.requester.userId,
                    .personId = request.personId,
                    .event = "visitor_merge",
                    .details = details});
  co_return co_await detailOf(request.personId);
}

drogon::Task<ResponseVisitorDetailDto>
VisitorFeatureService::split(const VisitorSplitRequest& request) const
{
  requireOwner(request.requester);
  co_await requireVisible(
      {.requester = request.requester, .personId = request.personId});
  const auto all = co_await repository_.samples(request.personId);
  const auto owners =
      co_await repository_.sampleOwners(request.body.sampleIds, nullptr);
  const bool foreign = std::ranges::any_of(owners, [&](const auto& owner) {
    return owner.personId != request.personId;
  });
  if (foreign || owners.size() != request.body.sampleIds.size() ||
      owners.size() >= all.size())
    throw ResponseException(IdentityErrors::VisitorSplitInvalid);

  int64_t created = 0;
  auto transaction = co_await db_transaction::begin(DbService::identityClient());
  try {
    created = co_await repository_.createVisitor(transaction.get());
    co_await repository_.split({.newPersonId = created,
                                .sampleIds = request.body.sampleIds,
                                .client = transaction.get()});
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(IdentityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  IndexMove move{.ids = {}, .embeddings = {}, .personId = created};
  for (const auto& owner : owners) {
    move.ids.push_back(owner.id);
    move.embeddings.push_back(owner.embedding);
  }
  co_await reindex(std::move(move));
  Json::Value details(Json::objectValue);
  details["samples"] = static_cast<Json::UInt64>(owners.size());
  details["into"] = static_cast<Json::Int64>(created);
  co_await journal({.actorId = request.requester.userId,
                    .personId = request.personId,
                    .event = "visitor_split",
                    .details = details});
  co_return co_await detailOf(created);
}

drogon::Task<void> VisitorFeatureService::remove(const VisitorRequest& request) const
{
  requireOwner(request.requester);
  co_await requireVisible(request);
  VisitorRemoved removed;
  auto transaction = co_await db_transaction::begin(DbService::identityClient());
  try {
    removed = co_await repository_.remove(
        {.personIds = {request.personId}, .client = transaction.get()});
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(IdentityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_await unindex(removed.sampleIds);
  for (const auto& crop : removed.cropKeys)
    co_await cropStore_.remove(crop);
  co_await journal({.actorId = request.requester.userId,
                    .personId = request.personId,
                    .event = "visitor_delete",
                    .details = Json::Value(Json::objectValue)});
}

drogon::Task<void>
VisitorFeatureService::removeSample(const VisitorSampleRequest& request) const
{
  requireOwner(request.requester);
  co_await requireVisible(
      {.requester = request.requester, .personId = request.personId});
  const std::array<int64_t, 1> ids{request.sampleId};
  const auto owners = co_await repository_.sampleOwners(ids, nullptr);
  if (owners.empty() || owners.front().personId != request.personId)
    throw ResponseException(IdentityErrors::VisitorSampleNotFound);
  co_await repository_.deleteSamples(ids);
  co_await unindex({request.sampleId});
  co_await cropStore_.remove(owners.front().cropKey);
}

drogon::Task<ResponseVisitorCropCapabilityDto>
VisitorFeatureService::mintCrop(const VisitorSampleRequest& request) const
{
  const auto visitor = co_await requireVisible(
      {.requester = request.requester, .personId = request.personId});
  const int64_t sampleId =
      request.sampleId > 0 ? request.sampleId : visitor.coverSampleId.value_or(0);
  if (sampleId <= 0 || !co_await repository_.sampleCrop(request.personId, sampleId))
    throw ResponseException(IdentityErrors::VisitorCropUnavailable);
  auto token = opaque_token::mint();
  const auto hash = token ? opaque_token::sha256Hex(*token) : std::nullopt;
  if (!token || !hash)
    throw ResponseException(IdentityErrors::VisitorCropUnavailable);
  const int64_t expiresAt = now() + kCropLifetimeSeconds;
  co_await cropCapabilityRepository_.purgeExpired(now());
  co_await cropCapabilityRepository_.create({.tokenHash = *hash,
                                             .personId = request.personId,
                                             .sampleId = sampleId,
                                             .requesterUserId = request.requester.userId,
                                             .expiresAt = expiresAt});
  co_return ResponseVisitorCropCapabilityDto{.token = std::move(*token),
                                             .expiresAt = expiresAt};
}

drogon::Task<ResponseVisitorCropImageDto>
VisitorFeatureService::consumeCrop(const VisitorCropConsumeRequest& request) const
{
  const auto hash = opaque_token::sha256Hex(request.token);
  if (request.token.empty() || !hash)
    throw ResponseException(IdentityErrors::VisitorCropUnavailable);
  const auto consumed = co_await cropCapabilityRepository_.consume(
      {.tokenHash = *hash, .requesterUserId = request.requester.userId, .now = now()});
  if (!consumed)
    throw ResponseException(IdentityErrors::VisitorCropUnavailable);
  co_await requireVisible(
      {.requester = request.requester, .personId = consumed->personId});
  const auto key = co_await repository_.sampleCrop(consumed->personId,
                                                   consumed->sampleId);
  const auto bytes = key ? co_await cropStore_.read(*key) : std::nullopt;
  if (!bytes)
    throw ResponseException(IdentityErrors::VisitorCropUnavailable);
  if (const auto* sink = identity_change::getSink()) {
    Json::Value data(Json::objectValue);
    data["event"] = "visitor_crop_view";
    co_await sink->publishAction({.event = {.userId = request.requester.userId,
                                            .recordId = consumed->personId,
                                            .tableName = TableName::Person,
                                            .action = UserAction::Read,
                                            .oldData = Json::Value(),
                                            .newData = data,
                                            .ipAddress = ""},
                                  .client = nullptr});
  }
  co_return ResponseVisitorCropImageDto{.mimeType = "image/jpeg",
                                        .base64 = base64(*bytes)};
}

drogon::Task<ResponseVisitorSettingsDto> VisitorFeatureService::settings() const
{
  co_return ResponseVisitorSettingsDto{
      .setting = co_await settingRepository_.find(),
      .recognitionEnabled = (co_await privacyGate_.household()).visitorRecognition};
}

drogon::Task<ResponseVisitorSettingsDto>
VisitorFeatureService::updateSettings(const VisitorSettingsUpdateRequest& request) const
{
  requireOwner(request.requester);
  const auto setting = co_await settingRepository_.update(
      {.unnamedRetentionDays = request.body.unnamedRetentionDays,
       .updatedBy = request.requester.userId});
  Json::Value details(Json::objectValue);
  details["unnamedRetentionDays"] = static_cast<Json::Int64>(setting.unnamedRetentionDays);
  co_await journal({.actorId = request.requester.userId,
                    .personId = 0,
                    .event = "visitor_settings",
                    .details = details});
  co_return ResponseVisitorSettingsDto{
      .setting = setting,
      .recognitionEnabled = (co_await privacyGate_.household()).visitorRecognition};
}
