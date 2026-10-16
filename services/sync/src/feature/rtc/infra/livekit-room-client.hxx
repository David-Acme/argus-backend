#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <cstdint>
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

enum class RoomCloseStatus : uint8_t
{
  Closed,
  Gone,
  Failed
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
  virtual drogon::Task<bool> createRoom(std::string room) const;
  virtual drogon::Task<bool> deleteRoom(std::string room) const;
  virtual drogon::Task<RoomCloseStatus> closeRoom(std::string room) const;
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

  enum class TwirpOutcome : uint8_t
  {
    Ok,
    NotFound,
    Failed
  };

  struct TwirpReply
  {
    TwirpOutcome outcome{TwirpOutcome::Failed};
    Json::Value body;
  };

  drogon::Task<TwirpReply> answer(TwirpCall twirp) const;
  drogon::Task<std::optional<Json::Value>> call(TwirpCall twirp) const;

  LiveKitAdminConfig config_;
};
