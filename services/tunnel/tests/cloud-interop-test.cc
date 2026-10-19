#include "interop-support.hxx"
#include "link-harness.hxx"

#include <link/client/link-client.hxx>
#include <link/protocol/frames.hxx>
#include <link/tls/pinned-key.hxx>
#include <net/poll-loop.hxx>

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace tunnel;
using namespace tunnel::test;

namespace
{
constexpr std::string_view kHomeId = "9f9f9f9f9f9f9f9f9f9f9f9f9f9f9f9f";
constexpr std::string_view kNodeName = "home.argus-tunnel.lynk.page";

bool cloudRelayAvailable()
{
  const char* path = std::getenv("ARGUS_CLOUD_RELAY_BIN");
  return path != nullptr && std::filesystem::exists(path);
}

std::string relayBinary()
{
  const char* path = std::getenv("ARGUS_CLOUD_RELAY_BIN");
  return path == nullptr ? std::string{} : std::string{path};
}

struct CloudRun
{
  std::filesystem::path directory;
  TestIdentity node;
  TestIdentity home;
  TestIdentity carried;
  uint16_t homePort{0};
  uint16_t devicePort{0};
  ChildProcess relay;
  std::unique_ptr<HomeTlsServer> homeServer;
  PollLoop loop;
  std::unique_ptr<link::LinkClient> client;
  std::thread loopThread;

  CloudRun()
  {
    directory = makeTempDirectory("argus-tunnel-interop");
    node = mintIdentity({.directory = directory,
                         .name = "node",
                         .commonName = std::string{kNodeName}});
    home = mintIdentity({.directory = directory,
                         .name = "home",
                         .commonName = std::string{kHomeId}});
    carried = mintIdentity({.directory = directory,
                            .name = "carried",
                            .commonName = std::string{kHomeId} + "." +
                                        std::string{"argus-tunnel.lynk.page"}});
    homePort = freeLoopbackPort();
    devicePort = freeLoopbackPort();
  }

  ~CloudRun()
  {
    if (client != nullptr)
      client->stop();
    loop.stop();
    if (loopThread.joinable())
      loopThread.join();
    client.reset();
    homeServer.reset();
    relay.terminate();
  }

  [[nodiscard]] std::string configText() const
  {
    return "[listeners]\nhome_host = \"127.0.0.1\"\nhome_port = " +
           std::to_string(homePort) +
           "\ndevice_host = \"127.0.0.1\"\ndevice_port = " +
           std::to_string(devicePort) +
           "\n\n[tls]\ncertificate_path = \"" + node.certificatePath +
           "\"\nkey_path = \"" + node.keyPath +
           "\"\n\n[links]\ndrain_grace_seconds = 2\n\n[logging]\nlevel = "
           "\"info\"\n";
  }

  bool startRelay()
  {
    const std::filesystem::path configPath = directory / "cloud.toml";
    if (!writeFile(configPath, configText())) {
      MESSAGE("cannot write the relay configuration");
      return false;
    }
    if (!relay.spawn({relayBinary(), configPath.string()}))
      return false;
    return waitForPort(homePort, std::chrono::milliseconds{10000}) &&
           waitForPort(devicePort, std::chrono::milliseconds{10000});
  }

  bool startClient()
  {
    homeServer =
        std::make_unique<HomeTlsServer>(HomeTlsServer::Params{
            .certificatePath = carried.certificatePath,
            .keyPath = carried.keyPath});
    link::LinkOptions options;
    options.host = "127.0.0.1";
    options.port = homePort;
    options.gatewayHost = "127.0.0.1";
    options.gatewayPort = homeServer->port();
    options.certificatePath = home.certificatePath;
    options.keyPath = home.keyPath;
    options.pinnedSubjectPublicKeyDer =
        link::subjectPublicKeyDerOfFile(node.certificatePath)
            .value_or(std::string{});
    options.reconnectBaseMs = 50;
    options.reconnectCapMs = 200;
    const link::LinkClient::Deps deps{
        .loop = loop, .options = std::move(options), .observer = nullptr};
    client = std::make_unique<link::LinkClient>(deps);
    loopThread = std::thread([this] { loop.run(); });
    client->start();
    return waitFor([this] { return client->connected(); }, 10000);
  }
};
}

TEST_CASE("the home client and the cloud relay agree on the wire"
          * doctest::skip(!cloudRelayAvailable()))
{
  CloudRun run;
  REQUIRE(run.startRelay());
  REQUIRE(run.startClient());
  CHECK(run.client->reconnectAttempts() == 0);
  CHECK((run.client->remoteCapabilities() & link::capability::kStreamCredit) !=
        0);
  CHECK(run.client->remoteCapabilities() == link::capability::kStreamCredit);

  const std::string deviceName =
      std::string{kHomeId} + ".argus-tunnel.lynk.page";
  TunnelApp app({.host = "127.0.0.1",
                 .port = run.devicePort,
                 .serverName = deviceName,
                 .pinnedCertificatePath = run.carried.certificatePath});
  REQUIRE_MESSAGE(app.handshaken(), app.lastError());
  CHECK(app.request({.bytes = "Hola",
                     .expected = 4,
                     .timeout = std::chrono::milliseconds{5000}}) ==
        "Hola");
  CHECK(waitFor([&run] { return run.homeServer->accepted() == 1; }, 5000));
  CHECK(waitFor([&run] { return run.homeServer->echoed() >= 4; }, 5000));
  CHECK(waitFor([&run] { return run.client->streamCount() >= 1; }, 5000));

  run.relay.requestStop();
  CHECK(waitFor([&run] { return run.client->draining(); }, 10000));
}
