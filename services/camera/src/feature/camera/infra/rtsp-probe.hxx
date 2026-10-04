#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

enum class RtspProbeOutcome : uint8_t
{
  Ok = 0,
  Unreachable,
  Refused,
  AuthFailed,
  NotFound,
  NoVideo,
  Protocol
};

std::string_view rtspProbeOutcomeToString(RtspProbeOutcome outcome);

struct RtspProbeInput
{
  std::string host;
  int port{554};
  std::string username;
  std::string password;
  std::string path;
  int timeoutMs{4000};
};

struct RtspProbeResult
{
  RtspProbeOutcome outcome{RtspProbeOutcome::Protocol};
  int status{0};
  std::string detail;
  std::string videoCodec;
  std::string audioCodec;
  int width{0};
  int height{0};
  double fps{0.0};
};

struct VideoSize
{
  int width{0};
  int height{0};
  double fps{0.0};
};

namespace rtsp_probe
{
RtspProbeResult describe(const RtspProbeInput& input);
RtspProbeResult readSdp(std::string_view sdp);
VideoSize h264Size(std::span<const uint8_t> sps);
std::string urlHost(const std::string& host);
}
