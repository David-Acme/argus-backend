#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <feature/invitation/dtos/create-invitation-dto.hxx>
#include <feature/invitation/dtos/response-invitation-dto.hxx>
#include <feature/invitation/dtos/response-invitation-resolve-dto.hxx>
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
    drogon::orm::DbClient* client{nullptr};
  };

  struct InvitationEmitInput
  {
    UserInvitationSchema invitation;
    drogon::orm::DbClient* client{nullptr};
  };

  drogon::Task<void> emitInvitation(const InvitationEmitInput& input) const;
  drogon::Task<void>
  recordInvitationAction(const InvitationActionLogInput& input) const;

  UserInvitationRepository repository_;
};
