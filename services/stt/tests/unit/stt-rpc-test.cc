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
#include <algorithm>
#include <latch>
#include <memory>
#include <mutex>
#include <span>
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
          .slots = 1,
          .services = {}};
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

namespace
{
using argus::stt::StreamUpdate;

struct CountingEngine
{
  std::atomic<int> calls{0};
};

SttRpcInput countingInput(CountingEngine& engine)
{
  auto input = serverInput();
  input.transcribe = [&engine](const TranscribeRequest& request) {
    ++engine.calls;
    return "heard " + std::to_string(request.samples.size()) + " " + request.lang;
  };
  return input;
}

struct PartialLog
{
  std::mutex mutex;
  std::vector<StreamUpdate> seen;
};

argus::stt::StreamInput streamInput(PartialLog& log, std::stop_token cancellation = {})
{
  return {.sampleRate = kWireSampleRate,
          .language = "es",
          .onPartial =
              [&log](const StreamUpdate& update) {
                std::scoped_lock lock(log.mutex);
                log.seen.push_back(update);
              },
          .cancellation = std::move(cancellation)};
}

std::vector<float> halfSecond()
{
  return std::vector<float>(kWireSampleRate / 2, 0.25F);
}
}

TEST_CASE("a stream answers a flush with a partial and its end with the same decode")
{
  CountingEngine engine;
  SttRpcServer server(countingInput(engine));
  Client client(clientConfig(server.port()));
  PartialLog log;
  const auto stream = client.openStream(streamInput(log));
  const auto chunk = halfSecond();
  stream->push(chunk);
  stream->push(chunk);
  stream->flush();
  const StreamUpdate final = stream->finish();
  CHECK(final.final);
  CHECK(final.text == "heard 16000 es");
  CHECK(final.samples == 16000);
  CHECK(final.decodeMs == 0);
  CHECK(engine.calls.load() == 1);
  std::scoped_lock lock(log.mutex);
  REQUIRE(log.seen.size() == 1);
  CHECK_FALSE(log.seen.front().final);
  CHECK(log.seen.front().text == "heard 16000 es");
  CHECK(log.seen.front().samples == 16000);
}

TEST_CASE("audio that arrives after the last flush is decoded at the end")
{
  CountingEngine engine;
  SttRpcServer server(countingInput(engine));
  Client client(clientConfig(server.port()));
  PartialLog log;
  const auto stream = client.openStream(streamInput(log));
  const auto chunk = halfSecond();
  stream->push(chunk);
  stream->flush();
  stream->flush();
  stream->push(chunk);
  const StreamUpdate final = stream->finish();
  CHECK(final.text == "heard 16000 es");
  CHECK(final.samples == 16000);
  CHECK(engine.calls.load() == 2);
  std::scoped_lock lock(log.mutex);
  REQUIRE(log.seen.size() == 2);
  CHECK(log.seen[0].text == "heard 8000 es");
  CHECK(log.seen[1].text == "heard 8000 es");
  CHECK(log.seen[1].decodeMs == 0);
}

TEST_CASE("a stream without audio, with another language or another rate is refused")
{
  CountingEngine engine;
  SttRpcServer server(countingInput(engine));
  Client client(clientConfig(server.port()));
  PartialLog log;

  const auto silent = client.openStream(streamInput(log));
  CHECK(refusalOf([&silent] { return silent->finish(); }).status == 400);

  auto french = streamInput(log);
  french.language = "fr";
  const auto foreign = client.openStream(std::move(french));
  const auto refused = refusalOf([&foreign] {
    foreign->push(halfSecond());
    return foreign->finish();
  });
  CHECK(refused.status == 400);

  const auto stub = rawStub(server.port());
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
  argus::client::addCallerCredential(context, kSecret);
  const auto io = stub->TranscribeStream(&context);
  wire::TranscribeChunk first;
  first.set_sample_rate(kWireSampleRate);
  first.add_samples(0.25F);
  io->Write(first);
  wire::TranscribeChunk second;
  second.set_sample_rate(8000);
  second.add_samples(0.25F);
  io->Write(second);
  io->WritesDone();
  wire::TranscribeUpdate ignored;
  while (io->Read(&ignored)) {
  }
  const auto status = io->Finish();
  REQUIRE_FALSE(status.ok());
  CHECK(argus::response::fromRpcStatus(status).statusCode() == 400);
  CHECK(engine.calls.load() == 0);

  CHECK(refusalOf([&client, &log] {
          auto slow = streamInput(log);
          slow.sampleRate = 4000;
          return client.openStream(std::move(slow));
        }).status == 400);
}

TEST_CASE("a stream refuses an unlisted caller and stops on the caller's token")
{
  CountingEngine engine;
  SttRpcServer server(countingInput(engine));
  PartialLog log;

  ClientConfig wrong = clientConfig(server.port());
  wrong.credential = "wrong";
  Client untrusted(wrong);
  const auto rejected = untrusted.openStream(streamInput(log));
  const auto refusal = refusalOf([&rejected] {
    rejected->push(halfSecond());
    return rejected->finish();
  });
  CHECK(refusal.status == 401);

  Client client(clientConfig(server.port()));
  std::stop_source stop;
  const auto stream = client.openStream(streamInput(log, stop.get_token()));
  stream->push(halfSecond());
  stop.request_stop();
  CHECK(refusalOf([&stream] { return stream->finish(); }).status == 499);
  CHECK(engine.calls.load() == 0);
}

TEST_CASE("a language-agnostic engine changes language without reloading its model")
{
  CHECK(SttService::languageBound(SttEngine::Whisper));
  CHECK(SttService::languageBound(SttEngine::Canary));
  CHECK_FALSE(SttService::languageBound(SttEngine::NemoTransducer));
  CHECK_FALSE(SttService::languageBound(SttEngine::NemoCtc));
  CHECK_FALSE(SttService::languageBound(SttEngine::Omnilingual));

  ConfigService::setRuntimeString("stt.models_dir", ARGUS_TEST_STT_MODELS_DIR);
  ConfigService::setRuntimeString("stt.engine", "nemo_transducer");
  auto& engine = SttService::instance();
  engine.init();
  REQUIRE(engine.isLoaded());
  const auto started = std::chrono::steady_clock::now();
  CHECK(engine.setLanguage("en"));
  CHECK(engine.setLanguage("es"));
  CHECK(engine.setLanguage("en"));
  const auto switching = std::chrono::steady_clock::now() - started;
  CHECK(engine.language() == "en");
  CHECK(switching < std::chrono::milliseconds(50));
  CHECK_FALSE(engine.setLanguage("fr"));
  CHECK(engine.language() == "en");
  engine.shutdown();
  ConfigService::setRuntimeString("stt.engine", "");
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

  PartialLog log;
  const Client streaming(clientConfig(server.port()));
  const auto stream = streaming.openStream(
      {.sampleRate = kWireSampleRate,
       .language = "en",
       .onPartial =
           [&log](const StreamUpdate& update) {
             std::scoped_lock lock(log.mutex);
             log.seen.push_back(update);
           },
       .cancellation = {}});
  constexpr std::size_t kChunk = 1600;
  for (std::size_t at = 0; at < samples.size(); at += kChunk)
    stream->push(std::span<const float>(samples).subspan(
        at, std::min(kChunk, samples.size() - at)));
  stream->flush();
  const StreamUpdate streamed = stream->finish();
  CHECK(streamed.text == inProcess);
  CHECK(streamed.samples == samples.size());
  CHECK(streamed.decodeMs == 0);
  {
    std::scoped_lock lock(log.mutex);
    REQUIRE(log.seen.size() == 1);
    CHECK(log.seen.front().text == inProcess);
    CHECK(log.seen.front().decodeMs > 0);
  }
  server.shutdown();
  engine.shutdown();
  ConfigService::setRuntimeString("stt.grpc_target", "");
  ConfigService::setRuntimeString("stt.grpc_credential", "");
}
