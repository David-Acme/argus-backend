#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <optional>
#include <string>
#include <vector>

struct LiveKitAdminConfig
{
  std::string serverUrl;
  std::string apiKey;
  std::string apiSecret;
};

struct LiveKitParticipantRef
{
  std::string room;
  std::string identity;
};

class LiveKitRoomClient
{
public:
  explicit LiveKitRoomClient(LiveKitAdminConfig config);
  LiveKitRoomClient(const LiveKitRoomClient&) = delete;
  LiveKitRoomClient& operator=(const LiveKitRoomClient&) = delete;
  LiveKitRoomClient(LiveKitRoomClient&&) = delete;
  LiveKitRoomClient& operator=(LiveKitRoomClient&&) = delete;
  virtual ~LiveKitRoomClient() = default;

  virtual drogon::Task<std::optional<std::vector<std::string>>> listRooms() const;
  virtual drogon::Task<bool> removeParticipant(LiveKitParticipantRef participant) const;
  virtual drogon::Task<bool> deleteRoom(std::string room) const;
  virtual drogon::Task<std::optional<std::vector<std::string>>> listParticipants(std::string room) const;
  virtual drogon::Task<bool> silenceParticipant(LiveKitParticipantRef participant) const;

  static constexpr double kRequestTimeoutSeconds = 3.0;

private:
  struct TwirpCall
  {
    std::string method;
    Json::Value body;
    std::string room;
    bool list{false};
    bool create{false};
  };

  drogon::Task<std::optional<Json::Value>> call(TwirpCall twirp) const;

  LiveKitAdminConfig config_;
};
