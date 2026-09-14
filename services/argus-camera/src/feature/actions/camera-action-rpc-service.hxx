#pragma once

#include <argus/camera/v1/actions.grpc.pb.h>
#include <cstdint>
#include <feature/actions/stt-transcriber.hxx>
#include <feature/api/camera-control/services/camera-control-feature-service.hxx>
#include <grpc-server-identity.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <shared/repositories/action-command/action-command-repository.hxx>
#include <shared/repositories/camera/camera-repository.hxx>
#include <string>
#include <vector>

using CameraCommandOutcome = argus::camera::v1::CommandOutcome;

// Composition input for the action surface; the transcriber is owned here.
struct CameraActionServiceConfig
{
  std::vector<argus::sdk::CallerCredential> callers;
  std::unique_ptr<SttTranscriber> transcriber;
};

// argus.camera.v1.CameraActionService: capability-gated audible/device actions.
class CameraActionRpcService final
    : public argus::camera::v1::CameraActionService::CallbackService
{
public:
  explicit CameraActionRpcService(CameraActionServiceConfig config);

  grpc::ServerUnaryReactor*
  Announce(grpc::CallbackServerContext* context,
           const argus::camera::v1::AnnounceRequest* request,
           argus::camera::v1::ActionAck* response) override;

  grpc::ServerUnaryReactor*
  Alarm(grpc::CallbackServerContext* context,
        const argus::camera::v1::AlarmRequest* request,
        argus::camera::v1::ActionAck* response) override;

  grpc::ServerUnaryReactor*
  SetSiren(grpc::CallbackServerContext* context,
           const argus::camera::v1::SirenRequest* request,
           argus::camera::v1::ActionAck* response) override;

  grpc::ServerUnaryReactor*
  GetPersonCrop(grpc::CallbackServerContext* context,
                const argus::camera::v1::PersonCropRequest* request,
                argus::camera::v1::PersonCropResponse* response) override;

  grpc::ServerUnaryReactor*
  Listen(grpc::CallbackServerContext* context,
         const argus::camera::v1::ListenRequest* request,
         argus::camera::v1::ListenResponse* response) override;

  // Boot-only additive migration of the action_command table; true on success.
  bool migrateActionSchema();

  // Disarms cameras whose siren lease expired and re-reconciles lost claims.
  void startLeaseSweeper();

private:
  struct CommandContext
  {
    std::string commandId;
    std::string kind;
    int64_t cameraId{0};
    int64_t encounterId{0};
    int64_t expiresAt{0};
    int64_t leaseSeconds{120};
    std::string fingerprint;
  };

  struct CommandVerdict
  {
    CameraCommandOutcome outcome{
        CameraCommandOutcome::COMMAND_OUTCOME_UNSPECIFIED};
    bool proceed{false};
    int64_t generation{0};
    std::string detail;
    std::string response;
  };

  struct CommandSettleInput
  {
    std::string commandId;
    int64_t generation{0};
    std::string status;
    std::string detail;
    std::string response;
  };

  // The durable authoritative outcome read after a settle loses its fence.
  struct SettleVerdict
  {
    bool won{false};
    CameraCommandOutcome outcome{
        CameraCommandOutcome::COMMAND_OUTCOME_UNSPECIFIED};
    std::string detail;
    std::string response;
  };

  bool authorized(const grpc::CallbackServerContext* context) const;

  bool actionsEnabled() const;

  drogon::Task<CommandVerdict> beginCommand(const CommandContext& input);

  drogon::Task<bool> settleCommand(const CommandSettleInput& input);

  drogon::Task<SettleVerdict> settleFenced(const CommandSettleInput& input);

  std::vector<argus::sdk::CallerCredential> callers_;
  std::unique_ptr<SttTranscriber> transcriber_;

  ActionCommandRepository commandRepository_;
  CameraRepository cameraRepository_;
  CameraControlFeatureService cameraControlService_;
};
