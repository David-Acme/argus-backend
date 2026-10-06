#include "identity-role-reassign.hxx"

#include <auth/user-role.hxx>
#include <errors/response-exception.hxx>
#include <trantor/utils/Logger.h>

#include <utility>

namespace
{
constexpr std::string_view kRoleInvalid = "role_invalid";
constexpr std::string_view kFailed = "failed";
}

RoleReassignmentOutcome IdentityRoleReassign::reassign(const RoleReassignmentBatch& batch)
{
  return drogon::sync_wait(reassignAsync(batch));
}

drogon::Task<RoleReassignmentOutcome> IdentityRoleReassign::reassignAsync(RoleReassignmentBatch batch) const
{
  RoleReassignmentOutcome outcome{.status = ReassignStatus::Applied, .applied = 0, .failedUserId = 0, .reason = {}};
  for (const auto& entry : batch.reassignments) {
    const auto role = parseUserRole(entry.role);
    if (!role || *role == UserRole::Owner) {
      outcome = {.status = ReassignStatus::Refused,
                 .applied = outcome.applied,
                 .failedUserId = entry.userId,
                 .reason = std::string(kRoleInvalid)};
      co_return outcome;
    }
    try {
      static_cast<void>(co_await users_.update(
          {.targetUserId = entry.userId,
           .actorId = batch.actorUserId,
           .body = {.name = std::nullopt,
                    .lastName = std::nullopt,
                    .role = entry.role,
                    .userRole = role,
                    .isActive = std::nullopt}}));
      ++outcome.applied;
    }
    catch (const ResponseException& error) {
      co_return RoleReassignmentOutcome{.status = ReassignStatus::Refused,
                                        .applied = outcome.applied,
                                        .failedUserId = entry.userId,
                                        .reason = error.errorCode()};
    }
    catch (const std::exception& error) {
      LOG_WARN << "Identity: reassigning user " << entry.userId << " to " << entry.role << " failed: " << error.what();
      co_return RoleReassignmentOutcome{.status = ReassignStatus::Failed,
                                        .applied = outcome.applied,
                                        .failedUserId = entry.userId,
                                        .reason = std::string(kFailed)};
    }
  }
  co_return outcome;
}
