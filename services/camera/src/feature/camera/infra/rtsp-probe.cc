#include "rtsp-probe.hxx"

#include <shared/services/tapo/tapo-crypto.hxx>
#include <shared/services/tapo/tapo-http.hxx>

#include <drogon/utils/Utilities.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <vector>

namespace
{
constexpr size_t kMaxSdpBytes = size_t{64} * 1024;

class BitReader
{
public:
  explicit BitReader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

  uint32_t bits(int count)
  {
    uint32_t value = 0;
    for (int i = 0; i < count; ++i)
      value = (value << 1U) | bit();
    return value;
  }

  uint32_t bit()
  {
    if (position_ >= bytes_.size() * 8) {
      overrun_ = true;
      return 0;
    }
    const uint8_t byte = bytes_[position_ / 8];
    const uint32_t value = (byte >> (7U - (position_ % 8))) & 1U;
    ++position_;
    return value;
  }

  uint32_t ue()
  {
    int zeros = 0;
    while (bit() == 0 && !overrun_ && zeros < 32)
      ++zeros;
    if (zeros == 0)
      return 0;
    return ((1U << static_cast<uint32_t>(zeros)) - 1U) + bits(zeros);
  }

  int32_t se()
  {
    const uint32_t code = ue();
    const auto magnitude = static_cast<int32_t>((code + 1U) / 2U);
    return (code % 2U) == 1U ? magnitude : -magnitude;
  }

  [[nodiscard]] bool overrun() const { return overrun_; }

private:
  std::span<const uint8_t> bytes_;
  size_t position_{0};
  bool overrun_{false};
};

std::vector<uint8_t> unescaped(std::span<const uint8_t> nal)
{
  std::vector<uint8_t> out;
  out.reserve(nal.size());
  int zeros = 0;
  for (const uint8_t byte : nal) {
    if (zeros >= 2 && byte == 0x03) {
      zeros = 0;
      continue;
    }
    zeros = byte == 0 ? zeros + 1 : 0;
    out.push_back(byte);
  }
  return out;
}

void skipScalingList(BitReader& reader, int size)
{
  int32_t last = 8;
  int32_t next = 8;
  for (int j = 0; j < size; ++j) {
    if (next != 0)
      next = (last + reader.se() + 256) % 256;
    last = next == 0 ? last : next;
  }
}

constexpr std::array<uint32_t, 12> kHighProfiles{100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134};

std::string lower(std::string value)
{
  std::ranges::transform(value, value.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

struct DescribeRequest
{
  const std::string& url;
  int cseq;
  const std::string& authorization;
};

std::string describeHead(const DescribeRequest& request)
{
  std::string head = "DESCRIBE " + request.url + " RTSP/1.0\r\n";
  head += "CSeq: " + std::to_string(request.cseq) + "\r\n";
  head += "Accept: application/sdp\r\n";
  head += "User-Agent: Argus\r\n";
  if (!request.authorization.empty())
    head += "Authorization: " + request.authorization + "\r\n";
  head += "\r\n";
  return head;
}

std::optional<int> statusOf(const std::string& line)
{
  if (!line.starts_with("RTSP/"))
    return std::nullopt;
  const size_t space = line.find(' ');
  if (space == std::string::npos)
    return std::nullopt;
  return static_cast<int>(std::strtol(line.c_str() + space + 1, nullptr, 10));
}

struct Exchange
{
  int status{0};
  std::vector<TapoHttpHeader> headers;
  std::string body;
  std::string error;
  bool rtsp{false};
};

Exchange roundTrip(TapoConnection& connection, const std::string& head)
{
  Exchange out;
  if (!connection.write(head)) {
    out.error = connection.error();
    return out;
  }
  std::string line;
  if (!connection.readLine(line)) {
    out.error = connection.error();
    return out;
  }
  const auto status = statusOf(line);
  if (!status) {
    out.error = "the port does not speak RTSP";
    return out;
  }
  out.rtsp = true;
  out.status = *status;
  for (;;) {
    if (!connection.readLine(line)) {
      out.error = connection.error();
      return out;
    }
    if (line.empty())
      break;
    const size_t colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    std::string value = line.substr(colon + 1);
    const size_t begin = value.find_first_not_of(" \t");
    out.headers.push_back(
        {.name = line.substr(0, colon), .value = begin == std::string::npos ? "" : value.substr(begin)});
  }
  for (const auto& header : out.headers) {
    if (lower(header.name) != "content-length")
      continue;
    const size_t length = std::strtoul(header.value.c_str(), nullptr, 10);
    if (length > 0 && length <= kMaxSdpBytes && !connection.readExactly(length, out.body))
      out.error = connection.error();
  }
  return out;
}

std::string headerIn(const Exchange& exchange, const std::string& name)
{
  const std::string needle = lower(name);
  for (const auto& header : exchange.headers) {
    if (lower(header.name) == needle)
      return header.value;
  }
  return {};
}

std::string basicAuthorization(const RtspProbeInput& input)
{
  return "Basic " + drogon::utils::base64Encode(input.username + ":" + input.password);
}

struct AuthorizationInput
{
  const RtspProbeInput& probe;
  const std::string& challenge;
  const std::string& url;
};

std::string authorizationFor(const AuthorizationInput& input)
{
  if (lower(input.challenge).starts_with("basic"))
    return basicAuthorization(input.probe);
  const auto parsed = tapo_crypto::parseDigestChallenge(input.challenge);
  if (parsed.nonce.empty())
    return {};
  return tapo_crypto::buildDigestHeader({.username = input.probe.username,
                                         .password = input.probe.password,
                                         .realm = parsed.realm,
                                         .nonce = parsed.nonce,
                                         .qop = parsed.qop.empty() ? "" : "auth",
                                         .opaque = parsed.opaque,
                                         .algorithm = parsed.algorithm,
                                         .method = "DESCRIBE",
                                         .uri = input.url,
                                         .cnonce = tapo_crypto::randomHex(8),
                                         .nonceCount = 1});
}

std::string firstSprop(std::string_view fmtp)
{
  const std::string_view key = "sprop-parameter-sets=";
  const size_t at = fmtp.find(key);
  if (at == std::string_view::npos)
    return {};
  const size_t begin = at + key.size();
  const size_t end = fmtp.find_first_of(",; \r\n", begin);
  return std::string(fmtp.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin));
}

struct ProbeFailure
{
  RtspProbeOutcome outcome;
  std::string detail;
  int status{0};
};

RtspProbeResult failure(const ProbeFailure& input)
{
  return {.outcome = input.outcome,
          .status = input.status,
          .detail = input.detail,
          .videoCodec = {},
          .audioCodec = {},
          .width = 0,
          .height = 0,
          .fps = 0.0};
}
}

std::string_view rtspProbeOutcomeToString(RtspProbeOutcome outcome)
{
  switch (outcome) {
    case RtspProbeOutcome::Ok:
      return "ok";
    case RtspProbeOutcome::Unreachable:
      return "unreachable";
    case RtspProbeOutcome::Refused:
      return "refused";
    case RtspProbeOutcome::AuthFailed:
      return "auth_failed";
    case RtspProbeOutcome::NotFound:
      return "not_found";
    case RtspProbeOutcome::NoVideo:
      return "no_video";
    case RtspProbeOutcome::Protocol:
      return "protocol";
  }
  return "protocol";
}

std::string rtsp_probe::urlHost(const std::string& host)
{
  return host.find(':') == std::string::npos ? host : "[" + host + "]";
}

VideoSize rtsp_probe::h264Size(std::span<const uint8_t> sps)
{
  if (sps.size() < 4 || (sps[0] & 0x1FU) != 7)
    return {};
  const auto rbsp = unescaped(sps.subspan(1));
  BitReader reader(rbsp);
  const uint32_t profile = reader.bits(8);
  reader.bits(16);
  reader.ue();
  uint32_t chroma = 1;
  if (std::ranges::find(kHighProfiles, profile) != kHighProfiles.end()) {
    chroma = reader.ue();
    if (chroma == 3)
      reader.bit();
    reader.ue();
    reader.ue();
    reader.bit();
    if (reader.bit() == 1) {
      const int lists = chroma == 3 ? 12 : 8;
      for (int i = 0; i < lists; ++i) {
        if (reader.bit() == 1)
          skipScalingList(reader, i < 6 ? 16 : 64);
      }
    }
  }
  reader.ue();
  const uint32_t pocType = reader.ue();
  if (pocType == 0) {
    reader.ue();
  }
  else if (pocType == 1) {
    reader.bit();
    reader.se();
    reader.se();
    const uint32_t cycle = reader.ue();
    for (uint32_t i = 0; i < cycle && i < 256; ++i)
      reader.se();
  }
  reader.ue();
  reader.bit();
  const uint32_t widthMbs = reader.ue() + 1;
  const uint32_t heightUnits = reader.ue() + 1;
  const uint32_t frameMbsOnly = reader.bit();
  if (frameMbsOnly == 0)
    reader.bit();
  reader.bit();
  uint32_t cropLeft = 0;
  uint32_t cropRight = 0;
  uint32_t cropTop = 0;
  uint32_t cropBottom = 0;
  if (reader.bit() == 1) {
    cropLeft = reader.ue();
    cropRight = reader.ue();
    cropTop = reader.ue();
    cropBottom = reader.ue();
  }
  if (reader.overrun())
    return {};
  double fps = 0.0;
  if (reader.bit() == 1) {
    if (reader.bit() == 1 && reader.bits(8) == 255)
      reader.bits(32);
    if (reader.bit() == 1)
      reader.bit();
    if (reader.bit() == 1) {
      reader.bits(4);
      if (reader.bit() == 1)
        reader.bits(24);
    }
    if (reader.bit() == 1) {
      reader.ue();
      reader.ue();
    }
    if (reader.bit() == 1) {
      const uint32_t unitsInTick = reader.bits(32);
      const uint32_t timeScale = reader.bits(32);
      if (unitsInTick > 0 && !reader.overrun())
        fps = static_cast<double>(timeScale) / (2.0 * unitsInTick);
    }
  }
  const uint32_t cropUnitX = chroma == 0 || chroma == 3 ? 1 : 2;
  const uint32_t cropUnitY = (chroma == 1 ? 2 : 1) * (2 - frameMbsOnly);
  const auto width = static_cast<int64_t>(widthMbs * 16) - static_cast<int64_t>((cropLeft + cropRight) * cropUnitX);
  const auto height = static_cast<int64_t>((2 - frameMbsOnly) * heightUnits * 16) -
                      static_cast<int64_t>((cropTop + cropBottom) * cropUnitY);
  if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
    return {};
  return {.width = static_cast<int>(width),
          .height = static_cast<int>(height),
          .fps = fps > 0.0 && fps <= 240.0 ? fps : 0.0};
}

RtspProbeResult rtsp_probe::readSdp(std::string_view sdp)
{
  RtspProbeResult result = failure({.outcome = RtspProbeOutcome::NoVideo, .detail = "the stream carries no video", .status = 0});
  std::string media;
  std::string videoPayload;
  size_t begin = 0;
  while (begin < sdp.size()) {
    size_t end = sdp.find('\n', begin);
    if (end == std::string_view::npos)
      end = sdp.size();
    std::string_view line = sdp.substr(begin, end - begin);
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    begin = end + 1;
    if (line.starts_with("m=")) {
      media = std::string(line.substr(2, line.find(' ') == std::string_view::npos ? 0 : line.find(' ') - 2));
      continue;
    }
    if (line.starts_with("a=rtpmap:")) {
      const size_t space = line.find(' ');
      if (space == std::string_view::npos)
        continue;
      const std::string_view encoding = line.substr(space + 1, line.find('/', space) - space - 1);
      if (media == "video" && result.videoCodec.empty()) {
        result.videoCodec = std::string(encoding);
        videoPayload = std::string(line.substr(9, space - 9));
      }
      else if (media == "audio" && result.audioCodec.empty()) {
        result.audioCodec = std::string(encoding);
      }
      continue;
    }
    if (media == "video" && line.starts_with("a=fmtp:") && result.width == 0) {
      const std::string sprop = firstSprop(line);
      if (sprop.empty())
        continue;
      const std::string nal = drogon::utils::base64Decode(sprop);
      const auto size = h264Size(std::span(reinterpret_cast<const uint8_t*>(nal.data()), nal.size()));
      result.width = size.width;
      result.height = size.height;
      result.fps = size.fps;
    }
  }
  if (!result.videoCodec.empty()) {
    result.outcome = RtspProbeOutcome::Ok;
    result.detail.clear();
  }
  return result;
}

RtspProbeResult rtsp_probe::describe(const RtspProbeInput& input)
{
  const std::string url = "rtsp://" + urlHost(input.host) + ":" + std::to_string(input.port) +
                          (input.path.empty() ? "/" : input.path);
  TapoConnection connection;
  if (!connection.open({.host = input.host,
                        .port = input.port,
                        .tls = false,
                        .connectTimeoutMs = input.timeoutMs,
                        .ioTimeoutMs = input.timeoutMs})) {
    const std::string& error = connection.error();
    return failure({.outcome = error.starts_with("connection refused") ? RtspProbeOutcome::Refused
                                                                       : RtspProbeOutcome::Unreachable,
                    .detail = error,
                    .status = 0});
  }

  Exchange answer = roundTrip(connection, describeHead({.url = url, .cseq = 1, .authorization = {}}));
  if (!answer.rtsp)
    return failure({.outcome = RtspProbeOutcome::Protocol, .detail = answer.error, .status = 0});
  if (answer.status == 401) {
    const std::string challenge = headerIn(answer, "WWW-Authenticate");
    const std::string authorization =
        input.username.empty() && input.password.empty()
            ? std::string()
            : authorizationFor({.probe = input, .challenge = challenge, .url = url});
    if (authorization.empty())
      return failure({.outcome = RtspProbeOutcome::AuthFailed, .detail = "the camera asks for a user and password", .status = 401});
    const std::string authorized = describeHead({.url = url, .cseq = 2, .authorization = authorization});
    answer = roundTrip(connection, authorized);
    if (!answer.rtsp && connection.open({.host = input.host,
                                         .port = input.port,
                                         .tls = false,
                                         .connectTimeoutMs = input.timeoutMs,
                                         .ioTimeoutMs = input.timeoutMs}))
      answer = roundTrip(connection, authorized);
    if (!answer.rtsp)
      return failure({.outcome = RtspProbeOutcome::Protocol, .detail = answer.error, .status = 0});
    if (answer.status == 401 || answer.status == 403)
      return failure({.outcome = RtspProbeOutcome::AuthFailed, .detail = "the camera refused the user and password", .status = answer.status});
  }
  if (answer.status == 404 || answer.status == 454 || answer.status == 400)
    return failure({.outcome = RtspProbeOutcome::NotFound, .detail = "the camera has no stream at " + input.path, .status = answer.status});
  if (answer.status != 200)
    return failure({.outcome = RtspProbeOutcome::Protocol,
                    .detail = "the camera answered RTSP " + std::to_string(answer.status),
                    .status = answer.status});
  auto result = readSdp(answer.body);
  result.status = answer.status;
  return result;
}
