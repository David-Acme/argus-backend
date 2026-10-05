#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "app-loop.hxx"
#include "image-fixture.hxx"

#include <app/rpc/vlm-rpc-server.hxx>
#include <config/config-service.hxx>
#include <errors/response-exception.hxx>
#include <feature/vlm/controllers/vlm-controller.hxx>
#include <grpc/grpc-client-base.hxx>
#include <llama.h>
#include <response.pb.h>
#include <response/response-rpc.hxx>
#include <vlm.grpc.pb.h>
#include <vlm/vlm-client.hxx>
#include <vlm/vlm-errors.hxx>
#include <vlm/vlm-remote.hxx>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <latch>
#include <memory>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
#ifndef ARGUS_TEST_VLM_MODELS_DIR
#define ARGUS_TEST_VLM_MODELS_DIR "models/vision/lfm2vl-25"
#endif

namespace wire = argus::vlm::v1;

using argus::vlm::Capabilities;
using argus::vlm::Client;
using argus::vlm::ClientConfig;
using argus::vlm::DescribeInput;

constexpr const char* kSecret = "rpc-secret";
constexpr const char* kCaption = "a person in a grey coat";
constexpr const char* kScratchConfig = "/tmp/argus-vlm-rpc-test.toml";

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

std::string described(const VisionDescribeMatInput& input)
{
  return std::string(kCaption) + " " + input.prompt + " " +
         std::to_string(input.bgr.rows) + "x" +
         std::to_string(input.bgr.cols);
}

std::string expected(const DescribeInput& input)
{
  return std::string(kCaption) + " " + input.prompt + " 64x64";
}

Capabilities capabilities()
{
  return {.loaded = true, .maxInputPx = 384, .defaultMaxTokens = 64};
}

VlmRpcInput serverInput()
{
  return {.address = "127.0.0.1:0",
          .credentials = {{"guard", kSecret}},
          .capabilities = capabilities,
          .describe = described,
          .slots = 1,
          .services = {}};
}

constexpr auto kEntryWait = std::chrono::seconds(10);

struct HeldEngine
{
  std::counting_semaphore<1>& entered;
  std::latch& release;
  std::atomic<bool>& finished;
};

VlmRpcInput heldInput(const HeldEngine& engine)
{
  auto input = serverInput();
  input.describe = [engine](const VisionDescribeMatInput&) {
    engine.entered.release();
    engine.release.wait();
    engine.finished = true;
    return std::string("late");
  };
  return input;
}

ClientConfig clientConfig(int port,
                          std::chrono::milliseconds timeout =
                              std::chrono::seconds(5))
{
  return {.target = "127.0.0.1:" + std::to_string(port),
          .credential = kSecret,
          .timeout = timeout};
}

wire::DescribeRequest wireRequest(const DescribeInput& input)
{
  wire::DescribeRequest request;
  request.set_image_jpeg(input.jpeg);
  request.set_prompt(input.prompt);
  request.set_camera_id(input.cameraId);
  request.set_max_tokens(input.maxTokens);
  return request;
}

struct RawCall
{
  DescribeInput input;
  std::string credential{kSecret};
  std::optional<std::chrono::seconds> deadline{std::chrono::seconds(5)};
};

std::unique_ptr<wire::Vision::Stub> rawStub(int port)
{
  return wire::Vision::NewStub(
      argus::client::makeChannel("127.0.0.1:" + std::to_string(port)));
}

std::string rawDescribe(wire::Vision::Stub& stub, const RawCall& call)
{
  grpc::ClientContext context;
  if (call.deadline)
    context.set_deadline(std::chrono::system_clock::now() + *call.deadline);
  if (!call.credential.empty())
    argus::client::addCallerCredential(context, call.credential);
  const wire::DescribeRequest request = wireRequest(call.input);
  wire::DescribeResponse response;
  const auto status = stub.Describe(&context, request, &response);
  if (!status.ok())
    throw argus::response::fromRpcStatus(status);
  return response.caption();
}

DescribeInput ask()
{
  return {.jpeg = jpegFixture(),
          .prompt = "who is at the door?",
          .cameraId = ""};
}
}

TEST_CASE("capabilities reports engine metadata")
{
  VlmRpcServer server(serverInput());
  Client client(clientConfig(server.port()));
  const auto reported = client.capabilities();
  CHECK(reported.loaded);
  CHECK(reported.maxInputPx == 384);
  CHECK(reported.defaultMaxTokens == 64);
}

TEST_CASE("a capabilities reply outside the wire contract is refused")
{
  auto empty = serverInput();
  empty.capabilities = [] {
    auto reported = capabilities();
    reported.maxInputPx = 0;
    return reported;
  };
  VlmRpcServer emptyServer(std::move(empty));
  Client emptyClient(clientConfig(emptyServer.port()));
  const auto unreadable = refusalOf([&emptyClient] {
    return emptyClient.capabilities();
  });
  CHECK(unreadable.status == 502);
  CHECK(unreadable.code == "BAD_GATEWAY");

  auto unbounded = serverInput();
  unbounded.capabilities = [] {
    auto reported = capabilities();
    reported.defaultMaxTokens = 99999;
    return reported;
  };
  VlmRpcServer unboundedServer(std::move(unbounded));
  Client unboundedClient(clientConfig(unboundedServer.port()));
  const auto tokens =
      refusalOf([&unboundedClient] { return unboundedClient.capabilities(); });
  CHECK(tokens.status == 502);
  CHECK(tokens.code == "BAD_GATEWAY");
}

TEST_CASE("describe decodes the wire image and answers with the caption")
{
  VlmRpcServer server(serverInput());
  Client client(clientConfig(server.port()));
  const DescribeInput input = ask();
  CHECK(client.describe(input) == expected(input));

  DescribeInput promptless = input;
  promptless.prompt.clear();
  promptless.cameraId = "cam-1";
  CHECK(client.describe(promptless) == expected(promptless));
}

TEST_CASE("an image that decodes to nothing is refused")
{
  VlmRpcServer server(serverInput());
  Client client(clientConfig(server.port()));
  const auto undecodable = refusalOf([&client] {
    return client.describe(
        {.jpeg = "not an image", .prompt = "who?", .cameraId = "", .maxTokens = 0});
  });
  CHECK(undecodable.status == 400);
  CHECK(undecodable.code == "BAD_REQUEST");
  CHECK(undecodable.message == "Image is not decodable");
}

TEST_CASE("client rejects wrong credential")
{
  VlmRpcServer server(serverInput());
  ClientConfig config = clientConfig(server.port());
  config.credential = "wrong";
  Client client(config);
  const DescribeInput input = ask();
  const auto untrusted = refusalOf([&client, &input] {
    return client.describe(input);
  });
  CHECK(untrusted.status == 401);
  CHECK(untrusted.code == "UNAUTHORIZED");
  const auto described = refusalOf([&client] {
    return client.capabilities();
  });
  CHECK(described.status == 401);
  CHECK(described.code == "UNAUTHORIZED");
}

TEST_CASE("server errors roundtrip typed response details")
{
  auto unloaded = serverInput();
  unloaded.capabilities = [] {
    auto reported = capabilities();
    reported.loaded = false;
    return reported;
  };
  unloaded.describe = [](const VisionDescribeMatInput&) -> std::string {
    throw ResponseException(503, VlmErrors::VisionEngineNotLoaded);
  };
  VlmRpcServer server(std::move(unloaded));
  Client client(clientConfig(server.port()));
  CHECK_FALSE(client.capabilities().loaded);
  const DescribeInput input = ask();
  const auto refused = refusalOf([&client, &input] {
    return client.describe(input);
  });
  CHECK(refused.status == 503);
  CHECK(refused.code == "VLM_NOT_LOADED");
  CHECK(refused.message == "Vision engine is not loaded");
}

TEST_CASE("failures outside the response contract are sanitized")
{
  auto input = serverInput();
  input.describe = [](const VisionDescribeMatInput&) -> std::string {
    throw std::runtime_error("engine stack secret");
  };
  VlmRpcServer server(std::move(input));
  Client client(clientConfig(server.port()));
  const DescribeInput ask_ = ask();
  const auto sanitized = refusalOf([&client, &ask_] {
    return client.describe(ask_);
  });
  CHECK(sanitized.status == 500);
  CHECK(sanitized.code == "INTERNAL_ERROR");
  CHECK(sanitized.message.find("secret") == std::string::npos);
}

TEST_CASE("invalid requests are refused before the engine runs")
{
  auto input = serverInput();
  std::atomic<int> calls{0};
  input.describe = [&calls](const VisionDescribeMatInput&) {
    ++calls;
    return std::string("unreachable");
  };
  VlmRpcServer server(std::move(input));
  Client client(clientConfig(server.port()));
  const DescribeInput good = ask();
  const auto refused = [&client](const DescribeInput& candidate) {
    return refusalOf(
        [&client, &candidate] { return client.describe(candidate); });
  };

  DescribeInput empty = good;
  empty.jpeg.clear();
  CHECK(refused(empty).status == 400);
  DescribeInput negative = good;
  negative.maxTokens = -1;
  CHECK(refused(negative).status == 400);
  DescribeInput huge = good;
  huge.maxTokens = 4097;
  CHECK(refused(huge).status == 400);
  CHECK(calls.load() == 0);
  CHECK(client.describe(good) == "unreachable");
  CHECK(calls.load() == 1);
}

TEST_CASE("the wire refuses what its own client would never send")
{
  VlmRpcServer server(serverInput());
  const auto stub = rawStub(server.port());
  const DescribeInput good = ask();
  const auto refused = [&stub](const RawCall& call) {
    return refusalOf([&stub, &call] { return rawDescribe(*stub, call); });
  };

  DescribeInput empty = good;
  empty.jpeg.clear();
  CHECK(refused({.input = empty}).status == 400);
  DescribeInput longPrompt = good;
  longPrompt.prompt = std::string(513, 'p');
  CHECK(refused({.input = longPrompt}).status == 400);
  DescribeInput longCamera = good;
  longCamera.cameraId = std::string(65, 'c');
  CHECK(refused({.input = longCamera}).status == 400);
  DescribeInput negative = good;
  negative.maxTokens = -1;
  CHECK(refused({.input = negative}).status == 400);
  DescribeInput huge = good;
  huge.maxTokens = 4097;
  CHECK(refused({.input = huge}).status == 400);
  CHECK(refused({.input = good, .deadline = std::chrono::seconds(300)}).status ==
        400);
  CHECK(refused({.input = good, .deadline = std::chrono::seconds(125)}).status ==
        400);
  CHECK(rawDescribe(*stub, {.input = good,
                            .deadline = std::chrono::seconds(120)}) ==
        expected(good));
  CHECK(refused({.input = good, .credential = ""}).status == 401);

  CHECK(rawDescribe(*stub, {.input = good}) == expected(good));
  DescribeInput bare = good;
  bare.prompt.clear();
  bare.cameraId.clear();
  bare.maxTokens = 4096;
  CHECK(rawDescribe(*stub, {.input = bare, .deadline = std::nullopt}) ==
        expected(bare));
}

TEST_CASE("the wire refuses a request that presents two credentials")
{
  VlmRpcServer server(serverInput());
  const auto stub = rawStub(server.port());
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(5));
  argus::client::addCallerCredential(context, kSecret);
  argus::client::addCallerCredential(context, kSecret);
  const wire::DescribeRequest request = wireRequest(ask());
  wire::DescribeResponse response;
  const auto status = stub->Describe(&context, request, &response);
  REQUIRE_FALSE(status.ok());
  CHECK(argus::response::fromRpcStatus(status).statusCode() == 401);
  CHECK(response.caption().empty());
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

TEST_CASE("deadline and external cancellation stop the description")
{
  std::counting_semaphore<1> deadlineEntered{0};
  std::latch deadlineRelease(1);
  std::atomic<bool> deadlineFinished{false};
  const HeldEngine deadlineEngine{.entered = deadlineEntered,
                                  .release = deadlineRelease,
                                  .finished = deadlineFinished};
  VlmRpcServer deadlineServer(heldInput(deadlineEngine));
  auto config = clientConfig(deadlineServer.port());
  config.timeout = std::chrono::milliseconds(80);
  Client deadlineClient(config);
  const DescribeInput input = ask();
  const auto expired = refusalOf([&deadlineClient, &input] {
    return deadlineClient.describe(input);
  });
  CHECK(expired.status == 504);
  CHECK(expired.code == "DEADLINE_EXCEEDED");
  deadlineRelease.count_down();
  deadlineServer.shutdown();
  CHECK(deadlineFinished.load());

  std::counting_semaphore<1> cancelEntered{0};
  std::latch cancelRelease(1);
  std::atomic<bool> cancelFinished{false};
  const HeldEngine cancelEngine{.entered = cancelEntered,
                                .release = cancelRelease,
                                .finished = cancelFinished};
  VlmRpcServer cancelServer(heldInput(cancelEngine));
  const auto stub = rawStub(cancelServer.port());
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(30));
  argus::client::addCallerCredential(context, kSecret);
  const wire::DescribeRequest request = wireRequest(ask());
  wire::DescribeResponse response;
  std::jthread canceller([&context, &cancelEntered] {
    if (!cancelEntered.try_acquire_for(kEntryWait))
      return;
    context.TryCancel();
  });
  const auto status = stub->Describe(&context, request, &response);
  cancelRelease.count_down();
  canceller.join();
  REQUIRE_FALSE(status.ok());
  const auto stopped = argus::response::fromRpcStatus(status);
  CHECK(stopped.statusCode() == 499);
  CHECK(stopped.errorCode() == "CANCELLED");
  cancelServer.shutdown();
  CHECK(cancelFinished.load());
}

TEST_CASE("a second call is refused while the only slot is held")
{
  std::counting_semaphore<1> entered{0};
  std::latch release(1);
  std::atomic<bool> finished{false};
  const HeldEngine engine{
      .entered = entered, .release = release, .finished = finished};
  VlmRpcServer server(heldInput(engine));
  Client client(clientConfig(server.port()));
  const DescribeInput input = ask();
  std::string held;
  std::jthread holder([&client, &held, &input] {
    held = client.describe(input);
  });
  (void)entered.try_acquire_for(kEntryWait);
  const auto busy = refusalOf([&client, &input] {
    return client.describe(input);
  });
  CHECK(busy.status == 429);
  CHECK(busy.code == "TOO_MANY_REQUESTS");
  release.count_down();
  holder.join();
  CHECK(held == "late");
  server.shutdown();
  CHECK(finished.load());
}

TEST_CASE("the guard facade takes the gRPC leg when the knob is set")
{
  REQUIRE(appLoop().ready());
  VlmRpcServer server(serverInput());
  ConfigService::setRuntimeString("vlm.grpc_target",
                                  "127.0.0.1:" + std::to_string(server.port()));
  ConfigService::setRuntimeString("vlm.grpc_credential", kSecret);
  const VlmClient facade("", 5.0);
  CHECK(facade.remote());
  const DescribeInput input = ask();
  const auto described =
      drogon::sync_wait(facade.describe({.jpeg = input.jpeg,
                                         .prompt = input.prompt,
                                         .cameraId = input.cameraId}));
  CHECK(described.value_or(VlmDescribeResult{}).caption == expected(input));

  ConfigService::setRuntimeString("vlm.grpc_target", "");
  ConfigService::setRuntimeString("vlm.grpc_credential", "");
  CHECK_FALSE(facade.remote());
  CHECK_FALSE(drogon::sync_wait(facade.describe({.jpeg = input.jpeg,
                                                 .prompt = input.prompt,
                                                 .cameraId = input.cameraId}))
                  .has_value());
  server.shutdown();
}

TEST_CASE("real engine describes through the gRPC leg")
{
  std::remove(kScratchConfig);
  {
    std::ofstream config(kScratchConfig);
    config << "[vision]\n"
           << "model_path = \"" << ARGUS_TEST_VLM_MODELS_DIR
           << "/lm-Q8_0.gguf\"\n"
           << "mmproj_path = \"" << ARGUS_TEST_VLM_MODELS_DIR
           << "/mmproj-F16.gguf\"\n"
           << "max_tokens = 64\n"
              "[drogon.app]\nnumber_of_threads = 2\n";
  }
  ConfigService::load(kScratchConfig);

  llama_backend_init();

  VlmController engine;
  engine.initEngine();
  REQUIRE_MESSAGE(engine.isEngineLoaded(),
                  "Vision engine failed to load from " ARGUS_TEST_VLM_MODELS_DIR
                  " — run scripts/setup.sh first");

  auto input = serverInput();
  input.capabilities = [&engine] {
    return Capabilities{.loaded = engine.isEngineLoaded(),
                        .maxInputPx = engine.service().maxInputPx(),
                        .defaultMaxTokens = engine.service().defaultMaxTokens()};
  };
  input.describe = [&engine](const VisionDescribeMatInput& describedInput) {
    return engine.service().describeMat(describedInput);
  };
  VlmRpcServer server(std::move(input));

  ConfigService::setRuntimeString("vlm.grpc_target",
                                  "127.0.0.1:" + std::to_string(server.port()));
  ConfigService::setRuntimeString("vlm.grpc_credential", kSecret);
  Client client(clientConfig(server.port(), std::chrono::seconds(120)));
  const std::string jpeg = jpegFixture(256);
  const std::string prompt = "Describe the dominant shapes in one line.";
  const std::string caption =
      client.describe({.jpeg = jpeg, .prompt = prompt, .cameraId = "cam-1"});
  const std::string inProcess = engine.service().describeMat(
      {.bgr = decodedJpeg(jpeg), .prompt = prompt, .maxTokens = 0});
  CHECK_FALSE(caption.empty());
  CHECK(caption == inProcess);
  CHECK(client.capabilities().loaded);

  server.shutdown();
  engine.shutdownEngine();
  llama_backend_free();
  ConfigService::setRuntimeString("vlm.grpc_target", "");
  ConfigService::setRuntimeString("vlm.grpc_credential", "");
}

TEST_CASE("a listener whose callers are all placeholders refuses to start")
{
  VlmRpcInput input = serverInput();
  input.credentials = {{"voice", "CHANGE_ME_VOICE_VLM"}, {"settings", "CHANGE_ME_SETTINGS_VLM"}};
  CHECK_THROWS_AS(VlmRpcServer{std::move(input)}, std::invalid_argument);
}
