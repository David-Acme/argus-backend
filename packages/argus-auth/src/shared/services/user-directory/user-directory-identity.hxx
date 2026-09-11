#pragma once

#include <shared/contracts/user-directory.hxx>

// IdentityClient-backed directory; reads travel over argus.identity.v1.
class IdentityUserDirectory final : public IUserDirectory
{
public:
  drogon::Task<std::optional<DirectoryUser>>
  findById(int64_t userId) const override;
};
