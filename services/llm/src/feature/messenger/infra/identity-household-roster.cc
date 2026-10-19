#include "identity-household-roster.hxx"

#include <auth/user-role.hxx>

#include <trantor/utils/Logger.h>

#include <utility>

IdentityHouseholdRoster::IdentityHouseholdRoster(std::shared_ptr<const IdentityClient> client)
    : client_(std::move(client))
{
}

std::vector<relay_recipient::Member> IdentityHouseholdRoster::members() const
{
  std::vector<relay_recipient::Member> members;
  if (!client_)
    return members;
  const auto users = client_->listUsers();
  if (!users) {
    LOG_WARN << "argus-llm: the household roster is unavailable; no message can be relayed";
    return members;
  }
  for (const auto& user : *users) {
    if (!user.has_is_active() || !user.is_active())
      continue;
    if (!user.has_role() || userRoleFromString(user.role()) == UserRole::Unknown)
      continue;
    if (user.name().empty())
      continue;
    members.push_back({.userId = user.user_id(), .name = user.name()});
  }
  return members;
}
