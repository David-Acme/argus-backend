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
  UserRole role{UserRole::Unknown};
  bool isActive{false};
};

enum class DirectoryLookupStatus : std::uint8_t
{
  Found,
  Missing,
  Unavailable
};

struct DirectoryLookup
{
  DirectoryLookupStatus status{DirectoryLookupStatus::Unavailable};
  std::optional<DirectoryUser> user;
};

class IUserDirectory
{
public:
  virtual ~IUserDirectory() = default;

  virtual drogon::Task<DirectoryLookup> lookup(int64_t userId) const = 0;

  drogon::Task<std::optional<DirectoryUser>> findById(int64_t userId) const
  {
    co_return (co_await lookup(userId)).user;
  }
};
