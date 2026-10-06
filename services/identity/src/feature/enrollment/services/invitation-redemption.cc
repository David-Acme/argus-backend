#include "invitation-redemption.hxx"

#include <feature/invitation/services/invitation-standing.hxx>

drogon::Task<InvitationRedemptionResult> InvitationRedemption::consume(const InvitationRedemptionInput& input) const
{
  const auto standing = invitation_standing::ofRole(input.role);
  if (standing.standing != InvitationStanding::Usable)
    co_return InvitationRedemptionResult{.status = RedemptionStatus::ModuleDisabled, .moduleId = standing.moduleId};

  const bool consumed = co_await enrollmentRepository_.consumeInvitation(
      {.tokenHash = input.tokenHash, .now = input.now, .client = input.client});
  if (consumed)
    co_return InvitationRedemptionResult{.status = RedemptionStatus::Consumed};

  const auto stored = co_await invitationRepository_.findByTokenHash(input.tokenHash, input.client);
  const auto reason = invitation_standing::assess(stored, input.now);
  if (reason.standing == InvitationStanding::ModuleDisabled)
    co_return InvitationRedemptionResult{.status = RedemptionStatus::ModuleDisabled, .moduleId = reason.moduleId};
  co_return InvitationRedemptionResult{.status = RedemptionStatus::Invalid};
}
