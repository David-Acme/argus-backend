#include "identity-actor-directory.hxx"

#include <identity/identity-client.hxx>
#include <runtime/blocking-task.hxx>

IdentityActorDirectory::IdentityActorDirectory(const IdentityClient* identity) : identity_(identity) {}

drogon::Task<SafetyActor> IdentityActorDirectory::actorOf(int64_t userId) const
{
  if (identity_ == nullptr || userId <= 0)
    co_return SafetyActor{};
  const IdentityClient* identity = identity_;
  co_return co_await BlockingTask<SafetyActor>{[identity, userId]() {
    const auto response = identity->getUser(userId);
    if (!response || !response->has_user())
      return SafetyActor{};
    const auto& user = response->user();
    return SafetyActor{.known = true,
                       .active = !user.has_is_active() || user.is_active(),
                       .name = user.name(),
                       .role = user.has_role() ? user.role() : std::string{}};
  }};
}
