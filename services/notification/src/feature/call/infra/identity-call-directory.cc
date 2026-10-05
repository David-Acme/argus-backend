#include "identity-call-directory.hxx"

#include <algorithm>
#include <utility>

namespace
{
CallRecipient recipientOf(const argus::identity::v1::UserIdentity& user)
{
  return {.found = true,
          .name = user.name(),
          .lang = user.lang(),
          .role = user.has_role()
                      ? std::optional<UserRole>(userRoleFromString(user.role()))
                      : std::nullopt,
          .active = !user.has_is_active() || user.is_active()};
}
}

IdentityCallDirectory::IdentityCallDirectory(
    std::shared_ptr<const IdentityClient> client)
    : client_(std::move(client))
{
}

CallRecipient IdentityCallDirectory::recipient(int64_t userId) const
{
  if (!client_)
    return {};
  const auto response = client_->getUser(userId);
  if (!response || !response->has_user())
    return {};
  return recipientOf(response->user());
}

std::unordered_map<int64_t, CallRecipient>
IdentityCallDirectory::recipients(const std::vector<int64_t>& userIds) const
{
  if (!client_ || userIds.size() < 2)
    return CallDirectory::recipients(userIds);
  const auto users = client_->listUsers();
  if (!users)
    return CallDirectory::recipients(userIds);
  std::unordered_map<int64_t, CallRecipient> found;
  found.reserve(userIds.size());
  for (const auto& user : *users) {
    if (std::ranges::find(userIds, user.user_id()) != userIds.end())
      found.emplace(user.user_id(), recipientOf(user));
  }
  return found;
}

CallPerson IdentityCallDirectory::person(int64_t personId) const
{
  if (!client_)
    return {};
  const auto profile = client_->getPerson(personId);
  if (!profile)
    return {};
  return {.found = true,
          .name = profile->alias.empty() ? profile->name : profile->alias,
          .userId = profile->userId.value_or(0)};
}
