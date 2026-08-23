#pragma once

#include "user-invitation-query.hxx"

#include <drogon/utils/coroutine.h>
#include <shared/contracts/syncable.hxx>
#include <optional>
#include <shared/schemas/user-invitation/user-invitation-schema.hxx>
#include <vector>

class UserInvitationRepository : public Syncable
{
public:
  UserInvitationRepository() = default;

  drogon::Task<UserInvitationSchema>
  create(const UserInvitationCreateInput& input) const;
  drogon::Task<std::optional<UserInvitationSchema>>
  findById(int64_t id) const;
  drogon::Task<std::optional<UserInvitationSchema>>
  findByTokenHash(const std::string& tokenHash) const;
  drogon::Task<std::vector<UserInvitationSchema>> findAll() const;
  drogon::Task<bool> revoke(const UserInvitationRevokeInput& input) const;

  // The caller must use this conditional update in its enrollment transaction,
  // immediately before recording the completed redemption.
  drogon::Task<bool> tryConsume(int64_t invitationId, int64_t now) const;
  drogon::Task<bool>
  recordRedemption(const InvitationRedemptionCreateInput& input) const;

  drogon::Task<std::vector<Json::Value>>
  find(const SyncFilter& filter) const override;
  drogon::Task<std::vector<Json::Value>>
  findDeleted(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>>
  findLast(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>>
  findLastDeleted(const SyncFilter& filter) const override;
};
