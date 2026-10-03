#pragma once

#include <argus/identity/v1/voiceprint.grpc.pb.h>
#include <auth/auth-client.hxx>
#include <drogon/utils/coroutine.h>
#include <feature/voiceprint/services/voiceprint/voiceprint-feature-service.hxx>
#include <functional>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

class IdentityVoiceprintRpcService final
    : public argus::identity::v1::VoiceprintService::CallbackService
{
public:
  struct Dependencies
  {
    std::string fleetSecret;
    std::shared_ptr<const AuthClient> auth;
    IdentityVoiceprintConfig voiceprint;
  };

  explicit IdentityVoiceprintRpcService(Dependencies dependencies);

  grpc::ServerUnaryReactor* CreateChallenge(
      grpc::CallbackServerContext* context,
      const argus::identity::v1::CreateVoiceprintChallengeRequest* request,
      argus::identity::v1::CreateVoiceprintChallengeResponse* response)
      override;

  grpc::ServerUnaryReactor*
  Enroll(grpc::CallbackServerContext* context,
         const argus::identity::v1::EnrollVoiceprintRequest* request,
         argus::identity::v1::EnrollVoiceprintResponse* response) override;

  grpc::ServerUnaryReactor*
  Verify(grpc::CallbackServerContext* context,
         const argus::identity::v1::VerifyVoiceprintRequest* request,
         argus::identity::v1::VerifyVoiceprintResponse* response) override;

  grpc::ServerUnaryReactor*
  Identify(grpc::CallbackServerContext* context,
           const argus::identity::v1::IdentifyVoiceRequest* request,
           argus::identity::v1::IdentifyVoiceResponse* response) override;

  grpc::ServerUnaryReactor*
  Delete(grpc::CallbackServerContext* context,
         const argus::identity::v1::DeleteVoiceprintRequest* request,
         argus::identity::v1::DeleteVoiceprintResponse* response) override;

  grpc::ServerUnaryReactor* GetStatus(
      grpc::CallbackServerContext* context,
      const argus::identity::v1::GetVoiceprintStatusRequest* request,
      argus::identity::v1::GetVoiceprintStatusResponse* response) override;

private:
  struct SessionCredentials
  {
    std::string accessToken;
    std::string deviceHash;
  };

  struct VerifiedActor
  {
    grpc::Status status;
    VoiceprintActor actor;
  };

  using Work = std::function<drogon::Task<grpc::Status>()>;

  [[nodiscard]] bool
  fleetAuthorized(const grpc::CallbackServerContext* context) const;

  [[nodiscard]] static SessionCredentials
  credentialsOf(const grpc::CallbackServerContext* context);

  [[nodiscard]] drogon::Task<VerifiedActor>
  verifyActor(SessionCredentials credentials) const;

  static grpc::ServerUnaryReactor* refuse(grpc::CallbackServerContext* context,
                                          const grpc::Status& status);

  static grpc::ServerUnaryReactor*
  dispatch(grpc::CallbackServerContext* context, Work work);

  Dependencies dependencies_;
  VoiceprintFeatureService service_;
};
