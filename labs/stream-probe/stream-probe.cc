#include <arpa/inet.h>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <netinet/in.h>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <shared/services/stream/media-relay.hxx>
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

std::string mp4Box(const std::string& type, const std::string& payload)
{
  const uint32_t size = static_cast<uint32_t>(8 + payload.size());
  std::string out;
  out += static_cast<char>((size >> 24) & 0xFF);
  out += static_cast<char>((size >> 16) & 0xFF);
  out += static_cast<char>((size >> 8) & 0xFF);
  out += static_cast<char>(size & 0xFF);
  out += type;
  out += payload;
  return out;
}

std::string be32(uint32_t v)
{
  std::string out;
  out += static_cast<char>((v >> 24) & 0xFF);
  out += static_cast<char>((v >> 16) & 0xFF);
  out += static_cast<char>((v >> 8) & 0xFF);
  out += static_cast<char>(v & 0xFF);
  return out;
}

std::string moofWithSync(bool sync)
{
  const uint32_t flags = sync ? 0x00000000U : 0x00010000U;
  const std::string trun =
      mp4Box("trun", be32(0x00000005) + be32(1) + be32(0) + be32(flags));
  const std::string tfhd = mp4Box("tfhd", be32(0x00000000) + be32(1));
  const std::string traf = mp4Box("traf", tfhd + trun);
  return mp4Box("moof", mp4Box("mfhd", be32(0) + be32(1)) + traf);
}

std::string chunkEncode(const std::string& body, size_t chunkSize)
{
  std::string out;
  char hex[32];
  for (size_t i = 0; i < body.size(); i += chunkSize) {
    const size_t n = std::min(chunkSize, body.size() - i);
    std::snprintf(hex, sizeof(hex), "%zx\r\n", n);
    out += hex;
    out += body.substr(i, n);
    out += "\r\n";
  }
  out += "0\r\n\r\n";
  return out;
}

struct ReaderCapture
{
  std::string init;
  std::vector<std::string> fragments;
  std::vector<bool> keyframes;
};

ReaderCapture runReader(const std::string& wire, bool chunked, size_t step)
{
  ReaderCapture cap;
  upstream_http::Fmp4Reader reader({.chunked = chunked});
  reader.onInit = [&](std::string box) { cap.init = std::move(box); };
  reader.onFragment = [&](std::string box, bool key) {
    cap.fragments.push_back(std::move(box));
    cap.keyframes.push_back(key);
  };
  for (size_t i = 0; i < wire.size(); i += step)
    reader.feed(wire.data() + i, std::min(step, wire.size() - i));
  return cap;
}

void fmp4ReaderCheck()
{
  std::printf("\n=== fmp4 reader ===\n");

  const std::string init = mp4Box("ftyp", std::string(24, '\x0a')) +
                           mp4Box("moov", std::string(600, '\r'));
  const std::string frag1 =
      moofWithSync(true) + mp4Box("mdat", std::string(20000, '\n'));
  const std::string frag2 =
      moofWithSync(false) + mp4Box("mdat", std::string(15000, '\x0d'));
  const std::string body = init + frag1 + frag2;

  check("identity: init completo", runReader(body, false, 65536).init == init,
        true);
  check("identity: 2 fragmentos",
        runReader(body, false, 65536).fragments.size() == 2, true);
  check("identity: fragmento 1 intacto",
        runReader(body, false, 65536).fragments[0] == frag1, true);
  check("identity: keyframe detectado",
        runReader(body, false, 65536).keyframes[0], true);
  check("identity: no-keyframe detectado",
        runReader(body, false, 65536).keyframes[1], false);

  for (const size_t step : {size_t(1), size_t(3), size_t(199), size_t(4096)}) {
    const auto cap = runReader(body, false, step);
    check("identity: estable troceando el wire",
          cap.init == init && cap.fragments.size() == 2 &&
              cap.fragments[0] == frag1 && cap.fragments[1] == frag2,
          true);
  }

  const std::string wire = chunkEncode(body, 1300);
  for (const size_t step :
       {size_t(1), size_t(2), size_t(1299), size_t(65536)}) {
    const auto cap = runReader(wire, true, step);
    check("chunked: estable troceando el wire",
          cap.init == init && cap.fragments.size() == 2 &&
              cap.fragments[0] == frag1 && cap.fragments[1] == frag2,
          true);
  }

  check("isChunked lee la cabecera",
        upstream_http::isChunked(
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked"),
        true);
  check("isChunked ignora identity",
        upstream_http::isChunked("HTTP/1.1 200 OK\r\nContent-Type: video/mp4"),
        false);
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
  const std::string req = "PUT " + path + " HTTP/1.1\r\nHost: " + host +
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

  const std::string jpeg = MediaRelay::snapshotBytes(1);
  check("snapshot devuelve bytes", !jpeg.empty(), true);
  check("snapshot es un JPEG",
        jpeg.size() > 2 && static_cast<unsigned char>(jpeg[0]) == 0xFF &&
            static_cast<unsigned char>(jpeg[1]) == 0xD8,
        true);

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

  fmp4ReaderCheck();

  std::printf("=== name validation ===\n");
  check("plain name", Go2rtcManager::isSafeName("cam1"), true);
  check("underscore/dash", Go2rtcManager::isSafeName("front_door-2"), true);
  check("empty", Go2rtcManager::isSafeName(""), false);
  check("newline injection", Go2rtcManager::isSafeName("cam1\nstreams:"),
        false);
  check("space", Go2rtcManager::isSafeName("cam 1"), false);
  check("yaml colon", Go2rtcManager::isSafeName("cam:evil"), false);
  check("path traversal", Go2rtcManager::isSafeName("../../etc"), false);

  std::printf("\n=== url validation ===\n");
  check("private rtsp",
        Go2rtcManager::isSafeUrl("rtsp://u:p@192.168.1.50:554/stream1"), true);
  check("private tapo", Go2rtcManager::isSafeUrl("tapo://pass@10.0.0.7"), true);
  check("loopback", Go2rtcManager::isSafeUrl("rtsp://127.0.0.1:8554/cam1"),
        true);
  check("172.16 private", Go2rtcManager::isSafeUrl("rtsp://172.16.3.4:554/s"),
        true);
  check("172.32 NOT private",
        Go2rtcManager::isSafeUrl("rtsp://172.32.3.4:554/s"), false);
  check("public host rejected",
        Go2rtcManager::isSafeUrl("rtsp://8.8.8.8:554/s"), false);
  check("bad scheme", Go2rtcManager::isSafeUrl("file:///etc/passwd"), false);
  check("newline in url",
        Go2rtcManager::isSafeUrl("rtsp://192.168.1.5/a\nb: c"), false);
  check("shell metachar", Go2rtcManager::isSafeUrl("rtsp://192.168.1.5/$(id)"),
        false);
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
