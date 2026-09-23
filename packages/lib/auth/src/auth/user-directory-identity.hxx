#pragma once

#include <auth/user-directory.hxx>

class IdentityUserDirectory final : public IUserDirectory
{
public:
  drogon::Task<std::optional<DirectoryUser>>
  findById(int64_t userId) const override;
};
