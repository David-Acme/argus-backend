#include "identity-rpc.hxx"

#include <ctime>
#include <grpc-client-base.hxx>
#include <drogon/drogon.h>
#include <optional>
#include <shared/contracts/sync-operation.hxx>
#include <shared/dtos/socket-emit/socket-emit-dto.hxx>
#include <shared/enums.hxx>
#include <shared/services/socket/sync-change.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <trantor/utils/Logger.h>

namespace
{

// Row scoping: x-argus-user must carry the updated user's id.
std::optional<int64_t> scopedUserId(const grpc::CallbackServerContext* context)
{
  for (const auto& [key, value] : context->client_metadata()) {
    if (key == "x-argus-user") {
      try {
        return std::stoll(std::string(value.begin(), value.end()));
      }
      catch (const std::exception&) {
        return std::nullopt;
      }
    }
  }
  return std::nullopt;
}

// Secret comparison that does not return early on the first differing byte.
bool constantTimeEquals(const std::string& a, const std::string& b)
{
  if (a.size() != b.size())
    return false;
  unsigned char diff = 0;
  for (std::string::size_type i = 0; i < a.size(); ++i)
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  return diff == 0;
}

} // namespace

IdentityRpcService::IdentityRpcService(std::shared_ptr<NatsBus> bus,
                                       std::string fleetSecret)
    : bus_(std::move(bus)), fleetSecret_(std::move(fleetSecret))
{
}

bool IdentityRpcService::fleetAuthorized(
    const grpc::CallbackServerContext* context) const
{
  if (fleetSecret_.empty())
    return true;
  for (const auto& [key, value] : context->client_metadata()) {
    if (key == argus::sdk::kFleetSecretKey) {
      return constantTimeEquals(std::string(value.begin(), value.end()),
                                fleetSecret_);
    }
  }
  return false;
}

void IdentityRpcService::finishRejected(const TokenRejectionInput& input)
{
  input.response->set_valid(false);
  input.response->set_reason(input.reason);
  input.reactor->Finish(grpc::Status::OK);
}

grpc::ServerUnaryReactor* IdentityRpcService::UpdateUser(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::UpdateUserRequest* request,
    argus::identity::v1::UpdateUserResponse* response)
{
  if (!fleetAuthorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Fleet secret missing or invalid"));
    return reactor;
  }

  const std::optional<int64_t> scopedUser = scopedUserId(context);
  if (!scopedUser || *scopedUser != request->user_id()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "x-argus-user does not match user_id"));
    return reactor;
  }

  const int64_t userId = request->user_id();
  if (userId <= 0 || !request->has_name() || request->name().empty()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "user_id and name are required"));
    return reactor;
  }

  const std::string name = request->name();
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, userId, name,
                                        responseWriter]() {
    drogon::async_run([this, reactor, userId, name,
                       responseWriter]() -> drogon::Task<void> {
      try {
        auto user = co_await userRepository_.update(
            userId,
            {.name = name,
             .lastName = std::nullopt,
             .role = std::nullopt,
             .isActive = std::nullopt});
        if (user.id <= 0) {
          reactor->Finish(grpc::Status(grpc::StatusCode::NOT_FOUND,
                                       "user not found"));
          co_return;
        }

        SocketEmitDto emit;
        emit.operation = SyncOperation::Add;
        emit.option = TableName::User;
        emit.obj = user.toJson();
        if (bus_) {
          const Json::Value payload = sync_change::emitPayload(emit);
          bus_->publish(nats_subject::kSyncChange, json_util::toString(payload));
        }
        else {
          LOG_WARN << "Identity RPC: no NATS bus; user change not fanned out";
        }

        responseWriter->mutable_user()->set_user_id(user.id);
        responseWriter->mutable_user()->set_name(user.name);
        responseWriter->mutable_user()->set_lang(user.lang);
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Identity RPC: UpdateUser failed: " << e.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
      }
      co_return;
    });
  });
  return reactor;
}

// Device binding keys on request-field PRESENCE, not on the hash being non-empty.
grpc::ServerUnaryReactor* IdentityRpcService::ValidateToken(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::ValidateTokenRequest* request,
    argus::identity::v1::ValidateTokenResponse* response)
{
  if (!fleetAuthorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Fleet secret missing or invalid"));
    return reactor;
  }

  const std::string accessToken = request->access_token();
  const bool hasDeviceContext = request->has_device_hash();
  const std::string deviceHash =
      hasDeviceContext ? request->device_hash() : "";
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, accessToken, deviceHash,
                                        hasDeviceContext, responseWriter]() {
    drogon::async_run([this, reactor, accessToken, deviceHash,
                       hasDeviceContext,
                       responseWriter]() -> drogon::Task<void> {
      try {
        const auto claims = jwtService_.verifyAccess(accessToken);
        if (claims.empty()) {
          finishRejected({.reactor = reactor,
                          .response = responseWriter,
                          .reason = ""});
          co_return;
        }

        const auto subIt = claims.find("sub");
        int64_t userId = 0;
        if (subIt != claims.end()) {
          try {
            userId = std::stoll(subIt->second);
          }
          catch (const std::exception&) {
          }
        }
        if (userId <= 0) {
          finishRejected({.reactor = reactor,
                          .response = responseWriter,
                          .reason = ""});
          co_return;
        }

        const auto user = co_await userRepository_.findById(userId);
        if (!user) {
          finishRejected({.reactor = reactor,
                          .response = responseWriter,
                          .reason = ""});
          co_return;
        }

        if (!user->isActive) {
          finishRejected({.reactor = reactor,
                          .response = responseWriter,
                          .reason = "User account is disabled"});
          co_return;
        }

        int64_t expiresAt = 0;
        if (hasDeviceContext) {
          const auto rt =
              co_await refreshTokenRepository_.findByAccessToken(userId,
                                                                 accessToken);
          if (!rt) {
            finishRejected({.reactor = reactor,
                            .response = responseWriter,
                            .reason = ""});
            co_return;
          }

          expiresAt = rt->expiresAt;
          if (rt->expiresAt <= std::time(nullptr)) {
            LOG_WARN << "Refresh token expired for user " << userId;
            finishRejected({.reactor = reactor,
                            .response = responseWriter,
                            .reason = "Token expired"});
            co_return;
          }
          if (rt->deviceHash != deviceHash) {
            LOG_WARN << "Device hash mismatch for user " << userId;
            finishRejected({.reactor = reactor,
                            .response = responseWriter,
                            .reason = "Device mismatch"});
            co_return;
          }
        }

        auto* payload = responseWriter->mutable_user();
        payload->set_user_id(user->id);
        payload->set_name(user->name);
        payload->set_lang(user->lang);
        payload->set_last_name(user->lastName);
        payload->set_role(userRoleToString(user->role));
        payload->set_is_active(user->isActive);
        responseWriter->set_expires_at(expiresAt);
        responseWriter->set_valid(true);
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Identity RPC: ValidateToken failed: " << e.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
      }
      co_return;
    });
  });
  return reactor;
}

grpc::ServerUnaryReactor* IdentityRpcService::GetUser(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::GetUserRequest* request,
    argus::identity::v1::GetUserResponse* response)
{
  if (!fleetAuthorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Fleet secret missing or invalid"));
    return reactor;
  }

  const int64_t userId = request->user_id();
  if (userId <= 0) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "user_id is required"));
    return reactor;
  }

  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop(
      [this, reactor, userId, responseWriter]() {
        drogon::async_run([this, reactor, userId,
                           responseWriter]() -> drogon::Task<void> {
          try {
            const auto user = co_await userRepository_.findById(userId);
            if (user) {
              auto* payload = responseWriter->mutable_user();
              payload->set_user_id(user->id);
              payload->set_name(user->name);
              payload->set_lang(user->lang);
              payload->set_last_name(user->lastName);
              payload->set_role(userRoleToString(user->role));
              payload->set_is_active(user->isActive);
            }
            reactor->Finish(grpc::Status::OK);
          }
          catch (const std::exception& e) {
            LOG_WARN << "Identity RPC: GetUser failed: " << e.what();
            reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
          }
          co_return;
        });
      });
  return reactor;
}

grpc::ServerUnaryReactor* IdentityRpcService::ListPersons(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::ListPersonsRequest* request,
    argus::identity::v1::ListPersonsResponse* response)
{
  (void)request;
  if (!fleetAuthorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Fleet secret missing or invalid"));
    return reactor;
  }

  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, responseWriter]() {
    drogon::async_run([this, reactor, responseWriter]() -> drogon::Task<void> {
      try {
        for (const auto& person :
             co_await personRepository_.findAllCatalog()) {
          auto* row = responseWriter->add_persons();
          row->set_id(person.id);
          row->set_user_id(person.userId.value_or(0));
          row->set_name(person.name);
          row->set_alias(person.alias);
        }
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Identity RPC: ListPersons failed: " << e.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
      }
      co_return;
    });
  });
  return reactor;
}

grpc::ServerUnaryReactor* IdentityRpcService::CheckDeviceCredential(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::CheckDeviceCredentialRequest* request,
    argus::identity::v1::CheckDeviceCredentialResponse* response)
{
  if (!fleetAuthorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Fleet secret missing or invalid"));
    return reactor;
  }

  const std::string secretHash = request->secret_hash();
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, secretHash,
                                        responseWriter]() {
    drogon::async_run([this, reactor, secretHash,
                       responseWriter]() -> drogon::Task<void> {
      try {
        const auto active =
            co_await deviceCredentialRepository_.findActiveBySecretHash(
                secretHash);
        responseWriter->set_active(active.has_value());
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Identity RPC: CheckDeviceCredential failed: " << e.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
      }
      co_return;
    });
  });
  return reactor;
}
