#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <chrono>
#include <config/config-service.hxx>
#include <doctest/doctest.h>
#include <errors/response-exception.hxx>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <tts/tts-client.hxx>
#include <tts/tts-remote.hxx>
#include <tts/tts-wire.hxx>
#include <vector>

// Pins what the argus-tts client refuses at construction, how its two
// flavours resolve the endpoint they speak to, and the wire vocabulary.
namespace
{

const TtsRequest kAsk{.text = "hola",
                      .lang = TtsLang::EN,
                      .voiceId = "M3",
                      .quality = TtsQuality::Auto,
                      .speed = 1.05F};

argus::tts::ClientConfig configFor(const std::string& target,
                                   const std::string& credential,
                                   std::chrono::milliseconds timeout)
{
  return {.target = target, .credential = credential, .timeout = timeout};
}

// A configuration the gRPC client must refuse before it dials anything.
struct BadConfig
{
  const char* label;
  const char* target;
  const char* credential;
  std::chrono::milliseconds timeout;
};

const std::vector<BadConfig> kBadConfigs{
    {"an empty target", "", "fleet", std::chrono::seconds(30)},
    {"an empty credential", "127.0.0.1:7029", "", std::chrono::seconds(30)},
    {"a zero timeout", "127.0.0.1:7029", "fleet", std::chrono::seconds(0)},
    {"a timeout over two minutes", "127.0.0.1:7029", "fleet",
     std::chrono::seconds(121)},
};

// The refusal a construction raised, or zeros when it raised none.
struct Refusal
{
  int status{0};
  std::string code;
};

Refusal refusalOf(const argus::tts::ClientConfig& config)
{
  try {
    const argus::tts::Client client(config);
    (void)client;
  }
  catch (const ResponseException& error) {
    return {.status = error.statusCode(), .code = error.errorCode()};
  }
  return {};
}

// The what() of the runtime_error an action raised, or an empty string.
std::string failureOf(const std::function<void()>& action)
{
  try {
    action();
  }
  catch (const std::runtime_error& error) {
    return error.what();
  }
  return "";
}

// The what() of the refusal a remote URL raised, or an empty string.
std::string urlRefusal(const std::string& url)
{
  return failureOf([&] { (void)TtsHttpClient(url, 30000); });
}

// The tts knobs are process-global: every case leaves them as it found them.
void pointAt(const std::string& url, int timeoutMs)
{
  ConfigService::setRuntimeString("tts.remote_url", url);
  ConfigService::setRuntimeString("tts.remote_timeout_ms",
                                  std::to_string(timeoutMs));
}

void forgetEndpoint()
{
  pointAt("", 30000);
  ConfigService::setRuntimeString("tts.grpc_target", "");
  ConfigService::setRuntimeString("tts.grpc_credential", "");
}

} // namespace

TEST_CASE("the gRPC client refuses a configuration it cannot dial")
{
  for (const auto& bad : kBadConfigs) {
    CAPTURE(bad.label);
    const auto refusal =
        refusalOf(configFor(bad.target, bad.credential, bad.timeout));
    CHECK(refusal.status == 400);
    CHECK(refusal.code == "BAD_REQUEST");
  }

  // The gate is "over two minutes", so two minutes exactly still dials.
  CHECK_NOTHROW(argus::tts::Client(
      configFor("127.0.0.1:7029", "fleet", std::chrono::seconds(120))));
}

TEST_CASE("the HTTP client refuses a remote_url with no host")
{
  // The constructor is the only gate on the URL: no host, no dial.
  CHECK(urlRefusal("") == "argus-tts remote_url has no host");
  CHECK(urlRefusal("http://") == "argus-tts remote_url has no host");
  CHECK(urlRefusal("/tts/v1/config") == "argus-tts remote_url has no host");

  // A host is enough: the URL parses, and nothing is dialled here.
  CHECK(urlRefusal("127.0.0.1:7029").empty());
  CHECK(urlRefusal("http://argus-tts:7029").empty());
}

TEST_CASE("the remote endpoint resolves from the runtime configuration")
{
  forgetEndpoint();
  const auto unset = TtsRemoteConfig::resolve();
  CHECK(unset.url.empty());
  CHECK_FALSE(unset.enabled());
  CHECK(unset.timeoutMs == 30000);

  // The shipped configs write a scheme-less host:port, which is what the
  // four [tts] blocks in the repo carry.
  pointAt("127.0.0.1:7029", 30000);
  const auto pointed = TtsRemoteConfig::resolve();
  CHECK(pointed.url == "127.0.0.1:7029");
  CHECK(pointed.enabled());

  pointAt("127.0.0.1:7029", 1234);
  CHECK(TtsRemoteConfig::resolve().timeoutMs == 1234);

  // A zero is not a timeout: the default stands rather than disabling.
  pointAt("127.0.0.1:7029", 0);
  CHECK(TtsRemoteConfig::resolve().timeoutMs == 30000);
  forgetEndpoint();
}

TEST_CASE("the entry point refuses to work with no endpoint configured")
{
  forgetEndpoint();
  const TtsClient client;
  CHECK_FALSE(client.remote());

  const std::string refused = "tts.remote_url is not configured";
  CHECK(failureOf([&] { client.defaultSpeed(); }) == refused);
  CHECK(failureOf([&] { client.sampleRate(); }) == refused);
  CHECK(failureOf([&] { client.synthesize(kAsk); }) == refused);
  CHECK(failureOf([&] {
          client.synthesizeStream(kAsk, [](const std::vector<float>&) {});
        }) == refused);

  // The same client answers as remote once the knobs point somewhere.
  pointAt("127.0.0.1:7029", 30000);
  CHECK(client.remote());
  forgetEndpoint();
}

TEST_CASE("the wire vocabulary is frozen")
{
  // Everything a caller who names nothing gets.
  const TtsRequest defaults;
  CHECK(defaults.lang == TtsLang::EN);
  CHECK(defaults.voiceId == "M3");
  CHECK(defaults.speed == 1.05F);
  CHECK(defaults.quality == TtsQuality::Auto);

  CHECK(std::string(langCode(TtsLang::EN)) == "en");
  CHECK(std::string(langCode(TtsLang::ES)) == "es");
  CHECK(std::string(langCode(TtsLang::KO)) == "ko");
  CHECK(std::string(langCode(TtsLang::NA)) == "na");

  // One code per enumerator, each a two-letter spelling: the count the
  // header asserts is only honest if the switch covers all of them.
  std::set<std::string> codes;
  for (int value = 0; value < kTtsLangCount; ++value)
    codes.insert(langCode(static_cast<TtsLang>(value)));
  CHECK(codes.size() == 32);
  for (const auto& code : codes)
    CHECK(code.size() == 2);
}
