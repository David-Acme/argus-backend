#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/user/services/user-feature-service.hxx>
#include <settings/role-reassign-host.hxx>

class IdentityRoleReassign final : public RoleReassignHost
{
public:
  RoleReassignmentOutcome reassign(const RoleReassignmentBatch& batch) override;
  [[nodiscard]] drogon::Task<RoleReassignmentOutcome> reassignAsync(RoleReassignmentBatch batch) const;

private:
  UserFeatureService users_;
};
