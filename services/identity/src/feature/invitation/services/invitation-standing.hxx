#pragma once

#include <auth/user-role.hxx>
#include <cstdint>
#include <optional>
#include <shared/schemas/user-invitation/user-invitation-schema.hxx>
#include <string>

enum class InvitationStanding : std::uint8_t
{
  Usable = 0,
  Invalid,
  ModuleDisabled
};

struct InvitationAssessment
{
  InvitationStanding standing{InvitationStanding::Invalid};
  std::string moduleId{};
};

namespace invitation_standing
{
[[nodiscard]] InvitationAssessment assess(const std::optional<UserInvitationSchema>& invitation, int64_t now);
[[nodiscard]] InvitationAssessment ofRole(UserRole role);
[[nodiscard]] UserInvitationSchema require(const std::optional<UserInvitationSchema>& invitation, int64_t now);
}
