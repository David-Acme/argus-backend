#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "link-harness.hxx"

#include <link/client/link-client.hxx>
#include <link/protocol/frames.hxx>
#include <link/tls/pinned-key.hxx>

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

using namespace tunnel;
using namespace tunnel::test;

namespace
{
constexpr std::string_view kHomeId = "11111111111111111111111111111111";
constexpr std::string_view kOtherHomeId = "22222222222222222222222222222222";
constexpr std::string_view kNodeName = "home.argus-tunnel.lynk.page";

struct TestPki
{
  std::filesystem::path directory;
  TestIdentity node;
  TestIdentity home;
};

TestPki makePki()
{
  TestPki pki;
  pki.directory = makeTempDirectory("argus-tunnel-link");
  pki.node = mintIdentity({.directory = pki.directory,
                           .name = "node",
                           .commonName = std::string{kNodeName}});
  pki.home = mintIdentity({.directory = pki.directory,
                           .name = "home",
                           .commonName = std::string{kHomeId}});
  return pki;
}

FakeNodeOptions nodeOptionsOf(const TestIdentity& node)
{
  FakeNodeOptions options;
  options.certificatePath = node.certificatePath;
  options.keyPath = node.keyPath;
  return options;
}

LinkFixtureOptions fixtureOptions(const TestIdentity& home,
                                  const TestIdentity& node)
{
  LinkFixtureOptions options;
  options.home = home;
  options.node = node;
  options.nodeOptions = nodeOptionsOf(node);
  return options;
}

class RecordingObserver : public link::LinkObserver
{
public:
  void onLinkUp() override { ++ups; }
  void onLinkDown() override { ++downs; }
  void onGoingAway(link::GoAwayReason reason, uint16_t seconds) override
  {
    (void)reason;
    (void)seconds;
    ++goingAway;
  }

  void onWireError(link::WireError error) override
  {
    lastWireError.store(error, std::memory_order_relaxed);
    ++wireErrors;
  }

  std::atomic<int> ups{0};
  std::atomic<int> downs{0};
  std::atomic<int> goingAway{0};
  std::atomic<int> wireErrors{0};
  std::atomic<link::WireError> lastWireError{link::WireError::BadMagic};
};

struct DrainInput
{
  FakeNode& node;
  uint32_t streamId{0};
  size_t expected{0};
  size_t& granted;
  int timeoutMs{0};
};

bool drainFrames(const DrainInput& input)
{
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds{input.timeoutMs};
  size_t received = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto frame = input.node.nextFrame(50);
    if (!frame)
      continue;
    if (frame->type == link::FrameType::Data &&
        frame->streamId == input.streamId)
      received += frame->payload.size();
    if (frame->type == link::FrameType::Credit &&
        frame->streamId == input.streamId)
      input.granted += link::decodeCredit(*frame);
    if (received >= input.expected)
      return true;
  }
  return received >= input.expected;
}
}

TEST_SUITE("home link v2 client")
{
  TEST_CASE("the home client brings up a mutual TLS 1.3 link and exchanges "
            "HELLO")
  {
    const TestPki pki = makePki();
    LinkFixture fixture{fixtureOptions(pki.home, pki.node)};
    REQUIRE(fixture.waitConnected(5000));
    CHECK(fixture.node().state().handshaken);
    CHECK(fixture.node().state().peerCommonName == kHomeId);
    CHECK(fixture.node().state().helloSeen);

    REQUIRE(fixture.clientHello().has_value());
    const link::Frame hello = fixture.clientHello().value_or(link::Frame{});
    CHECK(hello.type == link::FrameType::Hello);
    CHECK(hello.streamId == 0);
    const link::HelloPayload payload = link::decodeHello(hello);
    CHECK(payload.version == link::kWireVersion);
    CHECK(payload.reserved == 0);
    CHECK((payload.capabilities & link::capability::kStreamCredit) != 0);
    const uint32_t both =
        link::capability::kPushIntents | link::capability::kStreamCredit;
    CHECK(payload.capabilities == both);
    CHECK(fixture.client().remoteCapabilities() == both);
  }

  TEST_CASE("a node whose key is not the pinned one is refused")
  {
    const TestPki pki = makePki();
    const TestIdentity other = mintIdentity({.directory = pki.directory,
                                             .name = "other",
                                             .commonName =
                                                 std::string{kOtherHomeId}});
    RecordingObserver observer;
    LinkFixtureOptions options = fixtureOptions(pki.home, pki.node);
    options.link.reconnectBaseMs = 20;
    options.link.reconnectCapMs = 50;
    options.pinnedSubjectPublicKeyDer =
        tunnel::link::subjectPublicKeyDerOfPem(other.publicKeyPem)
            .value_or(std::string{});
    options.observer = &observer;
    LinkFixture fixture{options};
    CHECK_FALSE(fixture.waitConnected(1500));
    CHECK_FALSE(fixture.client().connected());
    CHECK(fixture.client().reconnectAttempts() > 0);
    CHECK(observer.ups.load() == 0);
  }

  TEST_CASE("a stream the node opens carries the app's bytes to the home "
            "listener and back")
  {
    const TestPki pki = makePki();
    LinkFixture fixture{fixtureOptions(pki.home, pki.node)};
    REQUIRE(fixture.waitConnected(5000));

    REQUIRE(fixture.node().sendFrame(link::encodeOpen(7)));
    CHECK(waitFor([&fixture] { return fixture.echo().accepted() == 1; }, 5000));
    CHECK(waitFor([&fixture] { return fixture.client().streamCount() == 1; },
                  5000));

    const std::string payload = "hola, casa";
    REQUIRE(fixture.node().sendFrame(link::encodeData(7, payload)));
    CHECK(waitFor(
        [&fixture, &payload] {
          return fixture.echo().received() == payload.size();
        },
        5000));

    const link::Frame echoed = fixture.awaitFrame(5000);
    REQUIRE(echoed.type == link::FrameType::Data);
    CHECK(echoed.streamId == 7);
    CHECK(echoed.payload == payload);

    REQUIRE(fixture.node().sendFrame(
        link::encodeClose(7, link::CloseReason::Normal)));
    CHECK(waitFor([&fixture] { return fixture.client().streamCount() == 0; },
                  5000));
    CHECK(fixture.node().state().violations.empty());
  }

  TEST_CASE("a stream opened for a listener that is not there is closed busy")
  {
    const TestPki pki = makePki();
    LinkFixtureOptions options = fixtureOptions(pki.home, pki.node);
    options.gatewayPort = 1;
    LinkFixture fixture{options};
    REQUIRE(fixture.waitConnected(5000));

    REQUIRE(fixture.node().sendFrame(link::encodeOpen(11)));
    const link::Frame closed = fixture.awaitFrame(5000);
    REQUIRE(closed.type == link::FrameType::Close);
    CHECK(closed.streamId == 11);
    CHECK(link::decodeCloseReason(closed) == link::CloseReason::Busy);
    CHECK(fixture.client().connected());
    CHECK(fixture.client().streamCount() == 0);
  }

  TEST_CASE("the keepalive is short while a device is connected and long when "
            "everyone is at home")
  {
    const TestPki pki = makePki();
    LinkFixtureOptions options = fixtureOptions(pki.home, pki.node);
    options.link.keepaliveActive = std::chrono::seconds{1};
    options.link.keepaliveIdle = std::chrono::seconds{4};
    options.link.keepalivePongTimeout = std::chrono::seconds{2};
    LinkFixture fixture{options};
    REQUIRE(fixture.waitConnected(5000));

    CHECK_FALSE(fixture.client().keepaliveActive());
    fixture.drainFrames();
    CHECK(fixture.node().nextFrame(2000) == std::nullopt);

    REQUIRE(fixture.node().sendFrame(link::encodeOpen(3)));
    CHECK(waitFor([&fixture] { return fixture.echo().accepted() == 1; }, 5000));
    CHECK(waitFor([&fixture] { return fixture.client().keepaliveActive(); },
                  3000));

    bool pinged = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{3500};
    while (std::chrono::steady_clock::now() < deadline && !pinged) {
      const auto frame = fixture.node().nextFrame(100);
      if (frame && frame->type == link::FrameType::Ping)
        pinged = true;
    }
    CHECK(pinged);
    CHECK(fixture.client().connected());

    REQUIRE(fixture.node().sendFrame(
        link::encodeClose(3, link::CloseReason::Normal)));
    CHECK(waitFor([&fixture] { return fixture.client().streamCount() == 0; },
                  5000));
    CHECK(waitFor([&fixture] { return !fixture.client().keepaliveActive(); },
                  3000));
  }

  TEST_CASE("a ping the node never answers declares the link dead")
  {
    const TestPki pki = makePki();
    RecordingObserver observer;
    LinkFixtureOptions options = fixtureOptions(pki.home, pki.node);
    options.nodeOptions.answerPings = false;
    options.link.keepaliveActive = std::chrono::seconds{1};
    options.link.keepalivePongTimeout = std::chrono::seconds{1};
    options.link.reconnectBaseMs = 30;
    options.link.reconnectCapMs = 60;
    options.observer = &observer;
    LinkFixture fixture{options};
    REQUIRE(fixture.waitConnected(5000));
    REQUIRE(fixture.node().sendFrame(link::encodeOpen(5)));
    CHECK(waitFor([&fixture] { return fixture.echo().accepted() == 1; }, 5000));

    CHECK(waitFor([&fixture] { return !fixture.client().connected(); }, 6000));
    CHECK(waitFor([&observer] { return observer.downs.load() >= 1; }, 3000));
    CHECK(fixture.client().reconnectAttempts() >= 1);
  }

  TEST_CASE("GOAWAY drains the link and the next attempt starts at once")
  {
    const TestPki pki = makePki();
    const auto started = std::chrono::steady_clock::now();
    LinkFixtureOptions options = fixtureOptions(pki.home, pki.node);
    options.nodeOptions.acceptReconnects = true;
    options.link.reconnectBaseMs = 30000;
    options.link.reconnectCapMs = 60000;
    LinkFixture fixture{options};
    REQUIRE(fixture.waitConnected(5000));

    REQUIRE(fixture.node().sendFrame(
        link::encodeGoAway(link::GoAwayReason::NodeDrain, 1)));
    CHECK(waitFor([&fixture] { return fixture.client().draining(); }, 3000));

    fixture.node().closeConnection();
    CHECK(waitFor([&fixture] { return fixture.node().connections() == 2; },
                  1000));
    CHECK(waitFor([&fixture] { return fixture.client().connected(); }, 5000));
    CHECK_FALSE(fixture.client().draining());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    CHECK(fixture.client().lastReconnectDelayMs() <
          static_cast<int>(elapsed.count()));
    CHECK(fixture.client().reconnectAttempts() == 0);
  }

  TEST_CASE("the negotiated credit window bounds what the home sends")
  {
    const TestPki pki = makePki();
    LinkFixture fixture{fixtureOptions(pki.home, pki.node)};
    REQUIRE(fixture.waitConnected(5000));
    REQUIRE(fixture.node().sendFrame(link::encodeOpen(9)));
    CHECK(waitFor([&fixture] { return fixture.echo().accepted() == 1; }, 5000));

    const std::string chunk(static_cast<size_t>(64) * 1024, 'x');
    for (uint32_t index = 0; index < 4; ++index)
      REQUIRE(fixture.node().sendFrame(link::encodeData(9, chunk)));

    const size_t window = static_cast<size_t>(4) * chunk.size();
    size_t granted = 0;
    CHECK(drainFrames({.node = fixture.node(),
                       .streamId = 9,
                       .expected = window,
                       .granted = granted,
                       .timeoutMs = 8000}));
    std::this_thread::sleep_for(std::chrono::milliseconds{300});

    size_t extra = 0;
    while (const auto frame = fixture.node().nextFrame(20)) {
      if (frame->type == link::FrameType::Data && frame->streamId == 9)
        extra += frame->payload.size();
    }
    CHECK(extra == 0);
    CHECK(granted == window);
    CHECK(fixture.client().connected());
  }

  TEST_CASE("the home grants credit as it consumes the node's bytes")
  {
    const TestPki pki = makePki();
    LinkFixture fixture{fixtureOptions(pki.home, pki.node)};
    REQUIRE(fixture.waitConnected(5000));
    REQUIRE(fixture.node().sendFrame(link::encodeOpen(13)));
    CHECK(waitFor([&fixture] { return fixture.echo().accepted() == 1; }, 5000));

    const std::string chunk(static_cast<size_t>(64) * 1024, 'x');
    for (uint32_t index = 0; index < 4; ++index)
      REQUIRE(fixture.node().sendFrame(link::encodeData(13, chunk)));

    size_t granted = 0;
    const size_t total = static_cast<size_t>(4) * chunk.size();
    CHECK(waitFor(
        [&] {
          return drainFrames({.node = fixture.node(),
                               .streamId = 13,
                               .expected = total,
                               .granted = granted,
                               .timeoutMs = 1000}) &&
                 granted >= total;
        },
        8000));
    CHECK(granted == total);
  }

  TEST_CASE("a frame before HELLO refuses the link")
  {
    const TestPki pki = makePki();
    RecordingObserver observer;
    LinkFixtureOptions options = fixtureOptions(pki.home, pki.node);
    options.nodeOptions.sendHello = false;
    options.observer = &observer;
    LinkFixture fixture{options};
    REQUIRE(waitFor([&fixture] { return fixture.node().state().handshaken; },
                    5000));
    REQUIRE(fixture.node().sendFrame(link::encodeOpen(3)));
    CHECK(waitFor([&observer] { return observer.wireErrors.load() >= 1; },
                  3000));
    CHECK(observer.lastWireError.load() == link::WireError::HelloRequired);
    CHECK_FALSE(fixture.client().connected());

    const link::Frame mine = fixture.awaitFrame(2000);
    REQUIRE(mine.type == link::FrameType::Hello);
    const link::Frame goAway = fixture.awaitFrame(2000);
    REQUIRE(goAway.type == link::FrameType::GoAway);
    CHECK(link::decodeGoAway(goAway).reason ==
          link::GoAwayReason::ProtocolError);
  }

  TEST_CASE("a second HELLO refuses the link")
  {
    const TestPki pki = makePki();
    RecordingObserver observer;
    LinkFixtureOptions options = fixtureOptions(pki.home, pki.node);
    options.observer = &observer;
    LinkFixture fixture{options};
    REQUIRE(fixture.waitConnected(5000));
    REQUIRE(fixture.node().sendFrame(link::encodeHello(
        link::capability::kPushIntents | link::capability::kStreamCredit)));
    CHECK(waitFor([&observer] { return observer.wireErrors.load() >= 1; },
                  3000));
    CHECK(observer.lastWireError.load() == link::WireError::DuplicateHello);
    CHECK(waitFor([&fixture] { return !fixture.client().connected(); }, 3000));
  }

  TEST_CASE("the reconnect backoff is a full jitter inside the doubling window")
  {
    CHECK(link::linkReconnectDelayMs(
              {.attempt = 1, .baseMs = 1000, .capMs = 60000, .jitter = 0.0}) ==
          0);
    CHECK(link::linkReconnectDelayMs(
              {.attempt = 1, .baseMs = 1000, .capMs = 60000, .jitter = 0.5}) ==
          500);
    CHECK(link::linkReconnectDelayMs(
              {.attempt = 1, .baseMs = 1000, .capMs = 60000, .jitter = 1.0}) ==
          1000);
    CHECK(link::linkReconnectDelayMs(
              {.attempt = 2, .baseMs = 1000, .capMs = 60000, .jitter = 1.0}) ==
          2000);
    CHECK(link::linkReconnectDelayMs(
              {.attempt = 6, .baseMs = 1000, .capMs = 60000, .jitter = 1.0}) ==
          32000);
    CHECK(link::linkReconnectDelayMs(
              {.attempt = 7, .baseMs = 1000, .capMs = 60000, .jitter = 1.0}) ==
          60000);
    CHECK(link::linkReconnectDelayMs(
              {.attempt = 40, .baseMs = 1000, .capMs = 60000, .jitter = 1.0}) ==
          60000);
  }
}
