#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "tunnel-harness.hxx"

#include <doctest/doctest.h>

using namespace tunnel;
using namespace tunnel::test;

TEST_CASE("home link reconnect tears down streams and recovers")
{
  HarnessOptions options;
  options.echoGateway = true;
  Harness harness(std::move(options));
  REQUIRE(harness.start());

  auto device = connectTestPeer(harness.loop, "127.0.0.1",
                                harness.relay->devicePort());
  REQUIRE(waitFor([device] { return device->connected.load(); }, 5000));

  const std::string first = makePayload(16 * 1024, 11);
  postSend(harness.loop, device->peer, first);
  REQUIRE(waitFor([&] { return device->bytes().size() == first.size(); },
                  10000));
  CHECK(device->bytes() == first);
  CHECK(harness.client->reconnectAttempts() == 0);

  // Dropping the home link on the relay side tears down every device
  // stream: streams do not survive a reconnect.
  harness.loop.post([&harness] { harness.relay->dropLink(); });
  REQUIRE(waitFor([&] { return device->eof.load(); }, 10000));
  REQUIRE(waitFor([&] {
    return harness.client->reconnectAttempts() >= 1;
  }, 10000));
  REQUIRE(waitFor([&] { return harness.client->homeActive(); }, 10000));
  CHECK_FALSE(harness.client->gaveUp());
  REQUIRE(waitFor([&] {
    return harness.relay->streamCount() == 0 &&
           harness.client->streamCount() == 0;
  }, 10000));

  auto recovered = connectTestPeer(harness.loop, "127.0.0.1",
                                   harness.relay->devicePort());
  REQUIRE(waitFor([recovered] { return recovered->connected.load(); }, 5000));

  const std::string second = makePayload(24 * 1024, 22);
  postSend(harness.loop, recovered->peer, second);
  REQUIRE(waitFor([&] { return recovered->bytes().size() == second.size(); },
                  10000));
  CHECK(recovered->bytes() == second);
  CHECK(harness.gatewayConnections().size() == 2);

  recovered->peer->close();
  harness.stop();
}

TEST_CASE("auth rejection keeps the client reconnecting")
{
  HarnessOptions options;
  Harness harness(std::move(options));
  REQUIRE(harness.start());

  // A wrong secret must never activate the link.
  RelayOptions relayOptions;
  relayOptions.host = "127.0.0.1";
  relayOptions.devicePort = 0;
  relayOptions.homePort = 0;
  relayOptions.secret = "f5-4-loopback-secret";
  auto rogue = std::make_unique<TunnelRelay>(harness.loop,
                                             std::move(relayOptions));
  REQUIRE(rogue->start());
  CHECK(rogue->homePort() != harness.relay->homePort());

  auto client2 = std::make_unique<TunnelClient>(harness.loop, [&] {
    ClientOptions clientOptions;
    clientOptions.relayPort = rogue->homePort();
    clientOptions.reconnectWaitMs = 20;
    clientOptions.maxReconnects = 3;
    clientOptions.secret = "wrong-secret";
    return clientOptions;
  }());
  harness.loop.post([&] { client2->start(); });

  CHECK(waitFor([&] { return client2->gaveUp(); }, 10000));
  CHECK_FALSE(client2->homeActive());

  // Stop the loop before destroying the manual components: their posted
  // stop tasks would otherwise run against dangling pointers.
  harness.stop();
  rogue.reset();
  client2.reset();
}
