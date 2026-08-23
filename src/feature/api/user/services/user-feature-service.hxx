#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/api/user/dtos/update-user-dto.hxx>
#include <shared/repositories/refresh-token/refresh-token-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/services/socket/socket-service.hxx>
#include <shared/services/sync-audit/sync-audit-service.hxx>
#include <shared/services/user-action-log/user-action-log-service.hxx>
#include <vector>

struct UserManagementUpdateInput
{
  int64_t targetUserId{0};
  int64_t actorId{0};
  UpdateUserDto body;
};

struct UserChangeLogInput
{
  int64_t actorId{0};
  UserSchema before;
  UserSchema after;
  UserAction action{UserAction::Update};
};

class UserFeatureService
{
public:
  drogon::Task<std::vector<UserSchema>>
  list(int64_t actorId, UserRole actorRole) const;
  drogon::Task<UserSchema>
  update(const UserManagementUpdateInput& input) const;
  drogon::Task<void> deactivate(int64_t targetUserId, int64_t actorId) const;

private:
  void emitAuthContextChanged(const UserSchema& user) const;
  drogon::Task<void> recordChange(const UserChangeLogInput& input) const;

  UserRepository repository_;
  RefreshTokenRepository refreshTokenRepository_;
  UserActionLogService userActionLogService_;
  SocketService socketService_;
  SyncAuditService syncAuditService_;
};
