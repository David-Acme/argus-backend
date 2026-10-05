#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <feature/user/dtos/update-user-dto.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <sync/user-action.hxx>
#include <optional>
#include <string>
#include <vector>

struct UserManagementUpdateInput
{
  int64_t targetUserId{0};
  int64_t actorId{0};
  UpdateUserDto body;
};

struct UserRenameInput
{
  int64_t userId{0};
  std::string name;
};

struct UserChangeLogInput
{
  int64_t actorId{0};
  UserSchema before;
  UserSchema after;
  UserAction action{UserAction::Update};
  drogon::orm::DbClient* client{nullptr};
};

class UserFeatureService
{
public:
  drogon::Task<std::vector<UserSchema>>
  list(int64_t actorId, UserRole actorRole) const;
  drogon::Task<UserSchema>
  update(const UserManagementUpdateInput& input) const;
  drogon::Task<void> deactivate(int64_t targetUserId, int64_t actorId) const;
  drogon::Task<std::optional<UserSchema>> rename(const UserRenameInput& input) const;

private:
  struct DirectoryAuditInput
  {
    const UserSchema& before;
    const UserSchema& after;
    drogon::orm::DbClient* client{nullptr};
  };

  drogon::Task<void> publishDirectoryAudit(const DirectoryAuditInput& input) const;
  void emitAuthContextChanged(const UserSchema& user) const;
  drogon::Task<void> recordChange(const UserChangeLogInput& input) const;

  UserRepository repository_;
};
