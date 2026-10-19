#pragma once

#include <argus/guard/v1/presence.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct VoiceEvidenceReport
{
  int64_t userId{0};
  int64_t cameraId{0};
  float snrDb{0.0F};
  bool voiceprint{false};
  int64_t atMs{0};
};

struct GuardPresenceClientConfig
{
  std::string target;
  std::string credential;
};

class GuardPresenceClient
{
public:
  static constexpr int kCallTimeoutMs = 1500;

  explicit GuardPresenceClient(GuardPresenceClientConfig config);

  GuardPresenceClient(const GuardPresenceClient&) = delete;
  GuardPresenceClient& operator=(const GuardPresenceClient&) = delete;
  virtual ~GuardPresenceClient() = default;

  [[nodiscard]] virtual std::optional<argus::guard::v1::ListPresenceResponse>
  listPresence(const std::vector<int64_t>& userIds) const;

  [[nodiscard]] virtual std::optional<bool> onDuty(int64_t userId) const;

  [[nodiscard]] virtual std::optional<std::vector<argus::guard::v1::CameraRoom>> cameraRooms() const;

  [[nodiscard]] virtual bool reportVoice(const VoiceEvidenceReport& report) const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::guard::v1::PresenceService::StubInterface> stub_;
  std::string credential_;
};
