#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/onvif-events/onvif-motion-events.hxx>
#include <feature/onvif-events/onvif-motion-watch.hxx>
#include <shared/services/camera-driver/onvif-driver.hxx>
#include <shared/services/camera-motion/camera-motion-board.hxx>

#include <scripted-onvif.hxx>

#include <trantor/net/EventLoopThread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr int64_t kStart = 1791910800000;
constexpr int64_t kSecond = 1000;

using scripted_onvif::answer;
using scripted_onvif::ScriptedOnvif;

class FakeClock
{
public:
  [[nodiscard]] int64_t now() const { return nowMs_.load(); }

  void set(int64_t ms) { nowMs_.store(ms); }

private:
  std::atomic<int64_t> nowMs_{kStart};
};

bool waitUntil(const std::function<bool()>& ready, std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (ready())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return ready();
}

void onLoop(trantor::EventLoop* loop, const std::function<void()>& work)
{
  std::mutex mutex;
  std::condition_variable done;
  bool finished = false;
  loop->runInLoop([&] {
    work();
    const std::scoped_lock lock(mutex);
    finished = true;
    done.notify_one();
  });
  std::unique_lock lock(mutex);
  done.wait(lock, [&finished] { return finished; });
}

std::shared_ptr<ScriptedOnvif> eventsCamera()
{
  auto camera = scripted_onvif::ptzCamera();
  camera->always("GetEventPropertiesRequest", answer("get-event-properties.xml"));
  camera->always("CreatePullPointSubscriptionRequest", answer("create-pull-point-subscription.xml"));
  camera->always("RenewRequest", answer("renew.xml"));
  camera->always("UnsubscribeRequest", answer("unsubscribe.xml"));
  camera->hold("PullMessagesRequest");
  return camera;
}

OnvifEndpoint cameraAt()
{
  return {.host = "192.168.1.64", .port = 80, .username = "admin", .password = "Secret!1", .timeoutSeconds = 1.0};
}

struct RigTiming
{
  int64_t firstBackoffMs{10 * kSecond};
  int64_t minPullGapMs{OnvifMotionWatch::kMinPullGapMs};
};

struct Rig
{
  trantor::EventLoopThread thread{"onvif-watch-test"};
  std::shared_ptr<ScriptedOnvif> camera = eventsCamera();
  CameraMotionBoard board;
  FakeClock clock;
  std::shared_ptr<OnvifMotionWatch> watch;

  explicit Rig(RigTiming timing = {})
  {
    thread.run();
    watch = std::make_shared<OnvifMotionWatch>(OnvifMotionWatchParts{.cameraId = 7,
                                                                     .endpoint = cameraAt(),
                                                                     .transport = camera,
                                                                     .board = &board,
                                                                     .loop = thread.getLoop(),
                                                                     .clockMs = [this] { return clock.now(); },
                                                                     .firstBackoffMs = timing.firstBackoffMs,
                                                                     .maxBackoffMs = 6 * timing.firstBackoffMs,
                                                                     .minPullGapMs = timing.minPullGapMs});
  }

  Rig(const Rig&) = delete;
  Rig& operator=(const Rig&) = delete;
  Rig(Rig&&) = delete;
  Rig& operator=(Rig&&) = delete;

  ~Rig()
  {
    watch->stop();
    static_cast<void>(waitUntil([this] { return watch->finished(); }));
  }

  void deliver(const std::string& fixture, int64_t atMs)
  {
    clock.set(atMs);
    onLoop(thread.getLoop(), [this, &fixture] { CHECK(camera->release(answer(fixture))); });
  }

  [[nodiscard]] bool pulling() const { return camera->waiting(); }
};

std::ptrdiff_t positionOf(const std::vector<std::string>& journal, const std::string& entry)
{
  const auto found = std::ranges::find(journal, entry);
  return found == journal.end() ? -1 : std::distance(journal.begin(), found);
}
}

TEST_CASE("a watch subscribes, and the camera's onset and end reach the board at the time Argus receives them")
{
  Rig rig;
  rig.watch->start();
  REQUIRE(waitUntil([&rig] { return rig.pulling(); }));
  CHECK(rig.camera->operations() ==
        std::vector<std::string>{"GetSystemDateAndTime", "GetCapabilities", "GetEventPropertiesRequest",
                                 "CreatePullPointSubscriptionRequest", "PullMessagesRequest"});
  CHECK_FALSE(rig.board.raisesCadence(7, kStart));

  rig.deliver("pull-messages-onset.xml", kStart + 20 * kSecond);
  REQUIRE(waitUntil([&rig] { return rig.pulling(); }));
  CHECK(rig.board.raisesCadence(7, kStart + 20 * kSecond));
  CHECK(rig.board.raisesCadence(7, kStart + 25 * kSecond));

  rig.deliver("pull-messages-end.xml", kStart + 28 * kSecond);
  REQUIRE(waitUntil([&rig] { return rig.pulling(); }));
  CHECK(rig.board.raisesCadence(7, kStart + 28 * kSecond + CameraMotionBoard::kHoldMs - 1));
  CHECK_FALSE(rig.board.raisesCadence(7, kStart + 28 * kSecond + CameraMotionBoard::kHoldMs));
  CHECK(rig.camera->peak() == 1);
}

TEST_CASE("a watch renews at half-life before it pulls again")
{
  Rig rig;
  rig.watch->start();
  REQUIRE(waitUntil([&rig] { return rig.pulling(); }));
  rig.deliver("pull-messages-empty.xml", kStart + 29 * kSecond);
  REQUIRE(waitUntil([&rig] { return rig.pulling() && rig.camera->operations().size() == 6; }));
  CHECK(rig.camera->operations().back() == "PullMessagesRequest");
  rig.deliver("pull-messages-empty.xml", kStart + 30 * kSecond);
  REQUIRE(waitUntil([&rig] { return rig.pulling() && rig.camera->operations().size() == 8; }));
  const auto operations = rig.camera->operations();
  CHECK(operations[6] == "RenewRequest");
  CHECK(operations[7] == "PullMessagesRequest");
  CHECK(OnvifMotionWatch::renewAfterMs(60) == 30 * kSecond);
  CHECK(OnvifMotionWatch::renewAfterMs(0) == 30 * kSecond);
}

TEST_CASE("a drain cancels the pull in flight at once, then unsubscribes on a new connection, never two at once")
{
  Rig rig;
  rig.watch->start();
  REQUIRE(waitUntil([&rig] { return rig.pulling(); }));
  rig.deliver("pull-messages-onset.xml", kStart + kSecond);
  REQUIRE(waitUntil([&rig] { return rig.pulling(); }));

  const auto asked = std::chrono::steady_clock::now();
  rig.clock.set(kStart + 2 * kSecond);
  rig.watch->stop();
  REQUIRE(waitUntil([&rig] { return rig.watch->finished(); }));
  CHECK(std::chrono::steady_clock::now() - asked < std::chrono::seconds(1));

  const auto journal = rig.camera->journal();
  const auto cancel = positionOf(journal, "cancel");
  REQUIRE(cancel >= 0);
  CHECK(journal[static_cast<size_t>(cancel) + 1] == "close PullMessagesRequest");
  CHECK(positionOf(journal, "open UnsubscribeRequest") > cancel);
  CHECK(journal.back() == "close UnsubscribeRequest");
  CHECK(rig.camera->cancelled() == 1);
  CHECK(rig.camera->peak() == 1);
  CHECK(rig.camera->open() == 0);
  CHECK(rig.camera->operations().back() == "UnsubscribeRequest");
  CHECK(rig.board.raisesCadence(7, kStart + 2 * kSecond + CameraMotionBoard::kHoldMs - 1));
  CHECK_FALSE(rig.board.raisesCadence(7, kStart + 2 * kSecond + CameraMotionBoard::kHoldMs));
}

TEST_CASE("a refused connection is not retried in a loop: the watch waits out its backoff, and a stop wakes it")
{
  Rig rig;
  rig.camera->once("GetSystemDateAndTime",
                   {.status = 0, .body = {}, .challenge = {}, .error = "no ONVIF answer (Connection refused)"});
  rig.watch->start();
  REQUIRE(waitUntil([&rig] { return rig.camera->operations().size() == 1; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(rig.camera->operations() == std::vector<std::string>{"GetSystemDateAndTime"});

  const auto asked = std::chrono::steady_clock::now();
  rig.watch->stop();
  REQUIRE(waitUntil([&rig] { return rig.watch->finished(); }));
  CHECK(std::chrono::steady_clock::now() - asked < std::chrono::seconds(1));
  CHECK(rig.camera->cancelled() == 0);
  CHECK(rig.camera->operations() == std::vector<std::string>{"GetSystemDateAndTime"});

  CHECK(OnvifMotionWatch::nextBackoffMs(5000, 60000) == 10000);
  CHECK(OnvifMotionWatch::nextBackoffMs(40000, 60000) == 60000);
  CHECK(OnvifMotionWatch::nextBackoffMs(60000, 60000) == 60000);
}

TEST_CASE("a pull that fails ends the motion it had reported, frees the pull point once, then backs off")
{
  Rig rig;
  rig.watch->start();
  REQUIRE(waitUntil([&rig] { return rig.pulling(); }));
  rig.deliver("pull-messages-onset.xml", kStart + kSecond);
  REQUIRE(waitUntil([&rig] { return rig.pulling(); }));
  rig.clock.set(kStart + 3 * kSecond);
  onLoop(rig.thread.getLoop(), [&rig] {
    CHECK(rig.camera->release({.status = 0, .body = {}, .challenge = {}, .error = "connection reset by peer"}));
  });
  REQUIRE(waitUntil([&rig] { return rig.camera->operations().back() == "UnsubscribeRequest"; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(rig.camera->operations().back() == "UnsubscribeRequest");
  CHECK(rig.board.raisesCadence(7, kStart + 3 * kSecond + CameraMotionBoard::kHoldMs - 1));
  CHECK_FALSE(rig.board.raisesCadence(7, kStart + 3 * kSecond + CameraMotionBoard::kHoldMs));
  CHECK(rig.camera->peak() == 1);
}

TEST_CASE("a camera that answers every pull at once is pulled at most five times a second")
{
  for (const int64_t gap : {int64_t{0}, OnvifMotionWatch::kMinPullGapMs}) {
    CAPTURE(gap);
    Rig rig({.firstBackoffMs = 10 * kSecond, .minPullGapMs = gap});
    for (int answered = 0; answered < 6; ++answered)
      rig.camera->once("PullMessagesRequest", answer("pull-messages-empty.xml"));
    const auto started = std::chrono::steady_clock::now();
    rig.watch->start();
    REQUIRE(waitUntil([&rig] { return rig.pulling(); }));
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    const auto operations = rig.camera->operations();
    CHECK(std::ranges::count(operations, std::string("PullMessagesRequest")) == 7);
    MESSAGE("seven pulls, six answered at once, with a gap of " << gap << " ms took " << elapsed.count() << " ms");
    if (gap == 0)
      CHECK(elapsed < std::chrono::milliseconds(1000));
    else
      CHECK(elapsed >= std::chrono::milliseconds(6 * OnvifMotionWatch::kMinPullGapMs));
    CHECK(rig.camera->peak() == 1);
  }
}

TEST_CASE("a camera that publishes no motion topic is parked and never asked again")
{
  Rig rig({.firstBackoffMs = 20, .minPullGapMs = OnvifMotionWatch::kMinPullGapMs});
  rig.camera->always("GetEventPropertiesRequest", answer("get-event-properties-no-motion.xml"));
  rig.watch->start();
  REQUIRE(waitUntil([&rig] { return rig.watch->finished(); }));
  CHECK(rig.watch->parked());
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(rig.camera->operations() ==
        std::vector<std::string>{"GetSystemDateAndTime", "GetCapabilities", "GetEventPropertiesRequest"});
}

TEST_CASE("only enabled ONVIF cameras with an address get a subscription, on the driver's own endpoint")
{
  CameraSchema onvif;
  onvif.id = 3;
  onvif.ip = "192.168.1.70";
  onvif.driver = CameraDriver::Onvif;
  onvif.isEnabled = true;
  onvif.username = "admin";
  onvif.password = "Secret!1";
  onvif.config = R"({"onvifPort":2020})";

  CameraSchema disabled = onvif;
  disabled.id = 4;
  disabled.isEnabled = false;
  CameraSchema tapo = onvif;
  tapo.id = 5;
  tapo.driver = CameraDriver::Tapo;
  CameraSchema nowhere = onvif;
  nowhere.id = 6;
  nowhere.ip.clear();

  const auto targets = OnvifMotionEvents::targetsOf({onvif, disabled, tapo, nowhere});
  REQUIRE(targets.size() == 1);
  CHECK(targets.front().cameraId == 3);
  const OnvifEndpoint expected = OnvifDriver::endpointOf(onvif);
  CHECK(targets.front().endpoint.host == expected.host);
  CHECK(targets.front().endpoint.port == 2020);
  CHECK(targets.front().endpoint.username == "admin");
  CHECK(targets.front().endpoint.password == "Secret!1");
}

TEST_CASE("a stop before any start neither crashes nor starts, and reports drained")
{
  CameraMotionBoard board;
  OnvifMotionEvents events({.board = &board, .active = [] { return true; }});
  events.requestStop();
  CHECK(events.drained());
  events.start();
  events.requestStop();
  CHECK(events.drained());
}
