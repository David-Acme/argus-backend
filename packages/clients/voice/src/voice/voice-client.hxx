#pragma once

#include <argus/voice/v1/voice.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <auth/user-role.hxx>

class VoiceStreamObserver
{
public:
  virtual ~VoiceStreamObserver() = default;

  virtual void onServerFrame(argus::voice::v1::ServerFrame frame) = 0;
  virtual void onStreamClosed(const grpc::Status& status) = 0;
};

class VoiceStream
{
public:
  virtual ~VoiceStream() = default;

  virtual void start(const argus::voice::v1::VoiceStart& start) = 0;
  virtual void stop() = 0;
  virtual void skip() = 0;
  virtual void sendPcm(const void* data, size_t size) = 0;
  virtual void sendContext(const argus::voice::v1::VoiceContext& context) = 0;
  virtual void sendActionResult(const argus::voice::v1::VoiceActionResult& result) = 0;
  virtual void sendMute(bool muted) = 0;
  virtual void finish() = 0;
};

struct VoiceRoomJoinResult
{
  bool joined{false};
  bool already{false};
  grpc::Status status;
};

struct VoiceAnnounceInput
{
  int64_t userId{0};
  std::string text;
  std::string kind;
  std::string callId;
};

struct VoiceClientConfig
{
  std::string target;
  std::string credential;
};

class VoiceClient
{
public:
  explicit VoiceClient(VoiceClientConfig config);

  VoiceClient(const VoiceClient&) = delete;
  VoiceClient& operator=(const VoiceClient&) = delete;
  virtual ~VoiceClient() = default;

  virtual std::shared_ptr<VoiceStream> connect(
      const argus::voice::v1::VoiceIdentity& identity,
      std::shared_ptr<VoiceStreamObserver> observer);

  [[nodiscard]] virtual bool waitConnected(int timeoutMs) const;

  [[nodiscard]] virtual VoiceRoomJoinResult joinRoom(const argus::voice::v1::RtcJoin& join) const;

  [[nodiscard]] virtual std::optional<bool> announce(const VoiceAnnounceInput& input) const;

  static constexpr int kJoinRoomDeadlineMs = 6000;
  static constexpr int kAnnounceDeadlineMs = 1500;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::voice::v1::VoiceService::StubInterface> stub_;
  std::string credential_;
};

std::string voiceRoleToString(argus::voice::v1::VoiceRole role);

argus::voice::v1::VoiceRole voiceRoleToProto(UserRole role);

argus::voice::v1::VoiceLanguage voiceLanguageToProto(std::string_view lang);
