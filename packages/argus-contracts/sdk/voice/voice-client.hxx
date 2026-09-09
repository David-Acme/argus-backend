#pragma once

#include <argus/voice/v1/voice.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>

// Observer of one voice stream's server side; callbacks fire on gRPC threads.
class VoiceStreamObserver
{
public:
  virtual ~VoiceStreamObserver() = default;

  virtual void onServerFrame(argus::voice::v1::ServerFrame frame) = 0;
  virtual void onStreamClosed(const grpc::Status& status) = 0;
};

// One open bidirectional voice stream; control writes are thread-safe.
class VoiceStream
{
public:
  virtual ~VoiceStream() = default;

  virtual void start(const argus::voice::v1::VoiceIdentity& identity) = 0;
  virtual void stop() = 0;
  virtual void skip() = 0;
  virtual void sendPcm(const void* data, size_t size) = 0;
  virtual void finish() = 0;
};

// Thin SDK wrapper over argus.voice.v1.VoiceService (rule 23).
class VoiceClient
{
public:
  explicit VoiceClient(std::string target);

  VoiceClient(const VoiceClient&) = delete;
  VoiceClient& operator=(const VoiceClient&) = delete;
  virtual ~VoiceClient() = default;

  // Opens one bidi stream; the stream owns the observer until OnDone.
  virtual std::shared_ptr<VoiceStream>
  connect(const argus::voice::v1::VoiceIdentity& identity,
          std::shared_ptr<VoiceStreamObserver> observer);

  // Blocks up to timeoutMs for the channel to connect.
  virtual bool waitConnected(int timeoutMs) const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::voice::v1::VoiceService::StubInterface> stub_;
};

// VoiceRole → x-argus-role metadata string, mirroring userRoleToString.
std::string voiceRoleToString(argus::voice::v1::VoiceRole role);
