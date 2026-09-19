#include "user-directory-identity.hxx"

#include <filter/identity-access.hxx>
#include <identity/identity-client.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>

drogon::Task<std::optional<DirectoryUser>>
IdentityUserDirectory::findById(int64_t userId) const
{
  const auto client = filterIdentityClient();
  if (!client)
    co_return std::nullopt;

  const auto response = co_await BlockingTask<
      std::optional<argus::identity::v1::GetUserResponse>>(
      [client, userId] { return client->getUser(userId); });
  if (!response || !response->has_user())
    co_return std::nullopt;

  const auto& user = response->user();
  co_return DirectoryUser{
      .id = user.user_id(),
      .name = user.name(),
      .lastName = user.last_name(),
      .lang = user.lang(),
      .role = userRoleFromString(user.role()),
      .isActive = user.is_active(),
  };
}
