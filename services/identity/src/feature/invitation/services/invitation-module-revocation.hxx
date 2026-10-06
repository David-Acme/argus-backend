#pragma once

#include <cstddef>
#include <drogon/utils/coroutine.h>
#include <shared/repositories/user-invitation/user-invitation-repository.hxx>
#include <string>
#include <vector>

struct InvitationModuleRevocationInput
{
  std::string moduleId;
  std::vector<std::string> roles;
};

class InvitationModuleRevocation
{
public:
  [[nodiscard]] drogon::Task<std::vector<UserInvitationSchema>>
  revoke(const InvitationModuleRevocationInput& input) const;
  [[nodiscard]] drogon::Task<std::size_t> revokeModule(std::string moduleId) const;
  [[nodiscard]] drogon::Task<std::size_t> revokeDisabledModules() const;

private:
  UserInvitationRepository repository_;
};
