#pragma once

#include <argus/voice/v1/voice.grpc.pb.h>
#include <feature/voice/voice-session-service.hxx>
#include <grpc/grpc-server-identity.hxx>
#include <grpcpp/grpcpp.h>
#include <string>
#include <vector>

class VoiceRpcService final
    : public argus::voice::v1::VoiceService::CallbackService
{
public:
  explicit VoiceRpcService(std::string syncCallerSecret);

  grpc::ServerBidiReactor<argus::voice::v1::ClientFrame,
                          argus::voice::v1::ServerFrame>*
  Connect(grpc::CallbackServerContext* context) override;

private:
  std::vector<argus::client::CallerCredential> callers_;
  VoiceSessionService sessions_;
};
