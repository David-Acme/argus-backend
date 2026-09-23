#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <string>
#include <auth/user-role.hxx>

struct DirectoryUser
{
  int64_t id{0};
  std::string name;
  std::string lastName;
  std::string lang;
  UserRole role{UserRole::Guest};
  bool isActive{false};
};

class IUserDirectory
{
public:
  virtual ~IUserDirectory() = default;

  virtual drogon::Task<std::optional<DirectoryUser>>
  findById(int64_t userId) const = 0;
};
