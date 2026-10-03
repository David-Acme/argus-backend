#pragma once

#include <auth/user-directory.hxx>

class IdentityUserDirectory final : public IUserDirectory
{
public:
  drogon::Task<DirectoryLookup> lookup(int64_t userId) const override;
};
