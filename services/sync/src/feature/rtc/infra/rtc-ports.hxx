#pragma once

#include <argus/voice/v1/voice.pb.h>
#include <drogon/utils/coroutine.h>

#include <chrono>
#include <cstdint>
#include <string>

enum class RtcClaimStatus : uint8_t
{
  Claimed,
  Taken,
  Expired,
  NotFound,
  Unavailable
};

struct RtcClaimInput
{
  std::string callId;
  int64_t userId{0};
  std::string sessionId;
};

struct RtcClaim
{
  RtcClaimStatus status{RtcClaimStatus::Unavailable};
  std::string openingLine;
  std::string lang;
  std::string kind;
  std::string summary;
  int64_t cameraId{0};
  std::string cameraName;
  int64_t episodeId{0};
};

class RtcCallClaimer
{
public:
  virtual ~RtcCallClaimer() = default;
  virtual drogon::Task<RtcClaim> claim(RtcClaimInput input) const = 0;
};

struct RtcFarewellInput
{
  argus::voice::v1::RtcFarewell request;
  std::chrono::milliseconds deadline{0};
};

class RtcVoiceJoiner
{
public:
  virtual ~RtcVoiceJoiner() = default;
  virtual drogon::Task<bool> join(argus::voice::v1::RtcJoin join) const = 0;
  virtual drogon::Task<bool> farewell(RtcFarewellInput input) const = 0;
};
