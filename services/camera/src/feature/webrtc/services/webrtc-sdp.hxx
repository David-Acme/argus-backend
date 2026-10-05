#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct WebRtcOfferInput
{
  std::string_view sdp;
  bool audio{false};
};

struct WebRtcViewer
{
  int64_t userId{0};
  std::string sessionId;
};

namespace webrtc_sdp
{
std::optional<std::string> prepareOffer(const WebRtcOfferInput& input);
std::string separateAudio(std::string_view answer);
}

namespace webrtc_viewer
{
std::string tagOf(const WebRtcViewer& viewer);
bool isTagged(std::string_view userAgent);
}
