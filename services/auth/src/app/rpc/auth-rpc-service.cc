#include "auth-rpc-service.hxx"

#include <drogon/drogon.h>
#include <grpc/grpc-client-base.hxx>
#include <grpc/grpc-server-identity.hxx>
#include <trantor/utils/Logger.h>
#include <utility>

namespace
{
grpc::ServerUnaryReactor*
rejectUnauthenticated(grpc::CallbackServerContext* context)
{
  auto* reactor = context->DefaultReactor();
  reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                               "Fleet secret missing or invalid"));
  return reactor;
}
}

AuthRpcService::AuthRpcService(Dependencies dependencies,
                               std::string fleetSecret)
    : dependencies_(dependencies), fleetSecret_(std::move(fleetSecret))
{
}

bool AuthRpcService::fleetAuthorized(
    const grpc::CallbackServerContext* context) const
{
  if (fleetSecret_.empty())
    return true;
  return argus::client::constantTimeEquals(
      argus::client::metadata(context, argus::client::kFleetSecretKey),
      fleetSecret_);
}

grpc::ServerUnaryReactor* AuthRpcService::ValidateToken(
    grpc::CallbackServerContext* context,
    const argus::auth::v1::ValidateTokenRequest* request,
    argus::auth::v1::ValidateTokenResponse* response)
{
  if (!fleetAuthorized(context))
    return rejectUnauthenticated(context);

  const SessionValidationInput input{
      .accessToken = request->access_token(),
      .deviceHash = request->has_device_hash() ? request->device_hash() : "",
      .hasDeviceContext = request->has_device_hash(),
      .origin = request->has_origin()
                    ? sessionOriginFromString(request->origin())
                    : SessionOrigin::Unknown,
  };
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop(
      [this, reactor, input, responseWriter]() {
        drogon::async_run([this, reactor, input,
                           responseWriter]() -> drogon::Task<void> {
          try {
            const SessionVerdict verdict =
                co_await dependencies_.sessions->validate(input);
            if (!verdict.valid || !verdict.user.has_value()) {
              responseWriter->set_valid(false);
              responseWriter->set_reason(verdict.reason);
              reactor->Finish(grpc::Status::OK);
              co_return;
            }

            const UserContext& user = *verdict.user;
            auto* payload = responseWriter->mutable_user();
            payload->set_user_id(user.userId);
            payload->set_name(user.name);
            payload->set_lang(user.lang);
            payload->set_last_name(user.lastName);
            payload->set_role(user.role);
            payload->set_is_active(user.isActive);
            responseWriter->set_expires_at(verdict.expiresAt);
            responseWriter->set_session_id(verdict.sessionId);
            responseWriter->set_valid(true);
            reactor->Finish(grpc::Status::OK);
          }
          catch (const std::exception& error) {
            LOG_WARN << "Auth RPC: ValidateToken failed: " << error.what();
            reactor->Finish(
                grpc::Status(grpc::StatusCode::INTERNAL, error.what()));
          }
          co_return;
        });
      });
  return reactor;
}

grpc::ServerUnaryReactor* AuthRpcService::CheckDeviceCredential(
    grpc::CallbackServerContext* context,
    const argus::auth::v1::CheckDeviceCredentialRequest* request,
    argus::auth::v1::CheckDeviceCredentialResponse* response)
{
  if (!fleetAuthorized(context))
    return rejectUnauthenticated(context);

  const auto& secretHash = request->secret_hash();
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, secretHash,
                                        responseWriter]() {
    drogon::async_run([this, reactor, secretHash,
                       responseWriter]() -> drogon::Task<void> {
      try {
        const auto active =
            co_await dependencies_.deviceCredentials->findActiveBySecretHash(
                secretHash);
        responseWriter->set_active(active.has_value());
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& error) {
        LOG_WARN << "Auth RPC: CheckDeviceCredential failed: " << error.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, error.what()));
      }
      co_return;
    });
  });
  return reactor;
}
