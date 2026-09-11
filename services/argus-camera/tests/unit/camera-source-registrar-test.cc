#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/stream/camera-source-registrar.hxx>

#include <string>
#include <utility>
#include <vector>

namespace
{
class RecordingSink : public ICameraSourceSink
{
public:
  bool addSource(const std::string& name, const std::string& url) override
  {
    added.emplace_back(name, url);
    return true;
  }

  bool removeSource(const std::string& name) override
  {
    removed.push_back(name);
    return true;
  }

  std::vector<std::pair<std::string, std::string>> added;
  std::vector<std::string> removed;
};
} // namespace

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
