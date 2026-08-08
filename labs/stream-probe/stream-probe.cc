#include <chrono>
#include <cstdio>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <string>
#include <thread>
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

  Go2rtcManager::shutdown();
  std::printf("  after shutdown: running=%d\n",
              static_cast<int>(Go2rtcManager::isRunning()));

  std::printf("\n%s (%d failures)\n", failures == 0 ? "ALL PASS" : "FAILURES",
              failures);
  return failures == 0 ? 0 : 1;
}
