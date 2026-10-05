#include "webrtc-sdp.hxx"

#include <shared/utils/network-address/private-address.hxx>

#include <drogon/utils/Utilities.h>

#include <algorithm>
#include <array>
#include <vector>

namespace
{
constexpr std::size_t kMaxSections = 8;
constexpr std::size_t kTagDigestChars = 32;
constexpr std::string_view kTagPrefix = "argus-camera/webrtc ";
constexpr std::string_view kAudioStreamSuffix = "-audio";
constexpr std::array<std::string_view, 4> kDirections{"a=sendrecv", "a=sendonly", "a=recvonly",
                                                      "a=inactive"};

enum class SectionKind : uint8_t
{
  Session = 0,
  Video,
  Audio,
  Other
};

struct Section
{
  SectionKind kind{SectionKind::Session};
  std::vector<std::string_view> lines;
};

std::string_view trimmed(std::string_view line)
{
  while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
    line.remove_suffix(1);
  return line;
}

SectionKind kindOf(std::string_view mediaLine)
{
  if (mediaLine.starts_with("m=video "))
    return SectionKind::Video;
  if (mediaLine.starts_with("m=audio "))
    return SectionKind::Audio;
  return SectionKind::Other;
}

bool isDirection(std::string_view line)
{
  return std::ranges::find(kDirections, line) != kDirections.end();
}

std::optional<std::vector<Section>> sectionsOf(std::string_view sdp)
{
  std::vector<Section> sections(1);
  while (!sdp.empty()) {
    const auto newline = sdp.find('\n');
    const std::string_view line = trimmed(sdp.substr(0, newline));
    sdp.remove_prefix(newline == std::string_view::npos ? sdp.size() : newline + 1);
    if (line.empty())
      continue;
    if (line.size() < 2 || line[1] != '=')
      return std::nullopt;
    if (line.starts_with("m=")) {
      if (sections.size() > kMaxSections)
        return std::nullopt;
      sections.push_back({.kind = kindOf(line), .lines = {}});
    }
    sections.back().lines.push_back(line);
  }
  return sections;
}

std::string withAudioStream(std::string_view line)
{
  constexpr std::string_view kMsid = "msid:";
  constexpr std::string_view kLabel = "mslabel:";
  if (const auto at = line.find(kMsid); at != std::string_view::npos && line.starts_with("a=")) {
    const auto stream = at + kMsid.size();
    const auto end = line.find(' ', stream);
    if (end != std::string_view::npos)
      return std::string(line.substr(0, end)).append(kAudioStreamSuffix).append(line.substr(end));
  }
  if (const auto at = line.find(kLabel); at != std::string_view::npos && line.starts_with("a=ssrc:"))
    return std::string(line).append(kAudioStreamSuffix);
  return std::string(line);
}

std::string_view fieldOf(std::string_view line, std::size_t index)
{
  std::size_t position = 0;
  for (std::size_t field = 0; field <= index; ++field) {
    while (position < line.size() && line[position] == ' ')
      ++position;
    const auto end = line.find(' ', position);
    if (field == index)
      return line.substr(position, end == std::string_view::npos ? std::string_view::npos
                                                                 : end - position);
    if (end == std::string_view::npos)
      return {};
    position = end;
  }
  return {};
}

bool keepsCandidate(std::string_view line, std::span<const std::string> allowedHosts)
{
  const std::string address(fieldOf(line, 4));
  if (!network_address::isLiteral(address) || network_address::isHostLocal(address))
    return false;
  return allowedHosts.empty() || std::ranges::find(allowedHosts, address) != allowedHosts.end();
}

std::string_view directionFor(SectionKind kind, bool audio)
{
  if (kind == SectionKind::Audio && !audio)
    return "a=inactive";
  return "a=recvonly";
}
}

namespace webrtc_sdp
{
std::optional<std::string> prepareOffer(const WebRtcOfferInput& input)
{
  if (!input.sdp.starts_with("v=0"))
    return std::nullopt;
  const auto sections = sectionsOf(input.sdp);
  if (!sections)
    return std::nullopt;
  const bool asksVideo = std::ranges::any_of(
      *sections, [](const Section& section) { return section.kind == SectionKind::Video; });
  if (!asksVideo)
    return std::nullopt;

  std::string out;
  out.reserve(input.sdp.size() + 64);
  for (const Section& section : *sections) {
    const bool media =
        section.kind == SectionKind::Video || section.kind == SectionKind::Audio;
    for (const std::string_view line : section.lines) {
      if ((media || section.kind == SectionKind::Session) && isDirection(line))
        continue;
      if (isCandidate(line))
        continue;
      out.append(line).append("\r\n");
    }
    if (media)
      out.append(directionFor(section.kind, input.audio)).append("\r\n");
  }
  return out;
}

bool isCandidate(std::string_view line)
{
  return line.starts_with("a=candidate:") || line == "a=end-of-candidates";
}

std::string screenCandidates(const WebRtcAnswerScreen& screen)
{
  std::string out;
  out.reserve(screen.sdp.size());
  std::string_view sdp = screen.sdp;
  while (!sdp.empty()) {
    const auto newline = sdp.find('\n');
    const std::string_view line = trimmed(sdp.substr(0, newline));
    sdp.remove_prefix(newline == std::string_view::npos ? sdp.size() : newline + 1);
    if (line.empty())
      continue;
    if (line.starts_with("a=candidate:") && !keepsCandidate(line, screen.allowedHosts))
      continue;
    out.append(line).append("\r\n");
  }
  return out;
}

std::string separateAudio(std::string_view answer)
{
  const auto sections = sectionsOf(answer);
  if (!sections)
    return std::string(answer);
  std::string out;
  out.reserve(answer.size() + 64);
  for (const Section& section : *sections) {
    for (const std::string_view line : section.lines) {
      if (section.kind == SectionKind::Audio)
        out.append(withAudioStream(line));
      else
        out.append(line);
      out.append("\r\n");
    }
  }
  return out;
}
}

namespace webrtc_viewer
{
std::string tagOf(const WebRtcViewer& viewer)
{
  const std::string digest =
      drogon::utils::getSha256(std::to_string(viewer.userId) + ":" + viewer.sessionId);
  return std::string(kTagPrefix) + digest.substr(0, kTagDigestChars);
}

bool isTagged(std::string_view userAgent)
{
  return userAgent.starts_with(kTagPrefix);
}
}
