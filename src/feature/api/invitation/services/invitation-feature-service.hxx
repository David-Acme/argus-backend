#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/api/invitation/dtos/create-invitation-dto.hxx>
#include <feature/api/invitation/dtos/response-invitation-dto.hxx>
#include <feature/api/invitation/dtos/response-invitation-resolve-dto.hxx>
#include <shared/repositories/user-invitation/user-invitation-repository.hxx>
#include <shared/services/socket/socket-service.hxx>
#include <shared/services/user-action-log/user-action-log-service.hxx>
#include <string>
#include <vector>

class InvitationFeatureService
{
public:
  drogon::Task<ResponseInvitationDto>
  create(const CreateInvitationDto& body, int64_t actorId) const;
  drogon::Task<std::vector<ResponseInvitationDto>> list() const;
  drogon::Task<void> revoke(int64_t invitationId, int64_t actorId) const;
  drogon::Task<ResponseInvitationResolveDto>
  resolve(const std::string& token) const;

  static std::string hashToken(const std::string& token);

private:
  struct InvitationActionLogInput
  {
    int64_t actorId{0};
    UserInvitationSchema before;
    UserInvitationSchema after;
    UserAction action{UserAction::Create};
  };

  void emitInvitation(const UserInvitationSchema& invitation) const;
  drogon::Task<void>
  recordInvitationAction(const InvitationActionLogInput& input) const;

  UserInvitationRepository repository_;
  SocketService socketService_;
  UserActionLogService userActionLogService_;
};
