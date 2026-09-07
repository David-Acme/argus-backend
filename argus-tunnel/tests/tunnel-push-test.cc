#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <tunnel-harness.hxx>

#include <doctest/doctest.h>

using namespace tunnel;
using namespace tunnel::test;

TEST_CASE("push intents reach the client queue over an authenticated link")
{
  Harness harness({});
  REQUIRE(harness.start());

  harness.loop.post([&harness] {
    harness.relay->postPushIntent(R"({"notificationId":17})");
  });
  REQUIRE(waitFor([&] { return harness.client->pushReceived() == 1; }, 5000));
  CHECK(harness.client->pushQueued() == 1);
  CHECK(harness.relay->pushForwarded() == 1);
  CHECK(harness.relay->pushDropped() == 0);
}

namespace
{
// Mutex-guarded holder for a client created on the loop thread, so the test
// thread can poll its counters without racing the pointer itself.
struct ClientHolder
{
  std::mutex mutex;
  std::unique_ptr<TunnelClient> client;
};
} // namespace

TEST_CASE("intents posted while the link is down deliver after reconnect")
{
  auto restarted = std::make_shared<ClientHolder>();
  Harness harness({});
  REQUIRE(harness.start());

  harness.loop.post([&harness] { harness.relay->dropLink(); });
  REQUIRE(waitFor([&] { return !harness.relay->hasHome(); }, 5000));

  for (int i = 0; i < 2; ++i) {
    harness.loop.post([&harness, i] {
      harness.relay->postPushIntent(R"({"notificationId":)" +
                                    std::to_string(i) + "}");
    });
  }
  // No active link: the intents buffer at the relay instead of forwarding.
  REQUIRE(waitFor([&] { return harness.relay->pushQueued() == 2; }, 5000));
  CHECK(harness.relay->pushForwarded() == 0);

  // A restarted client re-authenticates and the queue drains.
  harness.loop.post([&harness, restarted] {
    ClientOptions options;
    options.relayHost = "127.0.0.1";
    options.relayPort = harness.relay->homePort();
    options.gatewayHost = "127.0.0.1";
    options.gatewayPort = 1;
    options.secret = harness.options_.secret;
    options.reconnectWaitMs = 50;
    options.limits = harness.options_.limits;
    options.pushQueueCapacity = harness.options_.clientPushCapacity;
    restarted->client = std::make_unique<TunnelClient>(harness.loop,
                                                       std::move(options));
    restarted->client->start();
  });
  REQUIRE(waitFor([&] {
    std::lock_guard<std::mutex> lock(restarted->mutex);
    return restarted->client && restarted->client->pushReceived() == 2;
  }, 20000));
  CHECK(harness.relay->pushQueued() == 0);
  CHECK(harness.relay->pushForwarded() == 2);

  // Stop the replacement before the harness tears the loop down.
  harness.loop.post([restarted] {
    std::lock_guard<std::mutex> lock(restarted->mutex);
    if (restarted->client)
      restarted->client->stop();
  });
  waitFor([&] { return !harness.relay->hasHome(); }, 5000);
}

TEST_CASE("the relay drops past capacity with accounting and no crash")
{
  // Capacity 4; the client is stopped so the link stays down and nothing
  // drains the queue.
  Harness small({});
  small.options_.relayPushCapacity = 4;
  REQUIRE(small.start());
  small.client->stop();
  small.loop.post([&small] { small.relay->dropLink(); });
  REQUIRE(waitFor([&] { return !small.relay->hasHome(); }, 5000));

  for (int i = 0; i < 6; ++i) {
    small.loop.post([&small, i] {
      small.relay->postPushIntent("intent-" + std::to_string(i));
    });
  }
  REQUIRE(waitFor([&] { return small.relay->pushReceived() == 6; }, 5000));
  CHECK(small.relay->pushQueued() == 4);
  CHECK(small.relay->pushDropped() == 2);
}

TEST_CASE("oversized and empty intents are dropped at the relay ingress")
{
  Harness harness({});
  REQUIRE(harness.start());

  const std::string oversized(kMaxPayload + 1, 'x');
  harness.loop.post([&harness, oversized] {
    harness.relay->postPushIntent(oversized);
    harness.relay->postPushIntent("");
  });
  REQUIRE(waitFor([&] { return harness.relay->pushReceived() == 2; }, 5000));
  CHECK(harness.relay->pushQueued() == 0);
  CHECK(harness.relay->pushDropped() == 2);
  CHECK(harness.relay->pushForwarded() == 0);
}

TEST_CASE("the client queue drops past capacity with accounting")
{
  HarnessOptions options;
  options.clientPushCapacity = 2;
  Harness harness(options);
  REQUIRE(harness.start());

  for (int i = 0; i < 3; ++i) {
    harness.loop.post([&harness, i] {
      harness.relay->postPushIntent("intent-" + std::to_string(i));
    });
  }
  REQUIRE(waitFor([&] { return harness.client->pushReceived() == 3; }, 5000));
  CHECK(harness.client->pushQueued() == 2);
  CHECK(harness.client->pushDropped() == 1);
}
