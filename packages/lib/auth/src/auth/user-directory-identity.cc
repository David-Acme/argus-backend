#include "user-directory-identity.hxx"

#include <auth/details/identity-access.hxx>
#include <identity/identity-client.hxx>
#include <runtime/blocking-task.hxx>

drogon::Task<DirectoryLookup>
IdentityUserDirectory::lookup(int64_t userId) const
{
  const auto client = filterIdentityClient();
  if (!client)
    co_return DirectoryLookup{.status = DirectoryLookupStatus::Unavailable, .user = std::nullopt};

  const auto response = co_await BlockingTask<
      std::optional<argus::identity::v1::GetUserResponse>>(
      [client, userId] { return client->getUser(userId); });
  if (!response)
    co_return DirectoryLookup{.status = DirectoryLookupStatus::Unavailable, .user = std::nullopt};
  if (!response->has_user())
    co_return DirectoryLookup{.status = DirectoryLookupStatus::Missing, .user = std::nullopt};

  const auto& user = response->user();
  co_return DirectoryLookup{
      .status = DirectoryLookupStatus::Found,
      .user = DirectoryUser{
          .id = user.user_id(),
          .name = user.name(),
          .lastName = user.last_name(),
          .lang = user.lang(),
          .role = userRoleFromString(user.role()),
          .isActive = user.is_active(),
      }};
}
