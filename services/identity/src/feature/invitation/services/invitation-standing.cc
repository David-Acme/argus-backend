#include "invitation-standing.hxx"

#include <auth/module-gate.hxx>
#include <errors/error-list.hxx>
#include <errors/response-exception.hxx>
#include <identity/identity-errors.hxx>


InvitationAssessment invitation_standing::ofRole(UserRole role)
{
  const auto modules = moduleGate().current();
  if (modules->roleActive(role))
    return {.standing = InvitationStanding::Usable, .moduleId = {}};
  return {.standing = InvitationStanding::ModuleDisabled,
          .moduleId = std::string(modules->moduleOfRole(role).value_or(std::string_view()))};
}

InvitationAssessment invitation_standing::assess(const std::optional<UserInvitationSchema>& invitation, int64_t now)
{
  if (!invitation || !userRoleKnown(invitation->role) || invitation->redemptionCount >= invitation->maxRedemptions)
    return {};
  if (invitation->revokedReason == InvitationRevocationReason::ModuleDisabled)
    return {.standing = InvitationStanding::ModuleDisabled, .moduleId = invitation->revokedModule.value_or("")};
  if (invitation->revokedAt || invitation->expiresAt <= now)
    return {};
  return ofRole(invitation->role);
}

UserInvitationSchema invitation_standing::require(const std::optional<UserInvitationSchema>& invitation, int64_t now)
{
  const auto assessment = assess(invitation, now);
  if (assessment.standing == InvitationStanding::Usable)
    return invitation.value_or(UserInvitationSchema{});
  if (assessment.standing == InvitationStanding::Invalid)
    throw ResponseException(404, IdentityErrors::InvitationInvalidOrExpired);
  throw ResponseException(IdentityErrors::InvitationModuleDisabled.status,
                          error_list::forModule(IdentityErrors::InvitationModuleDisabled, assessment.moduleId));
}
