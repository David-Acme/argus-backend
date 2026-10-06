#pragma once

#include <feature/webrtc/infra/go2rtc-webrtc-gateway.hxx>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

struct WebRtcViewerLimits
{
  int perCamera{0};
  int total{0};
  int perUser{0};
};

struct WebRtcSeatRequest
{
  int64_t cameraId{0};
  int64_t userId{0};
  bool priority{false};
  int perCamera{0};
  int total{0};
  std::span<const Go2rtcWebRtcConsumer> consumers;
  WebRtcViewerLimits limits;
  std::chrono::steady_clock::time_point snapshotAt;
};

enum class WebRtcSeatRefusal : uint8_t
{
  None = 0,
  Total,
  Camera,
  User
};

struct WebRtcSeatVerdict
{
  uint64_t ticket{0};
  WebRtcSeatRefusal refusal{WebRtcSeatRefusal::None};
};

struct WebRtcSeatRelease
{
  uint64_t ticket{0};
  std::chrono::steady_clock::time_point at;
};

struct WebRtcTagOwner
{
  std::string tag;
  int64_t userId{0};
  std::chrono::steady_clock::time_point at;
};

struct WebRtcRoomCheck
{
  int used{0};
  int limit{0};
  bool priority{false};
};

class WebRtcAdmission
{
public:
  [[nodiscard]] WebRtcSeatVerdict reserve(const WebRtcSeatRequest& request);
  void release(const WebRtcSeatRelease& seat);
  void remember(const WebRtcTagOwner& owner);
  [[nodiscard]] std::vector<std::string> tagsOf(int64_t userId);

  [[nodiscard]] static bool hasRoom(const WebRtcRoomCheck& check);

private:
  struct Seat
  {
    uint64_t ticket{0};
    int64_t cameraId{0};
    int64_t userId{0};
    bool released{false};
    std::chrono::steady_clock::time_point releasedAt;
  };

  struct Owner
  {
    int64_t userId{0};
    std::chrono::steady_clock::time_point at;
  };

  void prune(const WebRtcSeatRequest& request);

  std::mutex mutex_;
  uint64_t nextTicket_{1};
  std::vector<Seat> seats_;
  std::unordered_map<std::string, Owner> owners_;
};
