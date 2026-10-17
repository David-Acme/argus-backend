#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <app/rpc/tts-rpc-server.hxx>
#include <tts/tts-client.hxx>
#include <tts.grpc.pb.h>
#include <grpc/grpc-client-base.hxx>
#include <errors/response-exception.hxx>
#include <tts/tts-errors.hxx>
#include <response/response-rpc.hxx>
#include <response.pb.h>
#include <tts/tts-remote.hxx>
#include <config/config-service.hxx>
#include <feature/synthesis/services/tts-service.hxx>
#include <grpc-test-support/internal-tls-probe.hxx>
#include <grpc/internal-tls.hxx>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using argus::tts::AudioChunk;
using argus::tts::Client;
using argus::tts::ClientConfig;
using argus::tts::Quality;
using argus::tts::SynthesisInput;

constexpr const char* kSecret = "rpc-secret";

TtsRpcInput serverInput()
{
  return {.address = "127.0.0.1:0",
          .credentials = {{"gateway", kSecret}},
          .capabilities = {.sampleRate = 24000,
                           .channels = 1,
                           .defaultSpeed = 1.25F,
                           .voices = {"M3"},
                           .languages = {"en"}},
          .synthesize = [](TtsStreamInput input) {
            std::vector<float> samples(4097, 0.5F);
            samples[0] = static_cast<float>(input.request.text.size());
            input.onChunk(samples);
          },
          .slots = 1,
          .defaultSpeed = {},
          .services = {},
          .tlsAddress = {},
          .tls = std::nullopt};
}

ClientConfig clientConfig(int port)
{
  return {.target = "127.0.0.1:" + std::to_string(port),
          .credential = kSecret,
          .timeout = std::chrono::seconds(5)};
}

ClientConfig mutualTlsConfig(int port)
{
  return {.target = "tls:127.0.0.1:" + std::to_string(port),
          .credential = kSecret,
          .timeout = std::chrono::seconds(5)};
}

grpc::StatusCode synthesisCarrying(const std::string& target, std::size_t bytes)
{
  const auto stub = argus::tts::v1::Synthesis::NewStub(argus::client::makeChannel(target));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
  argus::client::addCallerCredential(context, kSecret);
  argus::tts::v1::SynthesisRequest request;
  request.set_text(std::string(bytes, 'a'));
  request.set_voice("M3");
  request.set_language("en");
  auto reader = stub->Synthesize(&context, request);
  argus::tts::v1::AudioChunk chunk;
  while (reader->Read(&chunk)) {
  }
  return reader->Finish().error_code();
}

SynthesisInput synthesisInput(std::string text)
{
  SynthesisInput input;
  input.text = std::move(text);
  input.voice = "M3";
  input.language = "en";
  input.speed = 1.0F;
  input.quality = Quality::Auto;
  return input;
}
}

TEST_CASE("legacy production client delegates to gRPC")
{
  auto input = serverInput();
  input.capabilities.sampleRate = 44100;
  TtsRpcServer server(std::move(input));
  ConfigService::setRuntimeString("tts.grpc_target", "127.0.0.1:" + std::to_string(server.port()));
  ConfigService::setRuntimeString("tts.grpc_credential", kSecret);
  TtsClient client;
  CHECK(client.remote());
  CHECK(client.sampleRate() == 44100);
  CHECK(client.defaultSpeed() == doctest::Approx(1.25F));
  const auto samples = client.synthesize({.text = "hello", .lang = TtsLang::EN,
      .voiceId = "M3", .quality = TtsQuality::Auto, .speed = 1.0F});
  REQUIRE(samples.size() == 4097);
  CHECK(samples.front() == 5.0F);
  CHECK(samples.back() == 0.5F);
  ConfigService::setRuntimeString("tts.grpc_target", "");
  ConfigService::setRuntimeString("tts.grpc_credential", "");
}

TEST_CASE("legacy production client propagates cancellation to gRPC")
{
  auto input = serverInput();
  std::atomic<bool> entered{false};
  std::atomic<bool> cancelled{false};
  input.synthesize = [&](TtsStreamInput stream) {
    entered.store(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!stream.stopRequested() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    cancelled.store(stream.stopRequested());
  };
  TtsRpcServer server(std::move(input));
  ConfigService::setRuntimeString("tts.grpc_target", "127.0.0.1:" + std::to_string(server.port()));
  ConfigService::setRuntimeString("tts.grpc_credential", kSecret);
  TtsClient client;
  std::stop_source stop;
  std::jthread canceller([&] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!entered.load() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    stop.request_stop();
  });
  bool stopped = false;
  try {
    client.synthesizeStream({
        .request = {.text = "hello", .lang = TtsLang::EN, .voiceId = "M3",
                    .quality = TtsQuality::Auto, .speed = 1.0F},
        .onChunk = [](const std::vector<float>&) {},
        .cancellation = stop.get_token()});
  }
  catch (const ResponseException& error) {
    stopped = error.statusCode() == 499;
  }
  canceller.join();
  server.shutdown();
  CHECK(entered.load());
  CHECK(cancelled.load());
  CHECK(stopped);
  ConfigService::setRuntimeString("tts.grpc_target", "");
  ConfigService::setRuntimeString("tts.grpc_credential", "");
}

TEST_CASE("capabilities reports engine metadata")
{
  TtsRpcServer server(serverInput());
  Client client(clientConfig(server.port()));
  const auto capabilities = client.capabilities();
  CHECK(capabilities.sampleRate == 24000);
  CHECK(capabilities.channels == 1);
  CHECK(capabilities.defaultSpeed == doctest::Approx(1.25F));
  REQUIRE(capabilities.voices.size() == 1);
  CHECK(capabilities.voices[0] == "M3");
  REQUIRE(capabilities.languages.size() == 1);
  CHECK(capabilities.languages[0] == "en");
}

TEST_CASE("capabilities reports the live default speed, not the boot snapshot")
{
  float speed = 1.25F;
  auto input = serverInput();
  input.defaultSpeed = [&speed] { return speed; };
  TtsRpcServer server(std::move(input));
  Client client(clientConfig(server.port()));
  CHECK(client.capabilities().defaultSpeed == doctest::Approx(1.25F));
  speed = 1.6F;
  CHECK(client.capabilities().defaultSpeed == doctest::Approx(1.6F));
}

TEST_CASE("synthesize streams owned chunks in sequence")
{
  TtsRpcServer server(serverInput());
  Client client(clientConfig(server.port()));
  std::vector<AudioChunk> chunks;
  SynthesisInput input = synthesisInput("hello");
  input.onChunk = [&](AudioChunk chunk) {
    CHECK(chunk.sampleRate == 24000);
    CHECK(chunk.sequence == chunks.size());
    chunks.push_back(std::move(chunk));
    return true;
  };
  client.synthesize(input);
  REQUIRE(chunks.size() == 2);
  CHECK(chunks[0].samples.size() == 4096);
  CHECK(chunks[1].samples.size() == 1);
  CHECK(chunks[0].samples[0] == doctest::Approx(5.0F));
  CHECK(chunks[1].samples[0] == doctest::Approx(0.5F));
}

TEST_CASE("client rejects wrong credential")
{
  TtsRpcServer server(serverInput());
  ClientConfig config = clientConfig(server.port());
  config.credential = "wrong";
  Client client(config);
  bool unauthorized = false;
  try { client.capabilities(); }
  catch (const ResponseException& error) {
    unauthorized = error.statusCode() == 401 && error.errorCode() == "UNAUTHORIZED";
  }
  CHECK(unauthorized);
  bool consumed = false;
  SynthesisInput input = synthesisInput("hello");
  input.onChunk = [&](AudioChunk) {
    consumed = true;
    return true;
  };
  CHECK_THROWS_AS(client.synthesize(input), ResponseException);
  CHECK_FALSE(consumed);
}

TEST_CASE("server errors roundtrip typed response details")
{
  auto single = serverInput();
  single.synthesize = [](TtsStreamInput) {
    throw ResponseException(503, TtsErrors::TtsNotLoaded);
  };
  TtsRpcServer server(std::move(single));
  Client client(clientConfig(server.port()));
  auto synthesis = synthesisInput("hello");
  synthesis.onChunk = [](AudioChunk) { return true; };
  bool notLoaded = false;
  try { client.synthesize(synthesis); }
  catch (const ResponseException& error) {
    notLoaded = error.statusCode() == 503 && error.errorCode() == "TTS_NOT_LOADED" &&
                std::string(error.what()) == "Text-to-speech engine is not loaded";
  }
  CHECK(notLoaded);

  auto multiple = serverInput();
  multiple.synthesize = [](TtsStreamInput) {
    throw ResponseException(422, std::vector<ResponseError>{
        {.code = "INVALID_TEXT", .message = "Text must not be empty"},
        {.code = "VOICE_UNKNOWN", .message = "Voice is not available"}});
  };
  TtsRpcServer listServer(std::move(multiple));
  Client listClient(clientConfig(listServer.port()));
  bool list = false;
  try { listClient.synthesize(synthesis); }
  catch (const ResponseException& error) {
    const auto* errors = std::get_if<std::vector<ResponseError>>(&error.errors());
    list = error.statusCode() == 422 && error.errorCode() == "INVALID_TEXT" &&
           errors && errors->size() == 2 &&
           (*errors)[1].message == "Voice is not available";
  }
  CHECK(list);
}

TEST_CASE("failures outside the response contract are sanitized")
{
  auto input = serverInput();
  input.synthesize = [](TtsStreamInput) {
    throw std::runtime_error("engine stack secret");
  };
  TtsRpcServer server(std::move(input));
  Client client(clientConfig(server.port()));
  auto synthesis = synthesisInput("hello");
  synthesis.onChunk = [](AudioChunk) { return true; };
  bool sanitized = false;
  try { client.synthesize(synthesis); }
  catch (const ResponseException& error) {
    sanitized = error.statusCode() == 500 && error.errorCode() == "INTERNAL_ERROR" &&
                std::string(error.what()).find("secret") == std::string::npos;
  }
  CHECK(sanitized);
}

TEST_CASE("invalid requests and malformed details keep http codes bounded")
{
  TtsRpcServer server(serverInput());
  Client client(clientConfig(server.port()));
  auto synthesis = synthesisInput("hello");
  synthesis.voice = "missing";
  synthesis.onChunk = [](AudioChunk) { return true; };
  bool invalid = false;
  try { client.synthesize(synthesis); }
  catch (const ResponseException& error) {
    invalid = error.statusCode() == 400 && error.errorCode() == "BAD_REQUEST";
  }
  CHECK(invalid);

  using argus::response::fromRpcStatus;
  const auto cancelled = fromRpcStatus({grpc::StatusCode::CANCELLED, "stopped"});
  CHECK(cancelled.statusCode() == 499);
  CHECK(cancelled.errorCode() == "CANCELLED");
  const auto deadline = fromRpcStatus({grpc::StatusCode::DEADLINE_EXCEEDED, "late"});
  CHECK(deadline.statusCode() == 504);
  CHECK(deadline.errorCode() == "DEADLINE_EXCEEDED");
  const auto garbage = fromRpcStatus({grpc::StatusCode::UNKNOWN, "boom", "junk"});
  CHECK(garbage.statusCode() == 502);
  CHECK(garbage.errorCode() == "BAD_GATEWAY");
  argus::response::v1::ErrorResponse unbounded;
  unbounded.set_status(503);
  unbounded.mutable_single()->set_message("no code");
  const auto malformed = fromRpcStatus(
      {grpc::StatusCode::UNAVAILABLE, "boom", unbounded.SerializeAsString()});
  CHECK(malformed.statusCode() == 502);
  CHECK(malformed.errorCode() == "BAD_GATEWAY");
}

TEST_CASE("real engine streams its native rate through the legacy client")
{
  ConfigService::setRuntimeString("tts.models_dir", ARGUS_TEST_TTS_MODELS_DIR);
  ConfigService::setRuntimeString("tts.speed", "1.0");
  ConfigService::setRuntimeString("tts.max_chunk_len", "350");
  ConfigService::setRuntimeString("tts.quality", "low");
  auto& engine = TtsService::instance();
  engine.init();
  if (!engine.isLoaded()) {
    const std::filesystem::path manifest =
        std::filesystem::path(ARGUS_TEST_TTS_MODELS_DIR) / "onnx" / "tts.json";
    if (std::filesystem::is_regular_file(manifest))
      FAIL("the TTS engine did not load from the model installed under "
           ARGUS_TEST_TTS_MODELS_DIR);
    MESSAGE("skipping the real-engine case: the TTS model is not installed "
            "under " ARGUS_TEST_TTS_MODELS_DIR
            " — run services/tts/scripts/provision.sh to install it");
    return;
  }
  auto input = serverInput();
  input.capabilities = {.sampleRate = engine.sampleRate(), .channels = 1,
      .defaultSpeed = engine.defaultSpeed(), .voices = engine.availableVoices(),
      .languages = TtsService::supportedLangs()};
  input.synthesize = [&engine](TtsStreamInput input) {
    engine.synthesizeStream(std::move(input));
  };
  TtsRpcServer server(std::move(input));
  ConfigService::setRuntimeString("tts.grpc_target", "127.0.0.1:" + std::to_string(server.port()));
  ConfigService::setRuntimeString("tts.grpc_credential", kSecret);
  TtsClient client;
  CHECK(client.sampleRate() == engine.sampleRate());
  const auto samples = client.synthesize({.text = "Hello Argus.", .lang = TtsLang::EN,
      .voiceId = "M3", .quality = TtsQuality::Low, .speed = 1.0F});
  CHECK_FALSE(samples.empty());
  server.shutdown();
  engine.shutdown();
  ConfigService::setRuntimeString("tts.grpc_target", "");
  ConfigService::setRuntimeString("tts.grpc_credential", "");
}

TEST_CASE("deadline and external cancellation stop synthesis")
{
  auto input = serverInput();
  input.synthesize = [](TtsStreamInput input) {
    for (int index = 0; index < 100; ++index) {
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      input.onChunk(std::vector<float>(100, 0.5F));
    }
  };
  TtsRpcServer server(std::move(input));
  auto config = clientConfig(server.port());
  config.timeout = std::chrono::milliseconds(80);
  Client client(config);
  auto synthesis = synthesisInput("hello");
  synthesis.onChunk = [](AudioChunk) { return true; };
  bool deadline = false;
  try { client.synthesize(synthesis); }
  catch (const ResponseException& error) { deadline = error.statusCode() == 504 && error.errorCode() == "DEADLINE_EXCEEDED"; }
  CHECK(deadline);
  server.shutdown();

  TtsRpcServer cancelServer(serverInput());
  Client cancelClient(clientConfig(cancelServer.port()));
  std::stop_source stop;
  synthesis.cancellation = stop.get_token();
  synthesis.onChunk = [&stop](AudioChunk) { stop.request_stop(); return true; };
  bool cancelled = false;
  try { cancelClient.synthesize(synthesis); }
  catch (const ResponseException& error) { cancelled = error.statusCode() == 499 && error.errorCode() == "CANCELLED"; }
  CHECK(cancelled);
}

TEST_CASE("client early-stop cancels the stream")
{
  TtsRpcInput input = serverInput();
  input.synthesize = [](TtsStreamInput input) {
    for (int index = 1; index <= 3; ++index) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      input.onChunk(std::vector<float>(100, static_cast<float>(index)));
    }
  };
  TtsRpcServer server(input);
  Client client(clientConfig(server.port()));
  std::atomic<int> calls{0};
  bool stopped = false;
  SynthesisInput synthesis = synthesisInput("hello");
  synthesis.onChunk = [&calls](AudioChunk) -> bool {
    return calls.fetch_add(1) + 1 < 2;
  };
  try {
    client.synthesize(synthesis);
  }
  catch (const ResponseException& error) {
    stopped = error.statusCode() == 499 && error.errorCode() == "CANCELLED";
  }
  CHECK(stopped);
  CHECK(calls.load() == 2);
}

TEST_CASE("a listener whose callers are all placeholders refuses to start")
{
  TtsRpcInput input = serverInput();
  input.credentials = {{"voice", "CHANGE_ME_VOICE_TTS"}, {"settings", "CHANGE_ME_SETTINGS_TTS"}};
  CHECK_THROWS_AS(TtsRpcServer{std::move(input)}, std::invalid_argument);
}

TEST_CASE("the wire refuses a request that presents two credentials")
{
  TtsRpcServer server(serverInput());
  const auto stub = argus::tts::v1::Synthesis::NewStub(
      argus::client::makeChannel("127.0.0.1:" + std::to_string(server.port())));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
  argus::client::addCallerCredential(context, kSecret);
  argus::client::addCallerCredential(context, kSecret);
  argus::tts::v1::CapabilitiesRequest request;
  argus::tts::v1::CapabilitiesResponse response;
  const auto status = stub->Capabilities(&context, request, &response);
  REQUIRE_FALSE(status.ok());
  CHECK(argus::response::fromRpcStatus(status).statusCode() == 401);
  CHECK(response.sample_rate() == 0);
}

TEST_CASE("the mutual TLS address opens a second listener for the same calls")
{
  TtsRpcInput input = serverInput();
  input.tlsAddress = "127.0.0.1:0";
  input.tls = argus::client::test_support::pkiMaterial("tts");
  TtsRpcServer server(std::move(input));
  REQUIRE(server.tlsPort() > 0);
  CHECK(server.tlsPort() != server.port());
  argus::client::installInternalTls(argus::client::test_support::pkiMaterial("voice"));
  CHECK(Client(mutualTlsConfig(server.tlsPort())).capabilities().sampleRate == 24000);
  CHECK(Client(clientConfig(server.port())).capabilities().sampleRate == 24000);
}

TEST_CASE("no second listener without both a TLS address and material")
{
  TtsRpcServer plain(serverInput());
  CHECK(plain.tlsPort() == 0);

  TtsRpcInput addressOnly = serverInput();
  addressOnly.tlsAddress = "127.0.0.1:0";
  TtsRpcServer withoutMaterial(std::move(addressOnly));
  CHECK(withoutMaterial.tlsPort() == 0);

  TtsRpcInput materialOnly = serverInput();
  materialOnly.tls = argus::client::test_support::pkiMaterial("tts");
  TtsRpcServer withoutAddress(std::move(materialOnly));
  CHECK(withoutAddress.tlsPort() == 0);
}

TEST_CASE("a mutual TLS address that cannot be bound stops the start")
{
  TtsRpcInput input = serverInput();
  input.tlsAddress = "192.0.2.1:0";
  input.tls = argus::client::test_support::pkiMaterial("tts");
  CHECK_THROWS_WITH_AS(TtsRpcServer{std::move(input)},
                       "TTS RPC failed to listen on 127.0.0.1:0 and 192.0.2.1:0",
                       std::runtime_error);
}

TEST_CASE("a request over the 32 KiB cap is refused on both ports")
{
  TtsRpcInput input = serverInput();
  input.tlsAddress = "127.0.0.1:0";
  input.tls = argus::client::test_support::pkiMaterial("tts");
  TtsRpcServer server(std::move(input));
  REQUIRE(server.tlsPort() > 0);
  argus::client::installInternalTls(argus::client::test_support::pkiMaterial("voice"));
  const std::string plain = "127.0.0.1:" + std::to_string(server.port());
  const std::string mutual = "tls:127.0.0.1:" + std::to_string(server.tlsPort());
  CHECK(synthesisCarrying(plain, 16000) == grpc::StatusCode::OK);
  CHECK(synthesisCarrying(mutual, 16000) == grpc::StatusCode::OK);
  CHECK(synthesisCarrying(plain, 40000) == grpc::StatusCode::RESOURCE_EXHAUSTED);
  CHECK(synthesisCarrying(mutual, 40000) == grpc::StatusCode::RESOURCE_EXHAUSTED);
}
