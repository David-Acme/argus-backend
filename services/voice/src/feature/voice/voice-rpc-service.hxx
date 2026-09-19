#pragma once

#include <argus/voice/v1/voice.grpc.pb.h>
#include <feature/voice/voice-session-service.hxx>
#include <grpcpp/grpcpp.h>

// argus.voice.v1.VoiceService: one bidi stream per app voice session.
class VoiceRpcService final
    : public argus::voice::v1::VoiceService::CallbackService
{
public:
  grpc::ServerBidiReactor<argus::voice::v1::ClientFrame,
                          argus::voice::v1::ServerFrame>*
  Connect(grpc::CallbackServerContext* context) override;

private:
  VoiceSessionService sessions_;
};
