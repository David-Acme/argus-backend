#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "fake-stt-server.hxx"

#include <config/config-service.hxx>
#include <cstdint>
#include <doctest/doctest.h>
#include <exception>
#include <string>
#include <stt/stt-remote.hxx>
#include <vector>

namespace
{

constexpr const char* kDefaultTimeoutMs = "30000";

void pointAt(const std::string& url)
{
  ConfigService::setRuntimeString("stt.remote_url", url);
  ConfigService::setRuntimeString("stt.remote_timeout_ms", kDefaultTimeoutMs);
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
