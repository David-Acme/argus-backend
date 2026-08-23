#include "portrait-access-request-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>

using namespace portrait_access_request_query;

drogon::Task<PortraitAccessRequestSchema>
PortraitAccessRequestRepository::create(
    const PortraitAccessRequestCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(), input.portraitUserId, input.requesterUserId);

  PortraitAccessRequestSchema schema;
  schema.id = result.insertId();
  schema.portraitUserId = input.portraitUserId;
  schema.requesterUserId = input.requesterUserId;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<std::optional<PortraitAccessRequestSchema>>
PortraitAccessRequestRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty())
    co_return std::nullopt;
  co_return PortraitAccessRequestSchema(result.front());
}

drogon::Task<std::vector<PortraitAccessRequestSchema>>
PortraitAccessRequestRepository::findPendingForPortraitUser(
    int64_t portraitUserId) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      FIND_PENDING_FOR_PORTRAIT.data(), portraitUserId,
      portraitAccessRequestStatusToString(PortraitAccessRequestStatus::Pending));
  std::vector<PortraitAccessRequestSchema> requests;
  requests.reserve(result.size());
  for (const auto& row : result)
    requests.emplace_back(row);
  co_return requests;
}

drogon::Task<bool>
PortraitAccessRequestRepository::resolve(
    const PortraitAccessRequestResolveInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      RESOLVE.data(), portraitAccessRequestStatusToString(input.status),
      input.resolvedBy, input.requestId,
      portraitAccessRequestStatusToString(PortraitAccessRequestStatus::Pending));
  co_return result.affectedRows() > 0;
}
