#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "tunnel-harness.hxx"

#include <doctest/doctest.h>

#include <algorithm>

using namespace tunnel;
using namespace tunnel::test;

TEST_CASE("loopback carries bytes identically through relay and client")
{
  HarnessOptions options;
  options.gatewayReply = makePayload(100 * 1024, 77);
  Harness harness(std::move(options));
  REQUIRE(harness.start());

  auto device = connectTestPeer({.loop = harness.loop,
                                 .ip = "127.0.0.1",
                                 .port = harness.relay->devicePort()});
  REQUIRE(waitFor([device] { return device->connected.load(); }, 5000));

  const std::string request = makePayload(200 * 1024, 42);
  postSend({.loop = harness.loop, .peer = device->peer, .data = request});

  auto gatewayConns = harness.gatewayConnections();
  REQUIRE(waitFor([&] {
    gatewayConns = harness.gatewayConnections();
    return !gatewayConns.empty() &&
           gatewayConns.front()->received.size() == request.size();
  }, 10000));
  REQUIRE(gatewayConns.front()->received == request);

  const std::string& reply = harness.options_.gatewayReply;
  REQUIRE(waitFor([&] { return device->bytes().size() == reply.size(); },
                  10000));
  CHECK(device->bytes() == reply);

  device->peer->close();
  REQUIRE(waitFor([&] {
    return gatewayConns.front()->eof &&
           harness.relay->streamCount() == 0 &&
           harness.client->streamCount() == 0;
  }, 10000));
  CHECK(harness.relay->pendingBytes() == 0);
  CHECK(harness.client->pendingBytes() == 0);
  harness.stop();
}

TEST_CASE("loopback multiplexes concurrent device streams")
{
  HarnessOptions options;
  options.echoGateway = true;
  Harness harness(std::move(options));
  REQUIRE(harness.start());

  const size_t kStreams = 4;
  const size_t kPayloadSize = 32 * 1024;
  std::vector<std::shared_ptr<TestPeer>> devices;
  std::vector<std::string> payloads;
  for (size_t index = 0; index < kStreams; ++index) {
    payloads.push_back(
        makePayload(kPayloadSize, static_cast<uint32_t>(1000 + index)));
    devices.push_back(connectTestPeer({.loop = harness.loop,
                                 .ip = "127.0.0.1",
                                 .port = harness.relay->devicePort()}));
    REQUIRE(waitFor(
        [peer = devices.back()] { return peer->connected.load(); }, 5000));
    postSend({.loop = harness.loop, .peer = devices.back()->peer, .data = payloads.back()});
  }

  REQUIRE(waitFor([&] {
    return harness.gatewayConnections().size() == kStreams;
  }, 10000));

  REQUIRE(waitFor([&] {
    for (size_t index = 0; index < kStreams; ++index) {
      if (devices[index]->bytes().size() != kPayloadSize)
        return false;
    }
    return true;
  }, 10000));
  for (size_t index = 0; index < kStreams; ++index)
    CHECK(devices[index]->bytes() == payloads[index]);

  for (const auto& device : devices)
    device->peer->close();
  REQUIRE(waitFor([&] {
    return harness.relay->streamCount() == 0 &&
           harness.client->streamCount() == 0;
  }, 10000));
  harness.stop();
}

TEST_CASE("idle streams are swept with a bounded close")
{
  HarnessOptions options;
  options.limits.idleTimeout = std::chrono::seconds(1);
  Harness harness(std::move(options));
  REQUIRE(harness.start());

  auto device = connectTestPeer({.loop = harness.loop,
                                 .ip = "127.0.0.1",
                                 .port = harness.relay->devicePort()});
  REQUIRE(waitFor([device] { return device->connected.load(); }, 5000));
  REQUIRE(waitFor([&] {
    return !harness.gatewayConnections().empty();
  }, 5000));

  CHECK(waitFor([&] {
    return harness.relay->streamCount() == 0 &&
           harness.client->streamCount() == 0;
  }, 8000));
  CHECK(device->eof.load());
  CHECK(harness.gatewayConnections().front()->eof);
  harness.stop();
}

TEST_CASE("stalled gateway read applies back-pressure without byte loss")
{
  HarnessOptions options;
  options.limits.socketSndBuf = 64 * 1024;
  options.slowGatewayReadMs = 20;
  options.gatewayRcvBuf = 64 * 1024;
  Harness harness(std::move(options));
  REQUIRE(harness.start());

  auto device = connectTestPeer({.loop = harness.loop,
                                 .ip = "127.0.0.1",
                                 .port = harness.relay->devicePort(),
                                 .sndBuf = options.limits.socketSndBuf});
  REQUIRE(waitFor([device] { return device->connected.load(); }, 5000));

  const size_t kChunk = 256 * 1024;
  const std::string request = makePayload(4 * 1024 * 1024, 9);
  size_t sent = 0;
  const auto sendNext = [&] {
    const size_t end = std::min(sent + kChunk, request.size());
    postSend({.loop = harness.loop,
              .peer = device->peer,
              .data = request.substr(sent, end - sent)});
    sent = end;
  };
  const auto stalled = [&] {
    return harness.client->pendingBytes() > 0 ||
           harness.relay->pendingBytes() > 0;
  };

  sendNext();
  while (sent < request.size() && !stalled()) {
    sendNext();
    REQUIRE(waitFor([&] {
      return device->peer->sendBufferBytes() < kChunk / 2 || stalled();
    }, 10000));
  }
  REQUIRE(waitFor(stalled, 10000));

  while (sent < request.size() && harness.relay->pendingBytes() == 0) {
    sendNext();
    REQUIRE(waitFor([&] {
      return device->peer->sendBufferBytes() < kChunk / 2 ||
             harness.relay->pendingBytes() > 0;
    }, 10000));
  }
  CHECK(waitFor([&] { return harness.relay->pendingBytes() > 0; }, 10000));

  const size_t relayPending = harness.relay->pendingBytes();
  const size_t clientPending = harness.client->pendingBytes();
  CHECK(relayPending <= 2 * harness.options_.limits.streamPendingCap);
  CHECK(clientPending <= 3 * harness.options_.limits.streamPendingCap);

  auto conns = harness.gatewayConnections();
  while (sent < request.size()) {
    sendNext();
    REQUIRE(waitFor([&] {
      return device->peer->sendBufferBytes() < kChunk / 2;
    }, 30000));
  }
  REQUIRE(waitFor([&] {
    conns = harness.gatewayConnections();
    return !conns.empty() &&
           conns.front()->received.size() == request.size() &&
           harness.client->pendingBytes() == 0 &&
           harness.relay->pendingBytes() == 0;
  }, 60000));
  CHECK(conns.front()->received == request);

  device->peer->close();
  harness.stop();
}
