#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>

#include <errors/validation-exception.hxx>
#include <feature/webrtc/dtos/camera-webrtc-offer-dto.hxx>
#include <feature/webrtc/dtos/response-camera-webrtc-dto.hxx>
#include <feature/webrtc/infra/go2rtc-webrtc-gateway.hxx>
#include <feature/webrtc/services/camera-webrtc-service.hxx>
#include <feature/webrtc/services/webrtc-admission.hxx>
#include <feature/webrtc/services/webrtc-sdp.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>

#include <chrono>
#include <json/reader.h>
#include <memory>
#include <json/value.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{
constexpr std::string_view kBrowserOffer =
    "v=0\r\n"
    "o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0 1\r\n"
    "a=sendrecv\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=recvonly\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111 8\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:1\r\n"
    "a=sendrecv\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=rtpmap:8 PCMA/8000\r\n"
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
    "a=mid:2\r\n"
    "a=sendrecv\r\n";

std::vector<std::string> sectionLines(const std::string& sdp, std::string_view media)
{
  std::vector<std::string> lines;
  bool inside = false;
  std::size_t start = 0;
  while (start < sdp.size()) {
    const auto end = sdp.find("\r\n", start);
    const std::string line = sdp.substr(start, end - start);
    start = end == std::string::npos ? sdp.size() : end + 2;
    if (line.starts_with("m="))
      inside = line.starts_with(media);
    if (inside)
      lines.push_back(line);
  }
  return lines;
}

int count(const std::vector<std::string>& lines, std::string_view line)
{
  int matches = 0;
  for (const auto& candidate : lines)
    matches += candidate == line ? 1 : 0;
  return matches;
}

Json::Value parse(std::string_view text)
{
  Json::Value value;
  Json::CharReaderBuilder builder;
  std::string errors;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  reader->parse(text.data(), text.data() + text.size(), &value, &errors);
  return value;
}

constexpr std::string_view kStreams = R"({
  "cam6": {
    "producers": [{"medias": ["video, recvonly, H264", "audio, recvonly, PCMA/8000"]}],
    "consumers": [
      {"format_name": "mp4", "user_agent": "argus"},
      {"format_name": "webrtc/whep", "user_agent": "argus-camera/webrtc aaaa"}
    ]
  },
  "cam6-sub": {
    "producers": [{"medias": ["video, recvonly, H264", "audio, recvonly, PCMA/8000"]}],
    "consumers": [{"format_name": "webrtc/whep", "user_agent": "argus-camera/webrtc bbbb"}]
  },
  "cam7-sub-opus": {
    "consumers": [{"format_name": "webrtc/whep", "user_agent": "argus-camera/webrtc cccc"}]
  },
  "cam9-sub": {
    "producers": [{"medias": ["video, recvonly, H264", "audio, recvonly, MPEG4-GENERIC/16000/1"]}]
  }
})";
}

TEST_CASE("an offer is rewritten to receive only, and audio follows the household's choice")
{
  const std::string allowed =
      webrtc_sdp::prepareOffer({.sdp = kBrowserOffer, .audio = true}).value_or("");
  REQUIRE_FALSE(allowed.empty());
  const auto video = sectionLines(allowed, "m=video");
  const auto audio = sectionLines(allowed, "m=audio");
  CHECK(count(video, "a=recvonly") == 1);
  CHECK(count(video, "a=mid:0") == 1);
  CHECK(count(audio, "a=recvonly") == 1);
  CHECK(count(audio, "a=sendrecv") == 0);
  CHECK(count(sectionLines(allowed, "m=application"), "a=sendrecv") == 1);
  CHECK(allowed.find("a=group:BUNDLE 0 1\r\n") != std::string::npos);
  CHECK(allowed.substr(0, allowed.find("m=")).find("a=sendrecv") == std::string::npos);
  CHECK(allowed.ends_with("\r\n"));

  const std::string withheld =
      webrtc_sdp::prepareOffer({.sdp = kBrowserOffer, .audio = false}).value_or("");
  REQUIRE_FALSE(withheld.empty());
  const auto silent = sectionLines(withheld, "m=audio");
  CHECK(count(silent, "a=inactive") == 1);
  CHECK(count(silent, "a=recvonly") == 0);
  CHECK(count(sectionLines(withheld, "m=video"), "a=recvonly") == 1);
}

TEST_CASE("an offer that would publish into a camera stream is turned into a viewer")
{
  const std::string publisher =
      "v=0\nm=video 9 UDP/TLS/RTP/SAVPF 96\na=mid:0\na=sendonly\na=rtpmap:96 H264/90000\n";
  const std::string prepared = webrtc_sdp::prepareOffer({.sdp = publisher, .audio = true}).value_or("");
  REQUIRE_FALSE(prepared.empty());
  const auto video = sectionLines(prepared, "m=video");
  CHECK(count(video, "a=sendonly") == 0);
  CHECK(count(video, "a=recvonly") == 1);
}

TEST_CASE("an offer without a video section, or that is not SDP, is refused")
{
  CHECK_FALSE(webrtc_sdp::prepareOffer({.sdp = "v=0\r\nm=audio 9 RTP/AVP 8\r\na=recvonly\r\n", .audio = true}));
  CHECK_FALSE(webrtc_sdp::prepareOffer({.sdp = "hello", .audio = true}));
  CHECK_FALSE(webrtc_sdp::prepareOffer({.sdp = "v=0\r\nm=video 9 RTP/AVP 96\r\nnot an sdp line\r\n", .audio = true}));
  std::string crowded = "v=0\r\n";
  for (int section = 0; section < 9; ++section)
    crowded += "m=video 9 RTP/AVP 96\r\na=recvonly\r\n";
  CHECK_FALSE(webrtc_sdp::prepareOffer({.sdp = crowded, .audio = true}));
}

TEST_CASE("the answer gives the camera's audio its own stream, so the browser does not hold the picture for lip sync")
{
  const std::string answer =
      "v=0\r\n"
      "a=msid-semantic:WMS *\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
      "a=ssrc:1 msid:go2rtc go2rtc-video\r\n"
      "a=ssrc:1 mslabel:go2rtc\r\n"
      "a=msid:go2rtc go2rtc-video\r\n"
      "m=audio 9 UDP/TLS/RTP/SAVPF 8\r\n"
      "a=ssrc:2 cname:go2rtc\r\n"
      "a=ssrc:2 msid:go2rtc go2rtc-audio\r\n"
      "a=ssrc:2 mslabel:go2rtc\r\n"
      "a=msid:go2rtc go2rtc-audio\r\n";
  const std::string separated = webrtc_sdp::separateAudio(answer);
  const auto video = sectionLines(separated, "m=video");
  const auto audio = sectionLines(separated, "m=audio");
  CHECK(count(video, "a=msid:go2rtc go2rtc-video") == 1);
  CHECK(count(video, "a=ssrc:1 mslabel:go2rtc") == 1);
  CHECK(count(audio, "a=msid:go2rtc-audio go2rtc-audio") == 1);
  CHECK(count(audio, "a=ssrc:2 msid:go2rtc-audio go2rtc-audio") == 1);
  CHECK(count(audio, "a=ssrc:2 mslabel:go2rtc-audio") == 1);
  CHECK(count(audio, "a=ssrc:2 cname:go2rtc") == 1);
  CHECK(separated.find("a=msid-semantic:WMS *\r\n") != std::string::npos);
  CHECK(webrtc_sdp::separateAudio("not sdp") == "not sdp");
}

TEST_CASE("a viewer's tag names its session without carrying it")
{
  const std::string tag = webrtc_viewer::tagOf({.userId = 4, .sessionId = "s-1"});
  CHECK(tag == webrtc_viewer::tagOf({.userId = 4, .sessionId = "s-1"}));
  CHECK(tag != webrtc_viewer::tagOf({.userId = 4, .sessionId = "s-2"}));
  CHECK(tag != webrtc_viewer::tagOf({.userId = 5, .sessionId = "s-1"}));
  CHECK(tag.find("s-1") == std::string::npos);
  CHECK(webrtc_viewer::isTagged(tag));
  CHECK_FALSE(webrtc_viewer::isTagged("argus"));
}

TEST_CASE("go2rtc's stream list yields its WebRTC viewers and the cameras that speak AAC")
{
  const Json::Value streams = parse(kStreams);
  const auto consumers = go2rtc_streams::webrtcConsumers(streams);
  REQUIRE(consumers.size() == 3);
  CHECK(go2rtc_streams::carriesAac(streams, "cam9-sub"));
  CHECK_FALSE(go2rtc_streams::carriesAac(streams, "cam6-sub"));
  CHECK_FALSE(go2rtc_streams::carriesAac(streams, "cam404"));
  CHECK(go2rtc_streams::webrtcConsumers(Json::Value("x")).empty());
}

TEST_CASE("WebRTC viewers count against the same limits as the WebSocket ones")
{
  const Json::Value streams = parse(kStreams);
  const std::unordered_map<int64_t, int> hub{{6, 1}, {7, 2}};
  const auto six = CameraWebRtcService::tally({.cameraId = 6, .streams = streams, .hubViewers = hub});
  CHECK(six.perCamera == 3);
  CHECK(six.total == 6);
  const auto seven = CameraWebRtcService::tally({.cameraId = 7, .streams = streams, .hubViewers = hub});
  CHECK(seven.perCamera == 3);
  const auto none = CameraWebRtcService::tally({.cameraId = 8, .streams = streams, .hubViewers = hub});
  CHECK(none.perCamera == 0);
  CHECK(none.total == 6);
}

TEST_CASE("the offer DTO reads the quality and refuses an unknown one")
{
  Json::Value body(Json::objectValue);
  body["sdp"] = std::string(kBrowserOffer);
  CHECK(CameraWebRtcOfferDto::fromJson(body).stream() == CameraStream::Main);
  body["quality"] = "sub";
  CHECK(CameraWebRtcOfferDto::fromJson(body).stream() == CameraStream::Sub);
  body["quality"] = "ultra";
  CHECK_THROWS_AS(CameraWebRtcOfferDto::fromJson(body), ValidationException);
  body["quality"] = "main";
  body["sdp"] = std::string(20000, 'v');
  CHECK_THROWS_AS(CameraWebRtcOfferDto::fromJson(body), ValidationException);
  CHECK_THROWS_AS(CameraWebRtcOfferDto::fromJson(Json::Value(Json::objectValue)), ValidationException);

  const Json::Value answer =
      ResponseCameraWebRtcDto{.sdp = "v=0\r\n", .stream = CameraStream::Sub, .audio = false}.toJson();
  CHECK(answer["type"].asString() == "answer");
  CHECK(answer["quality"].asString() == "sub");
  CHECK_FALSE(answer["audio"].asBool());
}

TEST_CASE("go2rtc listens for WebRTC on the configured port with no public STUN")
{
  const std::vector<Go2rtcSource> sources{
      {.name = "cam6", .url = "rtsp://192.168.1.20:554/stream1", .preload = false},
      {.name = "cam6-sub", .url = "rtsp://192.168.1.20:554/stream2", .preload = true}};
  const std::string on = Go2rtcManager::renderConfig(
      {.api = "127.0.0.1:1984",
       .rtsp = "127.0.0.1:8554",
       .webrtc = {.listen = ":8555", .candidates = {"192.168.1.10:8555", "bad host!", "stun:8555"}},
       .sources = sources});
  CHECK(on.find("webrtc:\n  listen: \":8555\"\n  ice_servers: []\n") != std::string::npos);
  CHECK(on.find("    - 192.168.1.10:8555\n") != std::string::npos);
  CHECK(on.find("    - stun:8555\n") != std::string::npos);
  CHECK(on.find("bad host") == std::string::npos);
  CHECK(on.find("  cam6-opus: ffmpeg:cam6#video=copy#audio=opus\n") != std::string::npos);
  CHECK(on.find("  cam6-sub-opus: ffmpeg:cam6-sub#video=copy#audio=opus\n") != std::string::npos);
  CHECK(on.find("preload:\n  cam6-sub:\n") != std::string::npos);
  CHECK(on.find("cam6-opus:\n") == std::string::npos);

  const std::string off = Go2rtcManager::renderConfig(
      {.api = "127.0.0.1:1984", .rtsp = "127.0.0.1:8554", .webrtc = {.listen = "", .candidates = {}}, .sources = sources});
  CHECK(off.find("webrtc:\n  listen: \"\"\nlog:") != std::string::npos);
  CHECK(off.find("-opus") == std::string::npos);

  CHECK(Go2rtcManager::isSafeListen(":8555"));
  CHECK(Go2rtcManager::isSafeListen("0.0.0.0:8555"));
  CHECK(Go2rtcManager::isSafeListen("[::]:8555"));
  CHECK_FALSE(Go2rtcManager::isSafeListen("8555"));
  CHECK_FALSE(Go2rtcManager::isSafeListen(":0"));
  CHECK_FALSE(Go2rtcManager::isSafeListen(":70000"));
  CHECK_FALSE(Go2rtcManager::isSafeListen(":8555\"\napi:"));
  CHECK(Go2rtcManager::isSafeCandidate("192.168.1.10"));
  CHECK(Go2rtcManager::isSafeCandidate("[fd00::2]:8555"));
  CHECK_FALSE(Go2rtcManager::isSafeCandidate("10.0.0.1:x"));
  CHECK_FALSE(Go2rtcManager::isSafeCandidate("10.0.0.1\n  - x"));
}

TEST_CASE("an offer loses its candidates and an answer keeps only reachable LAN ones")
{
  const std::string offer = std::string(kBrowserOffer) +
                            "a=candidate:1 1 udp 2130706431 10.0.0.9 9000 typ host\r\n"
                            "a=end-of-candidates\r\n";
  const std::string prepared = webrtc_sdp::prepareOffer({.sdp = offer, .audio = true}).value_or("");
  REQUIRE_FALSE(prepared.empty());
  CHECK(prepared.find("a=candidate") == std::string::npos);
  CHECK(prepared.find("a=end-of-candidates") == std::string::npos);

  const std::string answer =
      "v=0\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
      "a=candidate:1 1 udp 2130706431 192.168.1.10 8555 typ host\r\n"
      "a=candidate:2 1 udp 2130706431 127.0.0.1 8555 typ host\r\n"
      "a=candidate:3 1 udp 2130706431 169.254.3.3 8555 typ host\r\n"
      "a=candidate:4 1 udp 2130706431 fe80::1 8555 typ host\r\n"
      "a=candidate:5 1 tcp 1671430143 10.8.0.2 8555 typ host tcptype passive\r\n"
      "a=candidate:6 1 udp 2130706431 host.local 8555 typ host\r\n";
  const std::string open = webrtc_sdp::screenCandidates({.sdp = answer, .allowedHosts = {}});
  CHECK(open.find("192.168.1.10") != std::string::npos);
  CHECK(open.find("10.8.0.2") != std::string::npos);
  CHECK(open.find("127.0.0.1") == std::string::npos);
  CHECK(open.find("169.254.3.3") == std::string::npos);
  CHECK(open.find("fe80::1") == std::string::npos);
  CHECK(open.find("host.local") == std::string::npos);
  CHECK(open.find("m=video 9 UDP/TLS/RTP/SAVPF 96\r\n") != std::string::npos);

  const std::vector<std::string> configured{"192.168.1.10"};
  const std::string pinned = webrtc_sdp::screenCandidates({.sdp = answer, .allowedHosts = configured});
  CHECK(pinned.find("192.168.1.10") != std::string::npos);
  CHECK(pinned.find("10.8.0.2") == std::string::npos);

  CHECK(Go2rtcManager::answerHostsOf({"192.168.1.10:8555", "[fd00::4]:8555", "10.0.0.2"}) ==
        std::vector<std::string>{"192.168.1.10", "fd00::4", "10.0.0.2"});
  CHECK(Go2rtcManager::answerHostsOf({"192.168.1.10:8555", "stun:8555"}).empty());
}

TEST_CASE("WebRTC seats are reserved atomically, capped per user, and the household keeps one")
{
  WebRtcAdmission admission;
  const auto now = std::chrono::steady_clock::now();
  const std::vector<Go2rtcWebRtcConsumer> none;
  const WebRtcViewerLimits limits{.perCamera = 2, .total = 3, .perUser = 2};
  const auto request = [&](int64_t user, bool priority) {
    return WebRtcSeatRequest{.cameraId = 6,
                             .userId = user,
                             .priority = priority,
                             .perCamera = 0,
                             .total = 0,
                             .consumers = none,
                             .limits = limits,
                             .snapshotAt = now};
  };

  const auto guest = admission.reserve(request(9, false));
  CHECK(guest.refusal == WebRtcSeatRefusal::None);
  CHECK(admission.reserve(request(9, false)).refusal == WebRtcSeatRefusal::Camera);
  const auto owner = admission.reserve(request(1, true));
  CHECK(owner.refusal == WebRtcSeatRefusal::None);
  CHECK(admission.reserve(request(1, true)).refusal == WebRtcSeatRefusal::Camera);

  admission.release({.ticket = guest.ticket, .at = now + std::chrono::seconds(1)});
  CHECK(admission.reserve(request(1, true)).refusal == WebRtcSeatRefusal::Camera);
  admission.release({.ticket = guest.ticket, .at = now - std::chrono::seconds(1)});
  CHECK(admission.reserve(request(1, true)).refusal == WebRtcSeatRefusal::None);

  WebRtcAdmission perUser;
  const std::vector<Go2rtcWebRtcConsumer> watching{{.stream = "cam7", .userAgent = "tag-a"},
                                                   {.stream = "cam8", .userAgent = "tag-b"}};
  perUser.remember({.tag = "tag-a", .userId = 4, .at = now});
  perUser.remember({.tag = "tag-b", .userId = 4, .at = now});
  const WebRtcViewerLimits wide{.perCamera = 8, .total = 8, .perUser = 2};
  CHECK(perUser.reserve({.cameraId = 6, .userId = 4, .priority = true, .perCamera = 0, .total = 2,
                         .consumers = watching, .limits = wide, .snapshotAt = now})
            .refusal == WebRtcSeatRefusal::User);
  CHECK(perUser.reserve({.cameraId = 6, .userId = 5, .priority = false, .perCamera = 0, .total = 2,
                         .consumers = watching, .limits = wide, .snapshotAt = now})
            .refusal == WebRtcSeatRefusal::None);

  CHECK(WebRtcAdmission::hasRoom({.used = 7, .limit = 8, .priority = true}));
  CHECK_FALSE(WebRtcAdmission::hasRoom({.used = 7, .limit = 8, .priority = false}));
  CHECK(WebRtcAdmission::hasRoom({.used = 0, .limit = 1, .priority = false}));
}

TEST_CASE("a role change finds every WebRTC view its user holds, so the closer can end them")
{
  WebRtcAdmission admission;
  const auto now = std::chrono::steady_clock::now();
  const std::string first = webrtc_viewer::tagOf({.userId = 4, .sessionId = "aa11"});
  const std::string second = webrtc_viewer::tagOf({.userId = 4, .sessionId = "bb22"});
  const std::string other = webrtc_viewer::tagOf({.userId = 5, .sessionId = "aa11"});
  admission.remember({.tag = first, .userId = 4, .at = now});
  admission.remember({.tag = second, .userId = 4, .at = now});
  admission.remember({.tag = other, .userId = 5, .at = now});

  auto tags = admission.tagsOf(4);
  std::ranges::sort(tags);
  std::vector<std::string> expected{first, second};
  std::ranges::sort(expected);
  CHECK(tags == expected);
  CHECK(admission.tagsOf(5) == std::vector<std::string>{other});
  CHECK(admission.tagsOf(6).empty());
}
