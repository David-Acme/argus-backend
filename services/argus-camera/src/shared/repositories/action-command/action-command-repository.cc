#include "action-command-repository.hxx"

#include <shared/services/sqlite/db-service.hxx>
#include <string>

using namespace action_command_query;

bool ActionCommandRepository::migrateLegacySchema() const
{
  auto client = DbService::client();
  const auto hasColumn = [&client](const std::string& column) {
    const auto rows = client->execSqlSync(
        "SELECT COUNT(*) AS total FROM pragma_table_info('action_command') "
        "WHERE name = ?",
        column);
    return !rows.empty() && rows.front()["total"].as<int>() > 0;
  };
  if (!hasColumn("response"))
    client->execSqlSync(
        "ALTER TABLE action_command ADD COLUMN response TEXT NOT NULL "
        "DEFAULT ''");
  if (!hasColumn("generation"))
    client->execSqlSync(
        "ALTER TABLE action_command ADD COLUMN generation INTEGER NOT NULL "
        "DEFAULT 0");
  if (!hasColumn("fingerprint"))
    client->execSqlSync(
        "ALTER TABLE action_command ADD COLUMN fingerprint TEXT NOT NULL "
        "DEFAULT ''");
  client->execSqlSync(MIGRATE_LEGACY_STATUS.data());
  return true;
}

drogon::Task<ActionClaim>
ActionCommandRepository::claim(const ActionCommandClaimInput& input) const
{
  ActionClaim claim;
  if (input.commandId.empty())
    co_return claim;
  auto client = DbService::client();

  const int64_t lease = input.leaseSeconds > 0 ? input.leaseSeconds : 60;
  const int64_t staleBefore = input.at - lease;
  for (int attempt = 0; attempt < 4; ++attempt) {
    const auto existing =
        co_await client->execSqlCoro(FIND_STATUS.data(), input.commandId);
    if (existing.empty()) {
      const auto inserted =
          co_await client->execSqlCoro(CLAIM.data(), input.commandId,
                                       input.kind, input.cameraId,
                                       input.fingerprint, input.at, input.at);
      if (inserted.affectedRows() > 0) {
        claim.kind = ActionClaimKind::New;
        co_return claim;
      }
      continue;
    }
    const auto& row = existing.front();
    const std::string status = row["status"].as<std::string>();
    claim.generation = row["generation"].as<int64_t>();
    claim.detail = row["detail"].as<std::string>();
    claim.response = row["response"].as<std::string>();
    const std::string storedFingerprint = row["fingerprint"].as<std::string>();
    if (storedFingerprint.empty() && !input.fingerprint.empty()) {
      // Legacy row with no fingerprint evidence: never authorize a new
      // payload. A completed row still replays its persisted result.
      if (status == "succeeded") {
        claim.kind = ActionClaimKind::Completed;
        co_return claim;
      }
      if (status == "indeterminate") {
        claim.kind = ActionClaimKind::Indeterminate;
        claim.detail = "legacy_indeterminate";
        co_return claim;
      }
      if (status == "rejected") {
        claim.kind = ActionClaimKind::Rejected;
        co_return claim;
      }
      claim.kind = ActionClaimKind::Conflict;
      claim.detail = "legacy_indeterminate";
      co_return claim;
    }
    if (!storedFingerprint.empty() && input.fingerprint.empty()) {
      claim.kind = ActionClaimKind::Conflict;
      claim.detail = "fingerprint_missing";
      co_return claim;
    }
    if (!storedFingerprint.empty() && storedFingerprint != input.fingerprint) {
      claim.kind = ActionClaimKind::Conflict;
      claim.detail = "fingerprint_conflict";
      co_return claim;
    }
    if (status == "succeeded") {
      claim.kind = ActionClaimKind::Completed;
      co_return claim;
    }
    if (status == "indeterminate") {
      claim.kind = ActionClaimKind::Indeterminate;
      claim.detail = "indeterminate";
      co_return claim;
    }
    if (status == "rejected") {
      claim.kind = ActionClaimKind::Rejected;
      co_return claim;
    }
    if (status == "executing") {
      claim.kind = ActionClaimKind::InFlight;
      claim.detail = row["updated_at"].as<int64_t>() > staleBefore ? "in_flight"
                                                                   : "expired";
      co_return claim;
    }
    const auto rearm =
        co_await client->execSqlCoro(REARM_FAILED.data(), input.at,
                                     input.commandId, claim.generation);
    if (!rearm.empty()) {
      claim.generation = rearm.front()["generation"].as<int64_t>();
      claim.kind = ActionClaimKind::Retry;
      co_return claim;
    }
    claim.kind = ActionClaimKind::Indeterminate;
    claim.detail = status.empty() ? "unknown_status" : status;
    co_return claim;
  }
  claim.kind = ActionClaimKind::InFlight;
  claim.detail = "in_flight";
  co_return claim;
}

drogon::Task<ActionCommandRow>
ActionCommandRepository::find(const std::string& commandId) const
{
  ActionCommandRow row;
  if (commandId.empty())
    co_return row;
  auto client = DbService::client();
  const auto rows = co_await client->execSqlCoro(FIND_STATUS.data(), commandId);
  if (rows.empty())
    co_return row;
  row.found = true;
  row.status = rows.front()["status"].as<std::string>();
  row.detail = rows.front()["detail"].as<std::string>();
  row.response = rows.front()["response"].as<std::string>();
  row.generation = rows.front()["generation"].as<int64_t>();
  co_return row;
}

drogon::Task<bool>
ActionCommandRepository::settle(const ActionCommandResultInput& input) const
{
  if (input.commandId.empty())
    co_return true;
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(SETTLE.data(), input.status, input.detail,
                                   input.response, input.at, input.commandId,
                                   input.generation);
  co_return result.affectedRows() > 0;
}

drogon::Task<int64_t>
ActionCommandRepository::reconcileExpired(int64_t at,
                                          int64_t leaseSeconds) const
{
  auto client = DbService::client();
  const int64_t staleBefore = at - (leaseSeconds > 0 ? leaseSeconds : 60);
  const auto result =
      co_await client->execSqlCoro(RECONCILE_EXPIRED.data(), at, staleBefore);
  co_return result.affectedRows();
}

drogon::Task<bool>
ActionCommandRepository::upsertLease(const SirenLeaseInput& input) const
{
  if (input.cameraId <= 0)
    co_return false;
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(UPSERT_LEASE.data(), input.cameraId,
                                   input.commandId, input.expiresAt, input.at);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> ActionCommandRepository::deleteLease(int64_t cameraId) const
{
  if (cameraId <= 0)
    co_return false;
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(DELETE_LEASE.data(), cameraId);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<int64_t>>
ActionCommandRepository::expiredLeases(int64_t now) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(EXPIRED_LEASES.data(), now);
  std::vector<int64_t> cameras;
  cameras.reserve(result.size());
  for (const auto& row : result)
    cameras.push_back(row["camera_id"].as<int64_t>());
  co_return cameras;
}
