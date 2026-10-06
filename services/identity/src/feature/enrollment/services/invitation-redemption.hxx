#pragma once

#include <auth/user-role.hxx>
#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <feature/enrollment/repositories/enrollment/enrollment-repository.hxx>
#include <shared/repositories/user-invitation/user-invitation-repository.hxx>
#include <string>

enum class RedemptionStatus : std::uint8_t
{
  Consumed = 0,
  Invalid,
  ModuleDisabled
};

struct InvitationRedemptionInput
{
  std::string tokenHash;
  UserRole role{UserRole::Unknown};
  int64_t now{0};
  drogon::orm::DbClient* client{nullptr};
};

struct InvitationRedemptionResult
{
  RedemptionStatus status{RedemptionStatus::Invalid};
  std::string moduleId{};
};

class InvitationRedemption
{
public:
  [[nodiscard]] drogon::Task<InvitationRedemptionResult> consume(const InvitationRedemptionInput& input) const;

private:
  EnrollmentRepository enrollmentRepository_;
  UserInvitationRepository invitationRepository_;
};
