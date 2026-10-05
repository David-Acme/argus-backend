#pragma once

#include <argus/voice/v1/voice.pb.h>
#include <feature/rtc/rtc-call.hxx>
#include <feature/voice/voice-rpc-service.hxx>
#include <feature/voice/voice-session-service.hxx>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct RtcAgentConfig
{
  std::string url;
  RtcCallTimings timings;
};

struct RtcAgentInput
{
  VoiceSessionService* sessions{nullptr};
  RtcAgentConfig config;
  std::function<void(const RtcCallReport&)> onCallEnded;
};

struct RtcJoinOutcome
{
  bool joined{false};
  bool already{false};
};

enum class RtcJoinRefusal : uint8_t
{
  None,
  Invalid
};

class RtcAgentService final : public VoiceRoomJoiner
{
public:
  explicit RtcAgentService(RtcAgentInput input);
  RtcAgentService(const RtcAgentService&) = delete;
  RtcAgentService& operator=(const RtcAgentService&) = delete;
  RtcAgentService(RtcAgentService&&) = delete;
  RtcAgentService& operator=(RtcAgentService&&) = delete;
  ~RtcAgentService() override;

  [[nodiscard]] static RtcJoinRefusal validate(const argus::voice::v1::RtcJoin& join);
  void joinCall(const argus::voice::v1::RtcJoin& join, const std::function<void(RtcJoinOutcome)>& done);
  void joinRoom(const argus::voice::v1::RtcJoin& join,
                std::function<void(grpc::Status, argus::voice::v1::RtcJoined)> done) override;
  void farewellRoom(const argus::voice::v1::RtcFarewell& farewell, std::function<void(bool)> done) override;
  [[nodiscard]] size_t activeCalls() const;
  void shutdown();

private:
  void reapLocked(std::vector<std::shared_ptr<RtcCall>>& retired);

  VoiceSessionService& sessions_;
  RtcAgentConfig config_;
  std::function<void(const RtcCallReport&)> onCallEnded_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<RtcCall>> calls_;
  bool closed_{false};
};
