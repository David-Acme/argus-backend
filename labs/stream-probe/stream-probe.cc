#include <arpa/inet.h>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <netinet/in.h>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <shared/services/stream/stream-hub.hxx>
#include <shared/services/stream/upstream-http.hxx>
#include <shared/services/stream/ws-frame.hxx>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{

int failures = 0;

void check(const char* what, bool got, bool want)
{
  const bool ok = got == want;
  if (!ok)
    ++failures;
  std::printf("  [%s] %-58s got=%d want=%d\n", ok ? "ok" : "FAIL", what,
              static_cast<int>(got), static_cast<int>(want));
}

std::string urlEncode(const std::string& s)
{
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
      out += static_cast<char>(c);
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

bool addGo2rtcStream(const std::string& name, const std::string& src)
{
  const auto [host, port] =
      upstream_http::splitHostPort(Go2rtcManager::apiBase().substr(7));
  const std::string path =
      "/api/streams?name=" + name + "&src=" + urlEncode(src);

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
  timeval tv{};
  tv.tv_sec = 5;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return false;
  }
  const std::string req =
      "PUT " + path +
      " HTTP/1.1\r\nHost: " + host +
      "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  ::send(fd, req.data(), req.size(), 0);
  char buf[1024];
  const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
  ::close(fd);
  if (n <= 0)
    return false;
  return std::string(buf, static_cast<size_t>(n)).find(" 200") !=
         std::string::npos;
}

struct FakeSink final : StreamHub::ISink
{
  std::mutex mtx;
  std::vector<ws_frame::Header> headers;
  size_t payloadBytes{0};
  bool initSeen{false};
  bool mediaSeen{false};
  bool keyframeSeen{false};
  bool closed{false};

  bool sendBinary(const uint8_t* data, size_t len) override
  {
    ws_frame::Header h;
    if (!ws_frame::parse(data, len, h))
      return false;
    std::lock_guard<std::mutex> lock(mtx);
    headers.push_back(h);
    payloadBytes += len - ws_frame::kHeaderSize;
    if (h.type == ws_frame::kTypeInit)
      initSeen = true;
    if (h.type == ws_frame::kTypeMedia) {
      mediaSeen = true;
      if (h.keyframe)
        keyframeSeen = true;
    }
    return !closed;
  }

  void onClosed(const std::string&) override { closed = true; }

  size_t bytes()
  {
    std::lock_guard<std::mutex> lock(mtx);
    return payloadBytes;
  }
};

void streamHubCheck()
{
  std::printf("\n=== stream hub ===\n");

  const char* probeRtsp = std::getenv("ARGUS_PROBE_RTSP");
  if (!probeRtsp) {
    std::printf(
        "  hub checks skipped (set ARGUS_PROBE_RTSP to a camera RTSP URL)\n");
    return;
  }

  const bool added = addGo2rtcStream("cam1", probeRtsp);
  check("hub source added", added, true);
  if (!added) {
    std::printf("  hub checks skipped (could not create source)\n");
    return;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(2500));

  auto sink = std::make_shared<FakeSink>();
  StreamHub::SubscribeInput input;
  input.sink = sink;
  input.cameraId = 1;
  input.quality = "main";
  std::string err;
  const uint16_t subId = StreamHub::subscribe(input, err);
  check("hub subscribe", subId != 0, true);
  if (subId == 0)
    return;

  for (int i = 0; i < 80 && !(sink->initSeen && sink->mediaSeen); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  check("hub init before media", sink->initSeen, true);
  check("hub media frames", sink->mediaSeen, true);
  check("hub keyframe present", sink->keyframeSeen, true);

  const size_t w0 = sink->bytes();
  std::this_thread::sleep_for(std::chrono::seconds(12));
  const size_t w1 = sink->bytes();
  check("hub caps in-flight without ack", w1 - w0 < 256 * 1024, true);

  const size_t w2 = sink->bytes();
  std::this_thread::sleep_for(std::chrono::seconds(4));
  const size_t w3 = sink->bytes();
  check("hub stops growing without ack", w3 - w2 < 32 * 1024, true);

  StreamHub::ack(subId, static_cast<int64_t>(w3));
  const size_t base = sink->bytes();
  bool resumed = false;
  for (int i = 0; i < 24 && !resumed; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    resumed = sink->bytes() > base;
  }
  check("hub resumes after ack", resumed, true);

  StreamHub::unsubscribe(subId);
  const size_t beforeUnsub = sink->bytes();
  std::this_thread::sleep_for(std::chrono::seconds(1));
  const size_t afterUnsub = sink->bytes();
  check("hub unsubscribe stops flow", afterUnsub - beforeUnsub < 4096, true);

  check("hub single upstream shared", StreamHub::activeUpstreams() == 1, true);

  StreamHub::shutdown();
  check("hub shutdown clears", StreamHub::activeUpstreams() == 0, true);
}

} // namespace

int main()
{
  ConfigService::load("config.toml");

  std::printf("=== name validation ===\n");
  check("plain name", Go2rtcManager::isSafeName("cam1"), true);
  check("underscore/dash", Go2rtcManager::isSafeName("front_door-2"), true);
  check("empty", Go2rtcManager::isSafeName(""), false);
  check("newline injection", Go2rtcManager::isSafeName("cam1\nstreams:"), false);
  check("space", Go2rtcManager::isSafeName("cam 1"), false);
  check("yaml colon", Go2rtcManager::isSafeName("cam:evil"), false);
  check("path traversal", Go2rtcManager::isSafeName("../../etc"), false);

  std::printf("\n=== url validation ===\n");
  check("private rtsp", Go2rtcManager::isSafeUrl("rtsp://u:p@192.168.1.50:554/stream1"), true);
  check("private tapo", Go2rtcManager::isSafeUrl("tapo://pass@10.0.0.7"), true);
  check("loopback", Go2rtcManager::isSafeUrl("rtsp://127.0.0.1:8554/cam1"), true);
  check("172.16 private", Go2rtcManager::isSafeUrl("rtsp://172.16.3.4:554/s"), true);
  check("172.32 NOT private", Go2rtcManager::isSafeUrl("rtsp://172.32.3.4:554/s"), false);
  check("public host rejected", Go2rtcManager::isSafeUrl("rtsp://8.8.8.8:554/s"), false);
  check("bad scheme", Go2rtcManager::isSafeUrl("file:///etc/passwd"), false);
  check("newline in url", Go2rtcManager::isSafeUrl("rtsp://192.168.1.5/a\nb: c"), false);
  check("shell metachar", Go2rtcManager::isSafeUrl("rtsp://192.168.1.5/$(id)"), false);
  check("backtick", Go2rtcManager::isSafeUrl("rtsp://192.168.1.5/`id`"), false);
  check("quote", Go2rtcManager::isSafeUrl("rtsp://192.168.1.5/\"x"), false);

  std::printf("\n=== process lifecycle ===\n");
  Go2rtcManager::init();
  bool up = false;
  for (int i = 0; i < 40 && !up; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    up = Go2rtcManager::healthCheck();
  }
  const auto st = Go2rtcManager::status();
  std::printf("  running=%d healthy=%d pid=%lld restarts=%d api=%s\n",
              static_cast<int>(st.running), static_cast<int>(up),
              static_cast<long long>(st.pid), st.restarts,
              Go2rtcManager::apiBase().c_str());
  if (!up) {
    ++failures;
    std::printf("  [FAIL] go2rtc did not become healthy: %s\n",
                st.lastError.c_str());
  }

  check("addSource rejects unsafe url",
        Go2rtcManager::addSource({"cam9", "rtsp://8.8.8.8/x"}), false);

  streamHubCheck();

  Go2rtcManager::shutdown();
  std::printf("  after shutdown: running=%d\n",
              static_cast<int>(Go2rtcManager::isRunning()));

  std::printf("\n%s (%d failures)\n", failures == 0 ? "ALL PASS" : "FAILURES",
              failures);
  return failures == 0 ? 0 : 1;
}
