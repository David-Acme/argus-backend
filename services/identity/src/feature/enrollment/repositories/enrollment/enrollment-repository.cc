#include "enrollment-repository.hxx"

#include <shared/vocabulary/face-model.hxx>

#include <sqlite/db-service.hxx>
#include <vector>

using namespace enrollment_query;

drogon::Task<int64_t>
EnrollmentRepository::countUsers(drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::identityClient();
  auto* resolved = client ? client : pooled.get();
  const auto result = co_await resolved->execSqlCoro(COUNT_USERS.data());
  if (result.empty())
    co_return 0;
  co_return result.front()["total"].as<int64_t>();
}

drogon::Task<int64_t>
EnrollmentRepository::insertUser(const EnrollmentUserInput& input) const
{
  const auto pooled = DbService::identityClient();
  auto* client = input.client ? input.client : pooled.get();
  const auto result = co_await client->execSqlCoro(
      INSERT_USER.data(), input.name, input.lastName, input.role, input.lang);
  co_return static_cast<int64_t>(result.insertId());
}

drogon::Task<int64_t>
EnrollmentRepository::insertPerson(const EnrollmentPersonInput& input) const
{
  const auto pooled = DbService::identityClient();
  auto* client = input.client ? input.client : pooled.get();
  const auto result =
      co_await client->execSqlCoro(INSERT_PERSON.data(), input.userId,
                                   input.name);
  co_return static_cast<int64_t>(result.insertId());
}

drogon::Task<int64_t> EnrollmentRepository::insertFaceEmbedding(
    const EnrollmentFaceInput& input) const
{
  const auto pooled = DbService::identityClient();
  auto* client = input.client ? input.client : pooled.get();
  const std::vector<char> blob(input.embedding.begin(), input.embedding.end());
  const auto result = co_await client->execSqlCoro(
      INSERT_FACE_EMBEDDING.data(), input.personId, blob, input.quality,
      std::string(kFaceModelId));
  co_return static_cast<int64_t>(result.insertId());
}

drogon::Task<bool> EnrollmentRepository::consumeInvitation(
    const EnrollmentInvitationConsumeInput& input) const
{
  const auto pooled = DbService::identityClient();
  auto* client = input.client ? input.client : pooled.get();
  const auto result = co_await client->execSqlCoro(
      CONSUME_INVITATION.data(), input.tokenHash, input.now);
  co_return result.affectedRows() == 1;
}

drogon::Task<void> EnrollmentRepository::insertRedemption(
    const EnrollmentRedemptionInput& input) const
{
  const auto pooled = DbService::identityClient();
  auto* client = input.client ? input.client : pooled.get();
  co_await client->execSqlCoro(INSERT_REDEMPTION.data(), input.invitationId,
                               input.userId);
  co_return;
}
