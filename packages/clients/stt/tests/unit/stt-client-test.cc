#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "fake-stt-server.hxx"

#include <config/config-service.hxx>
#include <cstdint>
#include <doctest/doctest.h>
#include <errors/response-exception.hxx>
#include <chrono>
#include <exception>
#include <stt/stt-client.hxx>
#include <stt/stt-errors.hxx>
#include <string>
#include <stt/stt-remote.hxx>
#include <utility>
#include <vector>

namespace
{

constexpr const char* kDefaultTimeoutMs = "30000";
constexpr const char* kSecret = "rpc-secret";

void pointAt(const std::string& url)
{
  ConfigService::setRuntimeString("stt.remote_url", url);
  ConfigService::setRuntimeString("stt.remote_timeout_ms", kDefaultTimeoutMs);
}

void pointAtRpc(const std::string& target)
{
  ConfigService::setRuntimeString("stt.grpc_target", target);
  ConfigService::setRuntimeString("stt.grpc_credential", kSecret);
}

template <typename Call>
std::string thrownBy(Call call)
{
  try {
    call();
  }
  catch (const std::exception& error) {
    return error.what();
  }
  return "";
}

struct Refusal
{
  int status{0};
  std::string code;
  std::string message;

  bool operator==(const Refusal&) const = default;
};

template <typename Call>
Refusal refusalBy(Call call)
{
  try {
    call();
  }
  catch (const ResponseException& error) {
    return {.status = error.statusCode(),
            .code = error.errorCode(),
            .message = error.what()};
  }
  return {};
}

}

TEST_CASE("The stt client transcribes over the argus-stt wire")
{
  FakeSttServer server;
  pointAt("http://127.0.0.1:" + std::to_string(server.port()));

  const SttRemoteConfig config = SttRemoteConfig::resolve();
  REQUIRE(config.enabled());
  const SttHttpClient client(config.url, config.timeoutMs);
  const std::vector<float> samples(1600, 0.1F);

  CHECK(client.transcribe(samples, "") == "hola default");
  CHECK(server.requests().at("POST /stt/v1/transcribe?lang=") == 1);
  CHECK(client.transcribe(samples, "es") == "hola es");
  CHECK(server.requests().at("POST /stt/v1/transcribe?lang=es") == 1);

  CHECK(kWireSampleRate == 16000);
  CHECK(kPcmScale == 32768.0F);
  CHECK(server.lastBodySize() == samples.size() * sizeof(int16_t));

  const SttHttpClient bare("127.0.0.1:" + std::to_string(server.port()),
                           config.timeoutMs);
  CHECK(bare.transcribe(samples, "en") == "hola en");
  CHECK(server.requests().at("POST /stt/v1/transcribe?lang=en") == 1);

  const SttClient facade;
  CHECK(facade.remote());
  CHECK(facade.transcribe(samples, "es") == "hola es");
  CHECK(server.requests().at("POST /stt/v1/transcribe?lang=es") == 2);

  pointAt("");
}

TEST_CASE("The stt client maps a refusal into the frozen envelope error")
{
  FakeSttServer down(503);
  pointAt("http://127.0.0.1:" + std::to_string(down.port()));
  const SttHttpClient refused(SttRemoteConfig::resolve().url, 1000);
  const std::vector<float> samples(160, 0.1F);

  CHECK(thrownBy([&] { (void)refused.transcribe(samples, "es"); }) ==
        "argus-stt STT_NOT_LOADED: engine down");
  CHECK(down.requests().at("POST /stt/v1/transcribe?lang=es") == 1);

  pointAt("http://127.0.0.1:1");
  const SttHttpClient dead(SttRemoteConfig::resolve().url, 1000);
  CHECK(thrownBy([&] { (void)dead.transcribe(samples, "es"); }) ==
        "argus-stt unreachable at http://127.0.0.1:1");

  pointAt("");
}

TEST_CASE("The stt client validates its input before it dials")
{
  pointAt("http://127.0.0.1:1");
  const SttHttpClient client(SttRemoteConfig::resolve().url, 1000);

  CHECK(thrownBy([&] { (void)client.transcribe({}, "es"); }) ==
        "argus-stt transcribe needs a non-empty body");

  CHECK(thrownBy([&] { (void)SttHttpClient("", 1000); }) ==
        "argus-stt remote_url has no host");
  CHECK_FALSE(SttRemoteConfig{}.enabled());

  pointAt("");
}

TEST_CASE("The stt remote config reads its knobs and keeps its defaults")
{
  CHECK(SttRemoteConfig{}.url.empty());
  CHECK(SttRemoteConfig{}.timeoutMs == 30000);

  pointAt("127.0.0.1:7030");
  const SttRemoteConfig configured = SttRemoteConfig::resolve();
  CHECK(configured.enabled());
  CHECK(configured.url == "127.0.0.1:7030");
  CHECK(configured.timeoutMs == 30000);

  ConfigService::setRuntimeString("stt.remote_timeout_ms", "5000");
  CHECK(SttRemoteConfig::resolve().timeoutMs == 5000);

  ConfigService::setRuntimeString("stt.remote_timeout_ms", "0");
  CHECK(SttRemoteConfig::resolve().timeoutMs == 30000);

  pointAt("");
  CHECK_FALSE(SttRemoteConfig::resolve().enabled());
}

TEST_CASE("The stt gRPC client validates its configuration before it dials")
{
  const auto config = [](std::string target, std::string credential,
                         std::chrono::milliseconds timeout) {
    return argus::stt::ClientConfig{.target = std::move(target),
                                    .credential = std::move(credential),
                                    .timeout = timeout};
  };
  CHECK(refusalBy([&] { (void)argus::stt::Client(config("", kSecret, std::chrono::seconds(5))); }) ==
        Refusal{.status = 400, .code = "BAD_REQUEST",
                .message = "Invalid transcription request"});
  CHECK(refusalBy([&] { (void)argus::stt::Client(config("127.0.0.1:7030", "", std::chrono::seconds(5))); }) ==
        Refusal{.status = 400, .code = "BAD_REQUEST",
                .message = "Invalid transcription request"});
  CHECK(refusalBy([&] { (void)argus::stt::Client(config("127.0.0.1:7030", kSecret, std::chrono::seconds(0))); }) ==
        Refusal{.status = 400, .code = "BAD_REQUEST",
                .message = "timeout must be within 1 and 120000 ms"});
  CHECK(refusalBy([&] { (void)argus::stt::Client(config("127.0.0.1:7030", kSecret, std::chrono::seconds(121))); }) ==
        Refusal{.status = 400, .code = "BAD_REQUEST",
                .message = "timeout must be within 1 and 120000 ms"});

  const argus::stt::Client accepted(
      config("127.0.0.1:7030", kSecret, std::chrono::seconds(120)));
  const auto refused = [&accepted](std::vector<float> samples, int sampleRate) {
    return refusalBy([&accepted, &samples, sampleRate] {
      (void)accepted.transcribe({.samples = std::move(samples),
                                 .sampleRate = sampleRate,
                                 .language = "es",
                                 .cancellation = {}});
    });
  };
  const Refusal badRequest{.status = 400, .code = "BAD_REQUEST",
                           .message = "Invalid transcription request"};
  CHECK(refused({}, 16000) == badRequest);
  CHECK(refused({0.1F}, 0) == badRequest);
  CHECK(refused({0.1F}, 7999) == badRequest);
  CHECK(refused({0.1F}, 192001) == badRequest);
}

TEST_CASE("The stt entry point selects its transport from the runtime knobs")
{
  pointAt("");
  pointAtRpc("");
  SttClient client;
  CHECK_FALSE(client.remote());
  CHECK(thrownBy([&] { (void)client.transcribe({0.1F}, "es"); }) ==
        "stt.remote_url is not configured");

  pointAtRpc("127.0.0.1:1");
  CHECK(client.remote());
  const Refusal refused =
      refusalBy([&] { (void)client.transcribe({0.1F}, "es"); });
  CHECK(refused.status == 503);
  CHECK(refused.code == "SERVICE_UNAVAILABLE");
  CHECK(refused.message == "Service unavailable");

  pointAtRpc("");
  CHECK_FALSE(client.remote());
  CHECK(thrownBy([&] { (void)client.transcribe({0.1F}, "es"); }) ==
        "stt.remote_url is not configured");
}
