#pragma once

#include <feature/safety/infra/safety-ports.hxx>

class IdentityClient;

class IdentityActorDirectory : public SafetyActorDirectory
{
public:
  explicit IdentityActorDirectory(const IdentityClient* identity);

  [[nodiscard]] drogon::Task<SafetyActor> actorOf(int64_t userId) const override;

private:
  const IdentityClient* identity_{nullptr};
};
