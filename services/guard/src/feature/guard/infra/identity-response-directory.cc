#include "identity-response-directory.hxx"

#include <identity/identity-client.hxx>

IdentityResponseDirectory::IdentityResponseDirectory(const IdentityClient* identity)
    : identity_(identity)
{
}

std::optional<std::vector<ResponseUser>> IdentityResponseDirectory::users() const
{
  if (identity_ == nullptr)
    return std::nullopt;
  const auto listed = identity_->listUsers();
  if (!listed)
    return std::nullopt;
  std::vector<ResponseUser> users;
  users.reserve(listed->size());
  for (const auto& user : *listed) {
    std::string name = user.name();
    if (user.has_last_name() && !user.last_name().empty() && name.empty())
      name = user.last_name();
    users.push_back({.userId = user.user_id(),
                     .role = userRoleFromString(user.has_role() ? user.role() : std::string()),
                     .active = !user.has_is_active() || user.is_active(),
                     .name = std::move(name),
                     .lang = user.lang()});
  }
  return users;
}
