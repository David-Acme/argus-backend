#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/camera/infra/rtsp-probe.hxx>
#include <shared/services/camera-catalog/camera-catalog.hxx>
#include <shared/services/camera-driver/camera-capabilities.hxx>
#include <shared/services/camera-driver/stream-only-driver.hxx>
#include <shared/services/camera-driver/tapo-driver.hxx>
#include <shared/services/tapo/tapo-crypto.hxx>
#include <shared/services/tapo/tapo-motor.hxx>
#include <shared/services/tapo/tapo-video.hxx>
#include <shared/vocabulary/camera-stream-paths.hxx>
#include <text/json-util.hxx>

#include <drogon/utils/Utilities.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{

constexpr const char* kSdp =
    "v=0\r\n"
    "o=- 0 0 IN IP4 127.0.0.1\r\n"
    "s=Session\r\n"
    "t=0 0\r\n"
    "m=video 0 RTP/AVP 96\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=fmtp:96 packetization-mode=1; sprop-parameter-sets=Z2QAH6zZQFAFuwEQAAADABAAAAMB4PGDGWA=,aOvjyyLA; profile-level-id=64001F\r\n"
    "a=control:track1\r\n"
    "m=audio 0 RTP/AVP 8\r\n"
    "a=rtpmap:8 PCMA/8000\r\n"
    "a=control:track2\r\n";

std::vector<uint8_t> decoded(const std::string& base64)
{
  const std::string bytes = drogon::utils::base64Decode(base64);
  return {bytes.begin(), bytes.end()};
}

class FakeRtspCamera
{
public:
  enum class Mode : uint8_t
  {
    Digest,
    Basic,
    Open
  };

  explicit FakeRtspCamera(Mode mode) : mode_(mode)
  {
    listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listen_, 8) != 0)
      throw std::runtime_error("the fake camera cannot listen");
    socklen_t len = sizeof(addr);
    ::getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { serve(); });
  }

  ~FakeRtspCamera()
  {
    ::shutdown(listen_, SHUT_RDWR);
    ::close(listen_);
    thread_.join();
  }

  FakeRtspCamera(const FakeRtspCamera&) = delete;
  FakeRtspCamera& operator=(const FakeRtspCamera&) = delete;

  [[nodiscard]] int port() const { return port_; }

private:
  static std::string readRequest(int fd)
  {
    std::string head;
    std::array<char, 1024> buffer{};
    while (head.find("\r\n\r\n") == std::string::npos) {
      const auto n = ::recv(fd, buffer.data(), buffer.size(), 0);
      if (n <= 0)
        return {};
      head.append(buffer.data(), static_cast<size_t>(n));
    }
    return head;
  }

  static void reply(int fd, const std::string& text)
  {
    ::send(fd, text.data(), text.size(), MSG_NOSIGNAL);
  }

  static std::string field(const std::string& head, const std::string& name)
  {
    const size_t at = head.find(name + "=\"");
    if (at == std::string::npos)
      return {};
    const size_t begin = at + name.size() + 2;
    return head.substr(begin, head.find('"', begin) - begin);
  }

  [[nodiscard]] bool authorized(const std::string& head) const
  {
    if (mode_ == Mode::Open)
      return true;
    if (mode_ == Mode::Basic)
      return head.find("Authorization: Basic " + drogon::utils::base64Encode("admin:secret")) !=
             std::string::npos;
    if (head.find("Authorization: Digest") == std::string::npos)
      return false;
    const auto lowered = [](std::string value) {
      for (auto& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return value;
    };
    const std::string uri = field(head, "uri");
    const std::string ha1 = lowered(tapo_crypto::md5Hex("admin:Argus Test:secret"));
    const std::string ha2 = lowered(tapo_crypto::md5Hex("DESCRIBE:" + uri));
    const std::string expected = lowered(tapo_crypto::md5Hex(ha1 + ":abc123:" + ha2));
    return lowered(field(head, "response")) == expected;
  }

  void serve()
  {
    for (;;) {
      const int fd = ::accept(listen_, nullptr, nullptr);
      if (fd < 0)
        return;
      for (;;) {
        const std::string head = readRequest(fd);
        if (head.empty())
          break;
        const size_t cseqAt = head.find("CSeq: ");
        const std::string cseq = head.substr(cseqAt + 6, head.find("\r\n", cseqAt) - cseqAt - 6);
        if (!authorized(head)) {
          std::string answer = "RTSP/1.0 401 Unauthorized\r\nCSeq: " + cseq;
          answer += "\r\nWWW-Authenticate: ";
          answer += mode_ == Mode::Basic ? R"(Basic realm="Argus Test")"
                                         : R"(Digest realm="Argus Test", nonce="abc123")";
          answer += "\r\n\r\n";
          reply(fd, answer);
          continue;
        }
        if (head.find("/missing") != std::string::npos) {
          reply(fd, "RTSP/1.0 404 Not Found\r\nCSeq: " + cseq + "\r\n\r\n");
          continue;
        }
        const std::string sdp = kSdp;
        std::string answer = "RTSP/1.0 200 OK\r\nCSeq: " + cseq;
        answer += "\r\nContent-Type: application/sdp\r\nContent-Length: ";
        answer += std::to_string(sdp.size());
        answer += "\r\n\r\n";
        answer += sdp;
        reply(fd, answer);
      }
      ::close(fd);
    }
  }

  Mode mode_;
  int listen_{-1};
  int port_{0};
  std::thread thread_;
};

int closedPort()
{
  const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    throw std::runtime_error("no free port");
  socklen_t len = sizeof(addr);
  ::getsockname(probe, reinterpret_cast<sockaddr*>(&addr), &len);
  const int port = ntohs(addr.sin_port);
  ::close(probe);
  return port;
}

RtspProbeInput probeOf(int port, const std::string& path)
{
  return {.host = "127.0.0.1", .port = port, .username = "admin", .password = "secret", .path = path, .timeoutMs = 1500};
}

}

TEST_CASE("every catalog entry has a unique id and the defaults its driver needs")
{
  std::set<std::string> ids;
  for (const auto& entry : camera_catalog::entries()) {
    CAPTURE(std::string(entry.id));
    CHECK(ids.insert(std::string(entry.id)).second);
    CHECK_FALSE(entry.brand.empty());
    CHECK(entry.defaults.rtspPort > 0);
    CHECK(camera_stream_paths::isValid(entry.defaults.streamPath));
    CHECK(camera_stream_paths::isValid(entry.defaults.subStreamPath));
    if (entry.driver == CameraDriver::Tapo) {
      CHECK_FALSE(entry.generic);
      CHECK(entry.has(CameraFeature::Microphone));
      CHECK(entry.has(CameraFeature::Speaker));
      CHECK(entry.has(CameraFeature::Ptz) == entry.has(CameraFeature::Presets));
      CHECK(entry.defaults.streamPath == "/stream1");
    }
    else {
      CHECK(entry.features == 0);
    }
  }
  CHECK(ids.size() >= 30);
}

TEST_CASE("a camera's model text finds its catalog entry")
{
  const auto* byModel = camera_catalog::find({.catalogId = {}, .driver = CameraDriver::Tapo, .model = "Tapo C225 (EU)"});
  REQUIRE(byModel != nullptr);
  CHECK(byModel->id == "tapo-c225");
  CHECK(camera_catalog::find({.catalogId = {}, .driver = CameraDriver::Tapo, .model = "C2250"}) == nullptr);
  CHECK(camera_catalog::find({.catalogId = {}, .driver = CameraDriver::Tapo, .model = "Tapo"}) == nullptr);
  CHECK(camera_catalog::find({.catalogId = {}, .driver = CameraDriver::Rtsp, .model = "C225"}) == nullptr);
  const auto* byId = camera_catalog::find({.catalogId = "tapo-c310", .driver = CameraDriver::Tapo, .model = "C200"});
  REQUIRE(byId != nullptr);
  CHECK(byId->model == "C310");
  CHECK(camera_catalog::find({.catalogId = "tapo-c310", .driver = CameraDriver::Onvif, .model = ""}) == nullptr);
}

TEST_CASE("the catalog serializes the shape the app reads")
{
  const Json::Value catalog = camera_catalog::toJson();
  REQUIRE(catalog["models"].isArray());
  const Json::Value& first = catalog["models"][0];
  for (const char* key : {"id", "brand", "manufacturer", "model", "driver", "formFactor", "resolution", "note"})
    CHECK(first[key].isString());
  CHECK(first["outdoor"].isBool());
  CHECK(first["defaults"]["port"].isInt());
  CHECK(first["features"]["ptz"].isBool());
  CHECK(first["features"]["speaker"].isBool());
}

TEST_CASE("a fixed Tapo camera offers no pan and tilt, an unknown model keeps every control")
{
  CameraSchema fixed;
  fixed.model = "C310";
  fixed.cloudPassword = "cloud";
  const Json::Value bullet = TapoDriver(fixed).capabilities();
  CHECK_FALSE(bullet["ptz"].asBool());
  CHECK_FALSE(bullet["autoTrack"].asBool());
  CHECK(bullet["talk"].asBool());
  CHECK(bullet["alarm"].asBool());
  CHECK(bullet["catalogId"].asString() == "tapo-c310");

  CameraSchema unknown;
  unknown.model = "Tapo";
  const Json::Value all = TapoDriver(unknown).capabilities();
  CHECK(all["ptz"].asBool());
  CHECK_FALSE(all["talk"].asBool());
  CHECK(all["catalogId"].asString().empty());

  CameraSchema generic;
  generic.driver = CameraDriver::Onvif;
  generic.config = R"({"catalogId":"hikvision"})";
  const Json::Value stream = StreamOnlyDriver(generic).capabilities();
  CHECK(stream["streamOnly"].asBool());
  CHECK(stream["catalogId"].asString() == "hikvision");
}

TEST_CASE("the catalog id lives in the camera config beside the stream paths")
{
  const std::string config = camera_stream_paths::withConfig(
      {.config = R"({"streamPath":"/a"})", .main = std::nullopt, .sub = std::nullopt, .catalogId = "reolink"});
  CHECK(camera_stream_paths::catalogIdOf(config) == "reolink");
  CHECK(camera_stream_paths::of(config).main == "/a");
  const std::string cleared = camera_stream_paths::withConfig(
      {.config = config, .main = std::nullopt, .sub = std::nullopt, .catalogId = ""});
  CHECK(camera_stream_paths::catalogIdOf(cleared).empty());
}

TEST_CASE("an H.264 parameter set gives the picture size, cropping included")
{
  CHECK(rtsp_probe::h264Size(decoded("Z2QAH6zZQFAFuwEQAAADABAAAAMB4PGDGWA=")).width == 1280);
  CHECK(rtsp_probe::h264Size(decoded("Z2QAH6zZQFAFuwEQAAADABAAAAMB4PGDGWA=")).height == 720);
  const auto full = rtsp_probe::h264Size(decoded("Z2QAKKzZQHgCJ+XARAAAAwAEAAADAHg8YMZY"));
  CHECK(full.width == 1920);
  CHECK(full.height == 1080);
  const auto qhd = rtsp_probe::h264Size(decoded("Z2QAMqzZQCgAtbARAAADAAEAAAMAHg8YMZY="));
  CHECK(qhd.width == 2560);
  CHECK(qhd.height == 1440);
  const auto baseline = rtsp_probe::h264Size(decoded("Z0LAH9kAUAW7ARAAAAMAEAAAAwHg8YMkgA=="));
  CHECK(baseline.width == 1280);
  CHECK(baseline.height == 720);
  const auto timed = rtsp_probe::h264Size(decoded("Z0LAFtoCgL/lwEQAAAMABAAAAwB6PFi6gA=="));
  CHECK(timed.width == 640);
  CHECK(timed.height == 360);
  CHECK(timed.fps == doctest::Approx(15.0));
  CHECK(rtsp_probe::h264Size(std::vector<uint8_t>{0x68, 0x01}).width == 0);
}

TEST_CASE("a session description names the codecs and the size")
{
  const auto result = rtsp_probe::readSdp(kSdp);
  CHECK(result.outcome == RtspProbeOutcome::Ok);
  CHECK(result.videoCodec == "H264");
  CHECK(result.audioCodec == "PCMA");
  CHECK(result.width == 1280);
  CHECK(result.height == 720);
  CHECK(rtsp_probe::readSdp("v=0\r\nm=audio 0 RTP/AVP 8\r\na=rtpmap:8 PCMA/8000\r\n").outcome ==
        RtspProbeOutcome::NoVideo);
}

TEST_CASE("the probe answers a digest challenge and reads the stream")
{
  FakeRtspCamera camera(FakeRtspCamera::Mode::Digest);
  const auto result = rtsp_probe::describe(probeOf(camera.port(), "/stream1"));
  CHECK(rtspProbeOutcomeToString(result.outcome) == "ok");
  CHECK(result.videoCodec == "H264");
  CHECK(result.width == 1280);

  auto wrong = probeOf(camera.port(), "/stream1");
  wrong.password = "nope";
  CHECK(rtsp_probe::describe(wrong).outcome == RtspProbeOutcome::AuthFailed);

  auto anonymous = probeOf(camera.port(), "/stream1");
  anonymous.username.clear();
  anonymous.password.clear();
  CHECK(rtsp_probe::describe(anonymous).outcome == RtspProbeOutcome::AuthFailed);
}

TEST_CASE("the probe tells a wrong path, a closed port and a basic login apart")
{
  FakeRtspCamera digest(FakeRtspCamera::Mode::Digest);
  CHECK(rtsp_probe::describe(probeOf(digest.port(), "/missing")).outcome == RtspProbeOutcome::NotFound);

  FakeRtspCamera basic(FakeRtspCamera::Mode::Basic);
  CHECK(rtsp_probe::describe(probeOf(basic.port(), "/stream2")).outcome == RtspProbeOutcome::Ok);

  const auto refused = rtsp_probe::describe(probeOf(closedPort(), "/stream1"));
  CHECK(refused.outcome == RtspProbeOutcome::Refused);

  CHECK(rtsp_probe::urlHost("fd00::5") == "[fd00::5]");
  CHECK(rtsp_probe::urlHost("192.168.1.2") == "192.168.1.2");
}

TEST_CASE("a camera's capabilities travel on its row as the names of what it can do")
{
  CameraSchema patio;
  patio.model = "Tapo C225";
  patio.config = R"({"catalogId":"tapo-c225"})";
  const Json::Value withoutCloud = camera_capabilities::of(patio);
  CHECK(withoutCloud["ptz"].asBool());
  CHECK(withoutCloud["microphone"].asBool());
  CHECK_FALSE(withoutCloud["talk"].asBool());
  CHECK(camera_capabilities::listOf(withoutCloud).find("\"talk\"") == std::string::npos);

  patio.cloudPassword = "cloud";
  const std::string list = camera_capabilities::listOf(camera_capabilities::of(patio));
  CHECK(list ==
        R"(["ptz","presets","talk","microphone","privacy","led","dayNight","motion","autoTrack","alarm","sdCard"])");

  CameraSchema rtsp;
  rtsp.driver = CameraDriver::Rtsp;
  CHECK(camera_capabilities::listOf(camera_capabilities::of(rtsp)) == R"(["streamOnly"])");
  CHECK(StreamOnlyDriver(rtsp).capabilities() == camera_capabilities::of(rtsp));
  CHECK(TapoDriver(patio).capabilities() == camera_capabilities::of(patio));
}

TEST_CASE("the Tapo video profile reads the encoder's offer and its current setting")
{
  const Json::Value capability = json_util::fromString(
      R"({"video_capability":{"main":{"frame_rates":["65537","65551","65556","65561","65566"],)"
      R"("resolutions":["2688*1520","2560*1440","1920*1080"],"encode_types":["H264"]}}})");
  const Json::Value quality = json_util::fromString(
      R"({"video":{"main":{"resolution":"2688*1520","frame_rate":"65551","encode_type":"H264"}}})");
  const auto profile = tapo_video::profileOf({.capability = capability, .quality = quality});
  REQUIRE(profile.has_value());
  CHECK(profile->resolution == "2688x1520");
  CHECK(profile->frameRate == 15);
  CHECK(profile->encoding == "H264");
  CHECK(profile->frameRates == std::vector<int>{1, 15, 20, 25, 30});
  CHECK(profile->resolutions.size() == 3);
  CHECK(profile->toJson()["frameRates"].size() == 5);

  CHECK(tapo_video::frameRateCodeFor(capability, 30) == std::optional<std::string>("65566"));
  CHECK_FALSE(tapo_video::frameRateCodeFor(capability, 60).has_value());

  CHECK(tapo_video::frameRateOf(Json::Value("15")) == 15);
  CHECK(tapo_video::frameRateOf(Json::Value(25)) == 25);
  CHECK(tapo_video::frameRateOf(Json::Value("nonsense")) == 0);

  const Json::Value stringLists = json_util::fromString(
      R"({"video_capability":{"main":{"frame_rates":"[\"65551\",\"65566\"]"}}})");
  const Json::Value empty;
  const auto fromStrings = tapo_video::profileOf({.capability = stringLists, .quality = empty});
  REQUIRE(fromStrings.has_value());
  CHECK(fromStrings->frameRates == std::vector<int>{15, 30});
  CHECK(fromStrings->frameRate == 0);

  CHECK_FALSE(tapo_video::profileOf({.capability = empty, .quality = empty}).has_value());
}

TEST_CASE("a motor answer says whether the camera moved or stood at the end of its travel")
{
  const auto answer = [](int code) {
    return TapoResult::success(json_util::fromString(
        R"({"error_code":0,"result":{"responses":[{"error_code":)" + std::to_string(code) +
        R"(,"method":"motorMove","result":{}}]}})"));
  };
  const auto moved = tapo_motor::outcomeOf(answer(0));
  CHECK(moved.ok);
  CHECK(moved.data["moved"].asBool());
  CHECK_FALSE(moved.data["limit"].asBool());

  const auto limit = tapo_motor::outcomeOf(answer(tapo_motor::kLockedRotor));
  CHECK(limit.ok);
  CHECK_FALSE(limit.data["moved"].asBool());
  CHECK(limit.data["limit"].asBool());

  const auto refused = tapo_motor::outcomeOf(answer(-40210));
  CHECK_FALSE(refused.ok);
  CHECK(refused.errorCode == -40210);

  CHECK_FALSE(tapo_motor::outcomeOf(TapoResult::failure("no answer")).ok);
}
