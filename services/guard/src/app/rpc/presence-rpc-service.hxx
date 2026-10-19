#pragma once

#include <argus/guard/v1/presence.grpc.pb.h>
#include <grpc/grpc-server-identity.hxx>

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class PresenceService;

using DutyLookup = std::function<drogon::Task<std::vector<int64_t>>(int64_t)>;

struct CameraRoomRow
{
  int64_t cameraId{0};
  int64_t environmentId{0};
  std::string room;
  bool outdoor{false};
};

using CameraRoomLookup = std::function<drogon::Task<std::vector<CameraRoomRow>>()>;

struct VoiceEvidenceRow
{
  int64_t userId{0};
  int64_t cameraId{0};
  double snrDb{0.0};
  bool voiceprint{false};
  int64_t atMs{0};
};

using VoiceEvidenceSink = std::function<drogon::Task<bool>(VoiceEvidenceRow)>;

inline constexpr std::string_view kVoiceEvidenceCaller = "voice";

struct PresenceRpcInput
{
  const PresenceService* presence{nullptr};
  std::vector<argus::client::CallerCredential> credentials;
  DutyLookup duty{};
  CameraRoomLookup rooms{};
  VoiceEvidenceSink voice{};
};

class PresenceRpcService final
    : public argus::guard::v1::PresenceService::CallbackService
{
public:
  explicit PresenceRpcService(PresenceRpcInput input);

  grpc::ServerUnaryReactor*
  ListPresence(grpc::CallbackServerContext* context,
               const argus::guard::v1::ListPresenceRequest* request,
               argus::guard::v1::ListPresenceResponse* response) override;

  grpc::ServerUnaryReactor*
  GetDuty(grpc::CallbackServerContext* context,
          const argus::guard::v1::DutyRequest* request,
          argus::guard::v1::DutyResponse* response) override;

  grpc::ServerUnaryReactor*
  CameraRooms(grpc::CallbackServerContext* context,
              const argus::guard::v1::CameraRoomsRequest* request,
              argus::guard::v1::CameraRoomsResponse* response) override;

  grpc::ServerUnaryReactor*
  ReportVoiceEvidence(grpc::CallbackServerContext* context,
                      const argus::guard::v1::VoiceEvidenceRequest* request,
                      argus::guard::v1::VoiceEvidenceResponse* response) override;

private:
  PresenceRpcInput input_;
};
