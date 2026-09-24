#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <app/rpc/stt-rpc-server.hxx>
#include <config/config-service.hxx>
#include <errors/response-exception.hxx>
#include <feature/stt/services/stt-service.hxx>
#include <grpc/grpc-client-base.hxx>
#include <response.pb.h>
#include <response/response-rpc.hxx>
#include <stt.grpc.pb.h>
#include <stt/stt-client.hxx>
#include <stt/stt-errors.hxx>
#include <stt/stt-remote.hxx>
#include "wav-fixture.hxx"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <latch>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
#ifndef ARGUS_TEST_STT_MODELS_DIR
#define ARGUS_TEST_STT_MODELS_DIR "models/stt"
#endif

namespace wire = argus::stt::v1;

using argus::stt::Capabilities;
using argus::stt::Client;
using argus::stt::ClientConfig;
using argus::stt::TranscribeInput;

constexpr const char* kSecret = "rpc-secret";

struct Refusal
{
  int status{0};
  std::string code;
  std::string message;
};

template <typename Call>
Refusal refusalOf(Call&& call)
{
  try {
    call();
    return {};
  }
  catch (const ResponseException& error) {
    return {.status = error.statusCode(),
            .code = error.errorCode(),
            .message = error.what()};
  }
}

std::vector<float> speech()
{
  return std::vector<float>(kWireSampleRate, 0.25F);
}

std::string heardAt(int sampleRate, const std::string& language)
{
  return "heard " + std::to_string(kWireSampleRate) + " at " +
         std::to_string(sampleRate) + " " + language;
}

Capabilities capabilities()
{
  return {.sampleRate = kWireSampleRate,
          .loaded = true,
          .language = "es",
          .defaultLanguage = "es",
          .languages = {"es", "en", "auto"}};
}

bool speaksLanguage(const std::string& language)
{
  return language == "es" || language == "en";
}

SttRpcInput serverInput()
{
  return {.address = "127.0.0.1:0",
          .credentials = {{"voice", kSecret}},
          .capabilities = capabilities,
          .acceptsLanguage = speaksLanguage,
          .transcribe =
              [](const TranscribeRequest& request) {
                return heardAt(static_cast<int>(request.sampleRate),
                               request.lang) +
                       " " + std::to_string(request.samples.size());
              },
          .slots = 1};
}

struct HeldEngine
{
  std::latch& entered;
  std::latch& release;
  std::atomic<bool>& finished;
};

SttRpcInput heldInput(const HeldEngine& engine)
{
  auto input = serverInput();
  input.transcribe = [engine](const TranscribeRequest&) {
    engine.entered.count_down();
    engine.release.wait();
    engine.finished = true;
    return std::string("late");
  };
  return input;
}

ClientConfig clientConfig(int port)
{
  return {.target = "127.0.0.1:" + std::to_string(port),
          .credential = kSecret,
          .timeout = std::chrono::seconds(5)};
}

wire::TranscribeRequest wireRequest(const TranscribeInput& input)
{
  wire::TranscribeRequest request;
  request.mutable_samples()->Add(input.samples.begin(), input.samples.end());
  request.set_sample_rate(static_cast<std::uint32_t>(input.sampleRate));
  request.set_language(input.language);
  return request;
}

struct RawCall
{
  TranscribeInput input;
  std::string credential{kSecret};
  std::optional<std::chrono::seconds> deadline{std::chrono::seconds(5)};
};

std::unique_ptr<wire::Transcription::Stub> rawStub(int port)
{
  return wire::Transcription::NewStub(
      argus::client::makeChannel("127.0.0.1:" + std::to_string(port)));
}

std::string rawTranscribe(wire::Transcription::Stub& stub, const RawCall& call)
{
  grpc::ClientContext context;
  if (call.deadline)
    context.set_deadline(std::chrono::system_clock::now() + *call.deadline);
  if (!call.credential.empty())
    argus::client::addCallerCredential(context, call.credential);
  const wire::TranscribeRequest request = wireRequest(call.input);
  wire::TranscribeResponse response;
  const auto status = stub.Transcribe(&context, request, &response);
  if (!status.ok())
    throw argus::response::fromRpcStatus(status);
  return response.text();
}
}

TEST_CASE("legacy production client delegates to gRPC")
{
  SttRpcServer server(serverInput());
  ConfigService::setRuntimeString("stt.grpc_target",
                                  "127.0.0.1:" + std::to_string(server.port()));
  ConfigService::setRuntimeString("stt.grpc_credential", kSecret);
  SttClient client;
  CHECK(client.remote());
  CHECK(client.transcribe(speech(), "es") ==
        heardAt(kWireSampleRate, "es") + " 16000");
  ConfigService::setRuntimeString("stt.grpc_target", "");
  ConfigService::setRuntimeString("stt.grpc_credential", "");
  CHECK_FALSE(client.remote());
}

TEST_CASE("capabilities reports engine metadata")
{
  SttRpcServer server(serverInput());
  Client client(clientConfig(server.port()));
  const auto reported = client.capabilities();
  CHECK(reported.sampleRate == kWireSampleRate);
  CHECK(reported.loaded);
  CHECK(reported.language == "es");
  CHECK(reported.defaultLanguage == "es");
  REQUIRE(reported.languages.size() == 3);
  CHECK(reported.languages[0] == "es");
  CHECK(reported.languages[2] == "auto");
}

TEST_CASE("a capabilities reply outside the wire contract is refused")
{
  auto rate = serverInput();
  rate.capabilities = [] {
    auto reported = capabilities();
    reported.sampleRate = 0;
    return reported;
  };
  SttRpcServer rateServer(std::move(rate));
  Client rateClient(clientConfig(rateServer.port()));
  const auto unratable =
      refusalOf([&rateClient] { return rateClient.capabilities(); });
  CHECK(unratable.status == 502);
  CHECK(unratable.code == "BAD_GATEWAY");

  auto languages = serverInput();
  languages.capabilities = [] {
    auto reported = capabilities();
    reported.languages.clear();
    return reported;
  };
  SttRpcServer languageServer(std::move(languages));
  Client languageClient(clientConfig(languageServer.port()));
  const auto wordless =
      refusalOf([&languageClient] { return languageClient.capabilities(); });
  CHECK(wordless.status == 502);
  CHECK(wordless.code == "BAD_GATEWAY");
}

TEST_CASE("client rejects wrong credential")
{
  SttRpcServer server(serverInput());
  ClientConfig config = clientConfig(server.port());
  config.credential = "wrong";
  Client client(config);
  const auto untrusted =
      refusalOf([&client] { return client.capabilities(); });
  CHECK(untrusted.status == 401);
  CHECK(untrusted.code == "UNAUTHORIZED");
  const auto transcribed = refusalOf([&client] {
    return client.transcribe({.samples = speech(),
                              .sampleRate = kWireSampleRate,
                              .language = "es",
                              .cancellation = {}});
  });
  CHECK(transcribed.status == 401);
  CHECK(transcribed.code == "UNAUTHORIZED");
}

TEST_CASE("server errors roundtrip typed response details")
{
  auto unloaded = serverInput();
  unloaded.capabilities = [] {
    auto reported = capabilities();
    reported.loaded = false;
    return reported;
  };
  unloaded.transcribe = [](const TranscribeRequest&) -> std::string {
    throw ResponseException(503, SttErrors::SpeechEngineNotLoaded);
  };
  SttRpcServer server(std::move(unloaded));
  Client client(clientConfig(server.port()));
  CHECK_FALSE(client.capabilities().loaded);
  try {
    const std::string text = client.transcribe({.samples = speech(),
                                                .sampleRate = kWireSampleRate,
                                                .language = "es",
                                                .cancellation = {}});
    CHECK(text.empty());
  }
  catch (const ResponseException& error) {
    CHECK(error.statusCode() == 503);
    CHECK(error.errorCode() == "STT_NOT_LOADED");
    CHECK(std::string(error.what()) == "Speech-to-text engine is not loaded");
  }

  auto invalid = serverInput();
  invalid.transcribe = [](const TranscribeRequest&) -> std::string {
    throw ResponseException(422, std::vector<ResponseError>{
                                     {.code = "PCM_TOO_SHORT",
                                      .message = "PCM holds no speech"},
                                     {.code = "LANG_UNKNOWN",
                                      .message = "Language is not available"}});
  };
  SttRpcServer listServer(std::move(invalid));
  Client listClient(clientConfig(listServer.port()));
  try {
    const std::string text = listClient.transcribe({.samples = speech(),
                                                    .sampleRate = kWireSampleRate,
                                                    .language = "es",
                                                    .cancellation = {}});
    CHECK(text.empty());
  }
  catch (const ResponseException& error) {
    const auto* errors =
        std::get_if<std::vector<ResponseError>>(&error.errors());
    CHECK(error.statusCode() == 422);
    CHECK(error.errorCode() == "PCM_TOO_SHORT");
    REQUIRE(errors != nullptr);
    REQUIRE(errors->size() == 2);
    CHECK((*errors)[1].message == "Language is not available");
  }
}

TEST_CASE("failures outside the response contract are sanitized")
{
  auto input = serverInput();
  input.transcribe = [](const TranscribeRequest&) -> std::string {
    throw std::runtime_error("engine stack secret");
  };
  SttRpcServer server(std::move(input));
  Client client(clientConfig(server.port()));
  const auto sanitized = refusalOf([&client] {
    return client.transcribe({.samples = speech(),
                              .sampleRate = kWireSampleRate,
                              .language = "es",
                              .cancellation = {}});
  });
  CHECK(sanitized.status == 500);
  CHECK(sanitized.code == "INTERNAL_ERROR");
  CHECK(sanitized.message.find("secret") == std::string::npos);
}

TEST_CASE("invalid requests are refused before the engine runs")
{
  auto input = serverInput();
  std::atomic<int> calls{0};
  input.transcribe = [&calls](const TranscribeRequest&) {
    ++calls;
    return std::string("unreachable");
  };
  SttRpcServer server(std::move(input));
  Client client(clientConfig(server.port()));
  const auto refused = [&client](const TranscribeInput& candidate) {
    return refusalOf(
        [&client, &candidate] { return client.transcribe(candidate); });
  };
  CHECK(refused({.samples = {},
                 .sampleRate = kWireSampleRate,
                 .language = "es",
                 .cancellation = {}})
            .status == 400);
  CHECK(refused({.samples = speech(),
                 .sampleRate = 0,
                 .language = "es",
                 .cancellation = {}})
            .status == 400);
  CHECK(refused({.samples = speech(),
                 .sampleRate = 7999,
                 .language = "es",
                 .cancellation = {}})
            .status == 400);
  CHECK(refused({.samples = speech(),
                 .sampleRate = 192001,
                 .language = "es",
                 .cancellation = {}})
            .status == 400);
  CHECK(refused({.samples = speech(),
                 .sampleRate = kWireSampleRate,
                 .language = "fr",
                 .cancellation = {}})
            .code == "BAD_REQUEST");
  CHECK(calls.load() == 0);
  CHECK(client.transcribe({.samples = speech(),
                           .sampleRate = kWireSampleRate,
                           .language = "",
                           .cancellation = {}}) == "unreachable");
  CHECK(calls.load() == 1);
}

TEST_CASE("the wire refuses what its own client would never send")
{
  SttRpcServer server(serverInput());
  const auto stub = rawStub(server.port());
  const auto refused = [&stub](const RawCall& call) {
    return refusalOf([&stub, &call] { return rawTranscribe(*stub, call); });
  };
  CHECK(refused({.input = {.samples = {},
                           .sampleRate = kWireSampleRate,
                           .language = "es",
                           .cancellation = {}}})
            .status == 400);
  CHECK(refused({.input = {.samples = speech(),
                           .sampleRate = 7999,
                           .language = "es",
                           .cancellation = {}}})
            .status == 400);
  CHECK(refused({.input = {.samples = speech(),
                           .sampleRate = 192001,
                           .language = "es",
                           .cancellation = {}}})
            .status == 400);
  CHECK(refused({.input = {.samples = speech(),
                           .sampleRate = kWireSampleRate,
                           .language = "fr",
                           .cancellation = {}}})
            .status == 400);
  CHECK(refused({.input = {.samples = speech(),
                           .sampleRate = kWireSampleRate,
                           .language = "es",
                           .cancellation = {}},
                 .deadline = std::chrono::seconds(300)})
            .status == 400);
  CHECK(refused({.input = {.samples = speech(),
                           .sampleRate = kWireSampleRate,
                           .language = "es",
                           .cancellation = {}},
                 .credential = ""})
            .status == 401);

  CHECK(rawTranscribe(*stub, {.input = {.samples = speech(),
                                        .sampleRate = 8000,
                                        .language = "es",
                                        .cancellation = {}}}) ==
        heardAt(8000, "es") + " 16000");
  CHECK(rawTranscribe(*stub, {.input = {.samples = speech(),
                                        .sampleRate = 192000,
                                        .language = "",
                                        .cancellation = {}}}) ==
        heardAt(192000, "") + " 16000");
  CHECK(rawTranscribe(*stub, {.input = {.samples = speech(),
                                        .sampleRate = kWireSampleRate,
                                        .language = "es",
                                        .cancellation = {}},
                              .deadline = std::nullopt}) ==
        heardAt(kWireSampleRate, "es") + " 16000");
}

TEST_CASE("the wire refuses a request that presents two credentials")
{
  SttRpcServer server(serverInput());
  const auto stub = rawStub(server.port());
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(5));
  argus::client::addCallerCredential(context, kSecret);
  argus::client::addCallerCredential(context, kSecret);
  const wire::TranscribeRequest request =
      wireRequest({.samples = speech(),
                   .sampleRate = kWireSampleRate,
                   .language = "es",
                   .cancellation = {}});
  wire::TranscribeResponse response;
  const auto status = stub->Transcribe(&context, request, &response);
  REQUIRE_FALSE(status.ok());
  CHECK(argus::response::fromRpcStatus(status).statusCode() == 401);
  CHECK(response.text().empty());
}

TEST_CASE("transport failures map to the response contract")
{
  using argus::response::fromRpcStatus;
  const auto cancelled =
      fromRpcStatus({grpc::StatusCode::CANCELLED, "stopped"});
  CHECK(cancelled.statusCode() == 499);
  CHECK(cancelled.errorCode() == "CANCELLED");
  const auto deadline =
      fromRpcStatus({grpc::StatusCode::DEADLINE_EXCEEDED, "late"});
  CHECK(deadline.statusCode() == 504);
  CHECK(deadline.errorCode() == "DEADLINE_EXCEEDED");
  const auto garbage =
      fromRpcStatus({grpc::StatusCode::UNKNOWN, "boom", "junk"});
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

TEST_CASE("deadline and external cancellation stop transcription")
{
  std::latch deadlineEntered(1);
  std::latch deadlineRelease(1);
  std::atomic<bool> deadlineFinished{false};
  const HeldEngine deadlineEngine{.entered = deadlineEntered,
                                  .release = deadlineRelease,
                                  .finished = deadlineFinished};
  SttRpcServer deadlineServer(heldInput(deadlineEngine));
  auto config = clientConfig(deadlineServer.port());
  config.timeout = std::chrono::milliseconds(80);
  Client deadlineClient(config);
  const auto expired = refusalOf([&deadlineClient] {
    return deadlineClient.transcribe({.samples = speech(),
                                      .sampleRate = kWireSampleRate,
                                      .language = "es",
                                      .cancellation = {}});
  });
  CHECK(expired.status == 504);
  CHECK(expired.code == "DEADLINE_EXCEEDED");
  deadlineRelease.count_down();
  deadlineServer.shutdown();
  CHECK(deadlineFinished.load());

  std::latch cancelEntered(1);
  std::latch cancelRelease(1);
  std::atomic<bool> cancelFinished{false};
  const HeldEngine cancelEngine{.entered = cancelEntered,
                                .release = cancelRelease,
                                .finished = cancelFinished};
  SttRpcServer cancelServer(heldInput(cancelEngine));
  Client cancelClient(clientConfig(cancelServer.port()));
  std::stop_source stop;
  std::jthread canceller([&stop, &cancelEntered] {
    cancelEntered.wait();
    stop.request_stop();
  });
  const auto cancelled = refusalOf([&cancelClient, &stop] {
    return cancelClient.transcribe({.samples = speech(),
                                    .sampleRate = kWireSampleRate,
                                    .language = "es",
                                    .cancellation = stop.get_token()});
  });
  canceller.join();
  CHECK(cancelled.status == 499);
  CHECK(cancelled.code == "CANCELLED");
  cancelRelease.count_down();
  cancelServer.shutdown();
  CHECK(cancelFinished.load());
}

TEST_CASE("a second call is refused while the only slot is held")
{
  std::latch entered(1);
  std::latch release(1);
  std::atomic<bool> finished{false};
  const HeldEngine engine{.entered = entered,
                          .release = release,
                          .finished = finished};
  SttRpcServer server(heldInput(engine));
  Client client(clientConfig(server.port()));
  std::string held;
  std::jthread holder([&client, &held] {
    held = client.transcribe({.samples = speech(),
                              .sampleRate = kWireSampleRate,
                              .language = "es",
                              .cancellation = {}});
  });
  entered.wait();
  const auto busy = refusalOf([&client] {
    return client.transcribe({.samples = speech(),
                              .sampleRate = kWireSampleRate,
                              .language = "es",
                              .cancellation = {}});
  });
  CHECK(busy.status == 429);
  CHECK(busy.code == "TOO_MANY_REQUESTS");
  release.count_down();
  holder.join();
  CHECK(held == "late");
  server.shutdown();
  CHECK(finished.load());
}

TEST_CASE("real engine transcribes through the legacy client")
{
  ConfigService::setRuntimeString("stt.models_dir", ARGUS_TEST_STT_MODELS_DIR);
  auto& engine = SttService::instance();
  engine.init();
  REQUIRE_MESSAGE(engine.isLoaded(),
                  "STT engine failed to load from " ARGUS_TEST_STT_MODELS_DIR
                  " — this case reads models/stt/zipformer-en/test_wavs/0.wav, "
                  "which no provisioning script fetches");
  auto input = serverInput();
  input.capabilities = [] {
    auto& service = SttService::instance();
    return Capabilities{.sampleRate = kWireSampleRate,
                        .loaded = service.isLoaded(),
                        .language = service.language(),
                        .defaultLanguage = SttService::configLanguage(),
                        .languages = SttService::supportedLanguages()};
  };
  input.acceptsLanguage = [](const std::string& language) {
    return SttService::instance().isSupportedLanguage(language);
  };
  input.transcribe = [&engine](const TranscribeRequest& request) {
    return engine.transcribe(request);
  };
  SttRpcServer server(std::move(input));
  ConfigService::setRuntimeString("stt.grpc_target",
                                  "127.0.0.1:" + std::to_string(server.port()));
  ConfigService::setRuntimeString("stt.grpc_credential", kSecret);
  const std::vector<float> samples =
      wavSamples(ARGUS_TEST_STT_MODELS_DIR "/zipformer-en/test_wavs/0.wav");
  REQUIRE(samples.size() > 16000);
  SttClient client;
  const std::string spoken = client.transcribe(samples, "en");
  const std::string inProcess = engine.transcribe(
      {.samples = samples, .sampleRate = kWireSampleRate, .lang = "en"});
  CHECK(spoken == inProcess);
  CHECK_FALSE(spoken.empty());
  server.shutdown();
  engine.shutdown();
  ConfigService::setRuntimeString("stt.grpc_target", "");
  ConfigService::setRuntimeString("stt.grpc_credential", "");
}
