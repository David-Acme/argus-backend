#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/api/invitation/dtos/create-invitation-dto.hxx>
#include <feature/api/invitation/dtos/response-invitation-dto.hxx>
#include <feature/api/invitation/dtos/response-invitation-resolve-dto.hxx>
#include <shared/repositories/user-invitation/user-invitation-repository.hxx>
#include <string>
#include <sync/user-action.hxx>
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

  drogon::Task<void> emitInvitation(const UserInvitationSchema& invitation) const;
  drogon::Task<void>
  recordInvitationAction(const InvitationActionLogInput& input) const;

  UserInvitationRepository repository_;
};
