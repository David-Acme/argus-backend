#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/stream/camera-source-registrar.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <string>
#include <utility>
#include <vector>

namespace
{
class RecordingSink : public ICameraSourceSink
{
public:
  bool applySources(const CameraSourceChange& change) override
  {
    ++batches;
    for (const auto& source : change.upserts)
      added.emplace_back(source.name, source.url);
    removed.insert(removed.end(), change.removals.begin(), change.removals.end());
    return true;
  }

  int batches{0};
  std::vector<std::pair<std::string, std::string>> added;
  std::vector<std::string> removed;
};
}

TEST_CASE("camera source urls percent-encode credentials")
{
  CameraSchema camera;
  camera.id = 7;
  camera.ip = "192.168.1.50";
  camera.port = 554;
  camera.username = "user@example.com";
  camera.password = "p:a ss/word";

  CHECK(CameraSourceRegistrar::sourceUrl(camera, "stream1") ==
        "rtsp://user%40example.com:p%3Aa%20ss%2Fword@192.168.1.50:554/stream1");
}

TEST_CASE("an IPv6 camera address is bracketed in its source url")
{
  CameraSchema camera;
  camera.id = 8;
  camera.ip = "fd00::5";
  camera.port = 554;
  CHECK(CameraSourceRegistrar::sourceUrl(camera, "/stream1") == "rtsp://[fd00::5]:554/stream1");
  CHECK(Go2rtcManager::isSafeUrl("rtsp://[fd00::5]:554/stream1"));
  CHECK(Go2rtcManager::isSafeUrl("rtsp://admin:x@[fe80::1]:554/stream2"));
  CHECK_FALSE(Go2rtcManager::isSafeUrl("rtsp://[2001:db8::1]:554/stream1"));
}

TEST_CASE("only literal private hosts pass the go2rtc source guard")
{
  CHECK(Go2rtcManager::isSafeUrl("rtsp://admin:secret@192.168.1.50:554/stream1"));
  CHECK(Go2rtcManager::isSafeUrl("rtsp://172.20.0.4:554/live"));
  CHECK_FALSE(Go2rtcManager::isSafeUrl("rtsp://10.evil.example:554/stream1"));
  CHECK_FALSE(Go2rtcManager::isSafeUrl("rtsp://172.32.0.4:554/stream1"));
  CHECK_FALSE(Go2rtcManager::isSafeUrl("rtsp://8.8.8.8:554/stream1"));
  CHECK_FALSE(Go2rtcManager::isSafeUrl("rtsp://10.0.0.5:554/a#b"));
  CHECK_FALSE(Go2rtcManager::isSafeUrl("rtsp://10.0.0.5:554/a b"));
  CHECK(Go2rtcManager::isSafeUrl("rtsp://10.0.0.5:554/cam/realmonitor?channel=1&subtype=0"));
}

TEST_CASE("a camera's configured stream paths replace the Tapo defaults")
{
  CameraSchema camera;
  camera.id = 12;
  camera.ip = "10.0.0.12";
  camera.isEnabled = true;
  camera.config = R"({"streamPath":"/Streaming/Channels/101","subStreamPath":"/Streaming/Channels/102"})";

  RecordingSink sink;
  CameraSourceRegistrar registrar(sink);
  registrar.apply(camera);
  REQUIRE(sink.added.size() == 2);
  CHECK(sink.added[0].second == "rtsp://10.0.0.12:554/Streaming/Channels/101");
  CHECK(sink.added[1].second == "rtsp://10.0.0.12:554/Streaming/Channels/102");

  camera.config = R"({"streamPath":"no-slash"})";
  sink.added.clear();
  registrar.apply(camera);
  REQUIRE(sink.added.size() == 2);
  CHECK(sink.added[0].second == "rtsp://10.0.0.12:554/stream1");
  CHECK(sink.added[1].second == "rtsp://10.0.0.12:554/stream2");
}

TEST_CASE("camera registrar syncs main and sub sources")
{
  CameraSchema camera;
  camera.id = 3;
  camera.ip = "10.0.0.7";
  camera.username = "admin";
  camera.password = "secret";
  camera.isEnabled = true;

  RecordingSink sink;
  CameraSourceRegistrar registrar(sink);
  registrar.apply(camera);

  CHECK(sink.batches == 1);
  REQUIRE(sink.added.size() == 2);
  CHECK(sink.added[0].first == "cam3");
  CHECK(sink.added[0].second == "rtsp://admin:secret@10.0.0.7:554/stream1");
  CHECK(sink.added[1].first == "cam3-sub");
  CHECK(sink.added[1].second == "rtsp://admin:secret@10.0.0.7:554/stream2");

  registrar.remove(3);
  REQUIRE(sink.removed.size() == 2);
  CHECK(sink.removed[0] == "cam3");
  CHECK(sink.removed[1] == "cam3-sub");
}

TEST_CASE("camera registrar removes sources when disabled")
{
  CameraSchema camera;
  camera.id = 9;
  camera.ip = "10.0.0.9";
  camera.isEnabled = false;

  RecordingSink sink;
  CameraSourceRegistrar registrar(sink);
  registrar.apply(camera);

  CHECK(sink.added.empty());
  REQUIRE(sink.removed.size() == 2);
  CHECK(sink.removed[0] == "cam9");
  CHECK(sink.removed[1] == "cam9-sub");
}

TEST_CASE("camera registrar hands every camera at boot over in one batch")
{
  std::vector<CameraSchema> cameras(3);
  for (int64_t i = 0; i < 3; ++i) {
    cameras[static_cast<size_t>(i)].id = i + 1;
    cameras[static_cast<size_t>(i)].ip = "10.0.0." + std::to_string(i + 1);
    cameras[static_cast<size_t>(i)].isEnabled = i != 1;
  }

  RecordingSink sink;
  CameraSourceRegistrar registrar(sink);
  registrar.applyAll(cameras);

  CHECK(sink.batches == 1);
  CHECK(sink.added.size() == 4);
  CHECK(sink.removed == std::vector<std::string>{"cam2", "cam2-sub"});

  registrar.applyAll({});
  CHECK(sink.batches == 1);
}

TEST_CASE("go2rtc rewrites its config only when a source really changed")
{
  const std::filesystem::path config = "go2rtc.yaml";
  std::filesystem::remove(config);
  const auto written = [&config] {
    std::ifstream in(config);
    std::stringstream body;
    body << in.rdbuf();
    return body.str();
  };

  Go2rtcManager manager;
  const Go2rtcSource main{.name = "cam1", .url = "rtsp://10.0.0.1:554/stream1"};
  CHECK(manager.applySources({.upserts = {main}, .removals = {}}));
  CHECK(written().find("cam1: rtsp://10.0.0.1:554/stream1") != std::string::npos);

  std::filesystem::remove(config);
  CHECK(manager.applySources({.upserts = {main}, .removals = {}}));
  CHECK_FALSE(std::filesystem::exists(config));
  CHECK(manager.applySources({.upserts = {}, .removals = {"cam9"}}));
  CHECK_FALSE(std::filesystem::exists(config));

  CHECK(manager.applySources(
      {.upserts = {{.name = "cam1", .url = "rtsp://10.0.0.2:554/stream1"}},
       .removals = {}}));
  CHECK(written().find("cam1: rtsp://10.0.0.2:554/stream1") != std::string::npos);

  CHECK(manager.applySources(
      {.upserts = {{.name = "cam2", .url = "rtsp://8.8.8.8:554/stream1"}},
       .removals = {}}));
  CHECK(written().find("cam2") == std::string::npos);

  CHECK(manager.applySources({.upserts = {}, .removals = {"cam1"}}));
  CHECK(written().find("cam1") == std::string::npos);
  std::filesystem::remove(config);
}
