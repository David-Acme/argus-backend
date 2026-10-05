#pragma once

#include <argus/identity/v1/voiceprint.grpc.pb.h>
#include <atomic>
#include <drogon/utils/coroutine.h>
#include <feature/voiceprint/services/passive/passive-enrollment-service.hxx>
#include <feature/voiceprint/services/voiceprint/voiceprint-feature-service.hxx>
#include <functional>
#include <grpc/fleet-caller-gate.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

class IdentityVoiceprintRpcService final
    : public argus::identity::v1::VoiceprintService::CallbackService
{
public:
  struct Dependencies
  {
    std::shared_ptr<const argus::client::FleetCallerGate> gate;
    IdentityVoiceprintConfig voiceprint;
  };

  explicit IdentityVoiceprintRpcService(Dependencies dependencies);

  grpc::ServerUnaryReactor*
  Identify(grpc::CallbackServerContext* context,
           const argus::identity::v1::IdentifyVoiceRequest* request,
           argus::identity::v1::IdentifyVoiceResponse* response) override;

  grpc::ServerUnaryReactor*
  ObserveTurn(grpc::CallbackServerContext* context,
              const argus::identity::v1::ObserveVoiceTurnRequest* request,
              argus::identity::v1::IdentifyVoiceResponse* response) override;

  grpc::ServerUnaryReactor*
  CloseCall(grpc::CallbackServerContext* context,
            const argus::identity::v1::CloseVoiceCallRequest* request,
            argus::identity::v1::CloseVoiceCallResponse* response) override;

  void sweepIdleCalls() const;

  void purgeExpiredSamples() const;

  [[nodiscard]] const PassiveEnrollmentService& passive() const
  {
    return passive_;
  }

private:
  using Work = std::function<drogon::Task<grpc::Status>()>;

  [[nodiscard]] grpc::ServerUnaryReactor*
  refuseCaller(grpc::CallbackServerContext* context) const;

  static grpc::ServerUnaryReactor* refuse(grpc::CallbackServerContext* context,
                                          const grpc::Status& status);

  static grpc::ServerUnaryReactor*
  dispatch(grpc::CallbackServerContext* context, Work work);

  [[nodiscard]] bool admitLearning() const;

  [[nodiscard]] drogon::Task<grpc::Status>
  answerIdentify(const argus::identity::v1::VoiceClip& clip,
                 argus::identity::v1::IdentifyVoiceResponse* response) const;

  static constexpr int kMaxLearningInFlight = 8;

  Dependencies dependencies_;
  VoiceprintFeatureService service_;
  PassiveEnrollmentService passive_;
  mutable std::atomic<int> learning_{0};
};
