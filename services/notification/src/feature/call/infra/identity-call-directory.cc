#include "identity-call-directory.hxx"

#include <utility>

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
  const auto& user = response->user();
  return {.found = true,
          .name = user.name(),
          .lang = user.lang(),
          .role = user.has_role() ? user.role() : std::string{},
          .active = !user.has_is_active() || user.is_active()};
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
