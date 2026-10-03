#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <app/rpc/llm-rpc-server.hxx>
#include <config/config-service.hxx>
#include <errors/response-exception.hxx>
#include <feature/llm/controllers/llm-controller.hxx>
#include <grpc/grpc-client-base.hxx>
#include <llama.h>
#include <llm.grpc.pb.h>
#include <llm/llm-client.hxx>
#include <llm/llm-errors.hxx>
#include <llm/llm-service.hxx>
#include <response.pb.h>
#include <response/response-rpc.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
#ifndef ARGUS_TEST_LLM_MODELS_DIR
#define ARGUS_TEST_LLM_MODELS_DIR "models/llm"
#endif

namespace wire = argus::llm::v1;

using argus::llm::Capabilities;
using argus::llm::Client;
using argus::llm::ClientConfig;
using Clock = std::chrono::system_clock;

constexpr const char* kSecret = "rpc-secret";
constexpr const char* kScratchConfig = "/tmp/argus-llm-rpc-test.toml";

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

std::vector<std::string> tokens()
{
  return {"Hola", " de", " nuevo", "."};
}

Capabilities capabilities()
{
  return {.loaded = true,
          .defaultMaxTokens = 96,
          .defaultTemperature = 0.3F,
          .contextSize = 4096,
          .lastPromptTokens = 12,
          .lastReusedTokens = 2,
          .lastDecodedTokens = 30};
}

std::string echo(const ChatRequest& request)
{
  std::string text;
  for (const auto& message : request.messages)
    text += message.role + "=" + message.content + ";";
  text += "max=" + std::to_string(request.maxTokens);
  text += " temp=" + std::to_string(request.temperature);
  text += " reset=" + std::string(request.resetContext ? "1" : "0");
  text += " tools=" + std::string(request.toolsEnabled ? "1" : "0");
  text += " user=" + std::to_string(request.userId);
  text += " grammar=" + std::to_string(request.grammar.size());
  text += " required=" + std::string(request.grammarRequired ? "1" : "0");
  return text;
}

LlmChatOutcome answered(const ChatRequest& request)
{
  return {.text = echo(request),
          .hops = 1,
          .toolCalls = 2,
          .generateMs = 3,
          .toolMs = 4};
}

void streamed(const LlmStreamInput& input)
{
  for (const auto& token : tokens())
    input.onToken(token, false);
  if (input.stats) {
    input.stats->promptTokens = 7;
    input.stats->reusedTokens = 2;
    input.stats->decodedTokens = 4;
  }
  input.onToken("", true);
}

void silent(const LlmStreamInput& input)
{
  if (input.stats) {
    input.stats->promptTokens = 5;
    input.stats->reusedTokens = 5;
    input.stats->decodedTokens = 0;
  }
  input.onToken("", true);
}

LlmRpcInput serverInput()
{
  return {.address = "127.0.0.1:0",
          .credentials = {{"guard", kSecret}},
          .capabilities = capabilities,
          .chat = answered,
          .chatStream = streamed,
          .slots = 1};
}

ClientConfig clientConfig(int port,
                          std::chrono::milliseconds timeout =
                              std::chrono::seconds(5))
{
  return {.target = "127.0.0.1:" + std::to_string(port),
          .credential = kSecret,
          .timeout = timeout};
}

ChatRequest ask()
{
  ChatRequest request;
  request.messages = {{.role = "user", .content = "Di hola"}};
  request.maxTokens = 16;
  return request;
}

wire::ChatRequest wireRequest(const ChatRequest& input)
{
  wire::ChatRequest request;
  for (const auto& message : input.messages) {
    auto* entry = request.add_messages();
    entry->set_role(message.role);
    entry->set_content(message.content);
  }
  request.set_max_tokens(input.maxTokens);
  request.set_temperature(input.temperature);
  request.set_reset_context(input.resetContext);
  request.set_tools(input.toolsEnabled);
  request.set_user_id(input.userId);
  request.set_grammar(input.grammar);
  request.set_grammar_required(input.grammarRequired);
  return request;
}

struct RawCall
{
  ChatRequest input;
  std::string credential{kSecret};
  std::optional<std::chrono::seconds> deadline{std::chrono::seconds(5)};
};

std::unique_ptr<wire::Chat::Stub> rawStub(int port)
{
  return wire::Chat::NewStub(
      argus::client::makeChannel("127.0.0.1:" + std::to_string(port)));
}

std::string rawChat(wire::Chat::Stub& stub, const RawCall& call)
{
  grpc::ClientContext context;
  if (call.deadline)
    context.set_deadline(Clock::now() + *call.deadline);
  if (!call.credential.empty())
    argus::client::addCallerCredential(context, call.credential);
  const wire::ChatRequest request = wireRequest(call.input);
  wire::ChatResponse response;
  const auto status = stub.Chat(&context, request, &response);
  if (!status.ok())
    throw argus::response::fromRpcStatus(status);
  return response.text();
}

struct RawStream
{
  std::vector<std::string> texts;
  std::vector<std::uint32_t> sequences;
  std::uint32_t closing{0};
  bool closed{false};
};

RawStream rawStream(wire::Chat::Stub& stub, const RawCall& call)
{
  grpc::ClientContext context;
  if (call.deadline)
    context.set_deadline(Clock::now() + *call.deadline);
  if (!call.credential.empty())
    argus::client::addCallerCredential(context, call.credential);
  const wire::ChatRequest request = wireRequest(call.input);
  const auto reader = stub.ChatStream(&context, request);
  RawStream arrived;
  wire::ChatToken token;
  while (reader->Read(&token)) {
    if (token.done()) {
      arrived.closed = true;
      arrived.closing = token.sequence();
    }
    else {
      arrived.texts.push_back(token.text());
      arrived.sequences.push_back(token.sequence());
    }
  }
  const auto status = reader->Finish();
  if (!status.ok())
    throw argus::response::fromRpcStatus(status);
  return arrived;
}

constexpr auto kEntryWait = std::chrono::seconds(10);

struct HeldEngine
{
  std::counting_semaphore<1>& entered;
  std::latch& release;
  std::atomic<bool>& finished;
};

struct LatchRelease
{
  std::latch& latch;
  ~LatchRelease() { latch.count_down(); }
};

LlmRpcInput heldChat(const HeldEngine& engine)
{
  auto input = serverInput();
  input.chat = [engine](const ChatRequest&) {
    engine.entered.release();
    engine.release.wait();
    engine.finished = true;
    return LlmChatOutcome{.text = "late",
                          .hops = 0,
                          .toolCalls = 0,
                          .generateMs = 0,
                          .toolMs = 0};
  };
  return input;
}

LlmRpcInput heldStream(const HeldEngine& engine)
{
  auto input = serverInput();
  input.chatStream = [engine](const LlmStreamInput& stream) {
    engine.entered.release();
    engine.release.wait();
    engine.finished = true;
    stream.onToken("late", false);
    stream.onToken("", true);
  };
  return input;
}
}

TEST_CASE("capabilities reports engine metadata")
{
  LlmRpcServer server(serverInput());
  Client client(clientConfig(server.port()));
  const auto reported = client.capabilities();
  CHECK(reported.loaded);
  CHECK(reported.defaultMaxTokens == 96);
  CHECK(reported.defaultTemperature == doctest::Approx(0.3F));
  CHECK(reported.contextSize == 4096);
  CHECK(reported.lastPromptTokens == 12);
  CHECK(reported.lastReusedTokens == 2);
  CHECK(reported.lastDecodedTokens == 30);
}

TEST_CASE("a capabilities reply outside the wire contract is refused")
{
  auto empty = serverInput();
  empty.capabilities = [] {
    auto reported = capabilities();
    reported.contextSize = 0;
    return reported;
  };
  LlmRpcServer emptyServer(std::move(empty));
  Client emptyClient(clientConfig(emptyServer.port()));
  const auto unreadable =
      refusalOf([&emptyClient] { return emptyClient.capabilities(); });
  CHECK(unreadable.status == 502);
  CHECK(unreadable.code == "BAD_GATEWAY");

  auto unbounded = serverInput();
  unbounded.capabilities = [] {
    auto reported = capabilities();
    reported.defaultMaxTokens = 99999;
    return reported;
  };
  LlmRpcServer unboundedServer(std::move(unbounded));
  Client unboundedClient(clientConfig(unboundedServer.port()));
  const auto bound =
      refusalOf([&unboundedClient] { return unboundedClient.capabilities(); });
  CHECK(bound.status == 502);
  CHECK(bound.code == "BAD_GATEWAY");

  auto negative = serverInput();
  negative.capabilities = [] {
    auto reported = capabilities();
    reported.lastPromptTokens = -1;
    return reported;
  };
  LlmRpcServer negativeServer(std::move(negative));
  Client negativeClient(clientConfig(negativeServer.port()));
  const auto counter =
      refusalOf([&negativeClient] { return negativeClient.capabilities(); });
  CHECK(counter.status == 502);
  CHECK(counter.code == "BAD_GATEWAY");
}

TEST_CASE("chat carries the request to the engine and answers its completion")
{
  LlmRpcServer server(serverInput());
  Client client(clientConfig(server.port()));

  ChatRequest request = ask();
  request.temperature = 0.1F;
  request.resetContext = true;
  request.toolsEnabled = false;
  request.userId = 7;
  request.grammar = "root ::= \"a\"";
  request.grammarRequired = true;
  CHECK(client.chat(request) == echo(request));

  ChatRequest bare = ask();
  bare.messages = {{.role = "system", .content = "Eres Argus."},
                   {.role = "user", .content = "Di hola"}};
  bare.maxTokens = 0;
  CHECK(client.chat(bare) == echo(bare));
}

TEST_CASE("a generation that produces no text is a legitimate answer")
{
  auto input = serverInput();
  input.chat = [](const ChatRequest&) {
    return LlmChatOutcome{.text = "",
                          .hops = 0,
                          .toolCalls = 0,
                          .generateMs = 0,
                          .toolMs = 0};
  };
  input.chatStream = silent;
  LlmRpcServer server(std::move(input));
  Client client(clientConfig(server.port()));

  CHECK(client.chat(ask()).empty());

  LlmPrefillStats stats;
  bool done = false;
  client.chatStream({.request = ask(),
                     .onToken = [&done](const std::string&, bool atEnd) {
                       done = atEnd;
                     },
                     .stats = &stats,
                     .cancellation = {}});
  CHECK(done);
  CHECK(stats.promptTokens == 5);
  CHECK(stats.reusedTokens == 5);
  CHECK(stats.decodedTokens == 0);
}

TEST_CASE("chat-stream streams tokens in sequence and closes with the counters")
{
  LlmRpcServer server(serverInput());
  Client client(clientConfig(server.port()));

  std::vector<std::string> arrived;
  LlmPrefillStats stats;
  bool done = false;
  client.chatStream({.request = ask(),
                     .onToken = [&](const std::string& token, bool atEnd) {
                       if (atEnd)
                         done = true;
                       else
                         arrived.push_back(token);
                     },
                     .stats = &stats,
                     .cancellation = {}});
  CHECK(done);
  CHECK(arrived == tokens());
  CHECK(stats.promptTokens == 7);
  CHECK(stats.reusedTokens == 2);
  CHECK(stats.decodedTokens == 4);

  const auto stub = rawStub(server.port());
  const auto streamedWire = rawStream(*stub, {.input = ask()});
  CHECK(streamedWire.texts == tokens());
  CHECK(streamedWire.sequences == std::vector<std::uint32_t>{0, 1, 2, 3});
  CHECK(streamedWire.closed);
  CHECK(streamedWire.closing == 4);
  ChatRequest bare = ask();
  bare.maxTokens = 4096;
  const auto undeclared =
      rawStream(*stub, {.input = bare, .deadline = std::nullopt});
  CHECK(undeclared.texts == tokens());
  CHECK(undeclared.closing == 4);
}

TEST_CASE("an undeclared temperature and tools take the engine's defaults")
{
  LlmRpcServer server(serverInput());
  const auto stub = rawStub(server.port());
  const auto answer = [&stub](const wire::ChatRequest& request) {
    grpc::ClientContext context;
    context.set_deadline(Clock::now() + std::chrono::seconds(5));
    argus::client::addCallerCredential(context, kSecret);
    wire::ChatResponse response;
    if (stub->Chat(&context, request, &response).ok())
      return std::optional<std::string>{response.text()};
    return std::optional<std::string>{};
  };

  wire::ChatRequest request;
  auto* message = request.add_messages();
  message->set_role("user");
  message->set_content("Di hola");
  CHECK(answer(request) ==
        std::optional<std::string>{
            "user=Di hola;max=0 temp=-1.000000 reset=0 tools=1 user=0 "
            "grammar=0 required=0"});

  request.set_temperature(0.0F);
  request.set_tools(false);
  CHECK(answer(request) ==
        std::optional<std::string>{
            "user=Di hola;max=0 temp=0.000000 reset=0 tools=0 user=0 "
            "grammar=0 required=0"});
}

TEST_CASE("client rejects wrong credential")
{
  LlmRpcServer server(serverInput());
  ClientConfig config = clientConfig(server.port());
  config.credential = "wrong";
  Client client(config);

  const auto reported = refusalOf([&client] { return client.capabilities(); });
  CHECK(reported.status == 401);
  CHECK(reported.code == "UNAUTHORIZED");

  const ChatRequest request = ask();
  const auto single = refusalOf([&client, &request] {
    return client.chat(request);
  });
  CHECK(single.status == 401);
  CHECK(single.code == "UNAUTHORIZED");

  bool consumed = false;
  const auto streaming = refusalOf([&client, &request, &consumed] {
    client.chatStream({.request = request,
                       .onToken = [&consumed](const std::string&, bool) {
                         consumed = true;
                       },
                       .stats = nullptr,
                       .cancellation = {}});
  });
  CHECK(streaming.status == 401);
  CHECK(streaming.code == "UNAUTHORIZED");
  CHECK_FALSE(consumed);
}

TEST_CASE("server errors roundtrip typed response details")
{
  auto unloaded = serverInput();
  unloaded.capabilities = [] {
    auto reported = capabilities();
    reported.loaded = false;
    return reported;
  };
  unloaded.chat = [](const ChatRequest&) -> LlmChatOutcome {
    throw ResponseException(503, LlmErrors::LlmEngineNotLoaded);
  };
  unloaded.chatStream = [](const LlmStreamInput&) {
    throw ResponseException(503, LlmErrors::LlmEngineNotLoaded);
  };
  LlmRpcServer server(std::move(unloaded));
  Client client(clientConfig(server.port()));
  CHECK_FALSE(client.capabilities().loaded);

  const ChatRequest request = ask();
  const auto refused = refusalOf([&client, &request] {
    return client.chat(request);
  });
  CHECK(refused.status == 503);
  CHECK(refused.code == "LLM_NOT_LOADED");
  CHECK(refused.message == "LLM engine is not loaded");

  const auto streamRefused = refusalOf([&client, &request] {
    client.chatStream({.request = request,
                       .onToken = [](const std::string&, bool) {},
                       .stats = nullptr,
                       .cancellation = {}});
  });
  CHECK(streamRefused.status == 503);
  CHECK(streamRefused.code == "LLM_NOT_LOADED");

  auto busy = serverInput();
  busy.chat = [](const ChatRequest&) -> LlmChatOutcome {
    throw ResponseException(429, LlmErrors::Busy);
  };
  LlmRpcServer busyServer(std::move(busy));
  Client busyClient(clientConfig(busyServer.port()));
  const auto throttled = refusalOf([&busyClient, &request] {
    return busyClient.chat(request);
  });
  CHECK(throttled.status == 429);
  CHECK(throttled.code == "TOO_MANY_REQUESTS");
}

TEST_CASE("failures outside the response contract are sanitized")
{
  auto input = serverInput();
  input.chat = [](const ChatRequest&) -> LlmChatOutcome {
    throw std::runtime_error("engine stack secret");
  };
  input.chatStream = [](const LlmStreamInput&) {
    throw std::runtime_error("engine stack secret");
  };
  LlmRpcServer server(std::move(input));
  Client client(clientConfig(server.port()));
  const ChatRequest request = ask();

  const auto sanitized = refusalOf([&client, &request] {
    return client.chat(request);
  });
  CHECK(sanitized.status == 500);
  CHECK(sanitized.code == "INTERNAL_ERROR");
  CHECK(sanitized.message.find("secret") == std::string::npos);

  const auto streamSanitized = refusalOf([&client, &request] {
    client.chatStream({.request = request,
                       .onToken = [](const std::string&, bool) {},
                       .stats = nullptr,
                       .cancellation = {}});
  });
  CHECK(streamSanitized.status == 500);
  CHECK(streamSanitized.code == "INTERNAL_ERROR");
  CHECK(streamSanitized.message.find("secret") == std::string::npos);
}

TEST_CASE("invalid requests are refused before the engine runs")
{
  auto input = serverInput();
  std::atomic<int> calls{0};
  input.chat = [&calls](const ChatRequest& request) {
    ++calls;
    return answered(request);
  };
  LlmRpcServer server(std::move(input));
  Client client(clientConfig(server.port()));

  const ChatRequest good = ask();
  const auto refused = [&client](const ChatRequest& candidate) {
    return refusalOf([&client, &candidate] { return client.chat(candidate); });
  };

  ChatRequest empty = good;
  empty.messages.clear();
  CHECK(refused(empty).status == 400);
  ChatRequest blank = good;
  blank.messages = {{.role = "user", .content = ""}};
  CHECK(refused(blank).status == 400);
  ChatRequest roleless = good;
  roleless.messages = {{.role = "", .content = "Di hola"}};
  CHECK(refused(roleless).status == 400);
  ChatRequest huge = good;
  huge.messages = {{.role = "user", .content = std::string(32 * 1024 + 1, 'c')}};
  CHECK(refused(huge).status == 400);
  ChatRequest many = good;
  many.messages.assign(65, ChatMessage{.role = "user", .content = "Di hola"});
  CHECK(refused(many).status == 400);
  ChatRequest longGrammar = good;
  longGrammar.grammar = std::string(8 * 1024 + 1, 'g');
  CHECK(refused(longGrammar).status == 400);
  ChatRequest negativeUser = good;
  negativeUser.userId = -1;
  CHECK(refused(negativeUser).status == 400);

  CHECK(calls.load() == 0);
  CHECK(client.chat(good) == echo(good));
  CHECK(calls.load() == 1);
}

TEST_CASE("the wire refuses what its own client would never send")
{
  LlmRpcServer server(serverInput());
  const auto stub = rawStub(server.port());
  const ChatRequest good = ask();
  const auto refused = [&stub](const RawCall& call) {
    return refusalOf([&stub, &call] { return rawChat(*stub, call); });
  };

  ChatRequest longRole = good;
  longRole.messages = {{.role = std::string(33, 'r'), .content = "Di hola"}};
  CHECK(refused({.input = longRole}).status == 400);
  ChatRequest huge = good;
  huge.messages = {{.role = "user", .content = std::string(32 * 1024 + 1, 'c')}};
  CHECK(refused({.input = huge}).status == 400);
  ChatRequest many = good;
  many.messages.assign(65, ChatMessage{.role = "user", .content = "Di hola"});
  CHECK(refused({.input = many}).status == 400);
  ChatRequest negative = good;
  negative.maxTokens = -1;
  CHECK(refused({.input = negative}).status == 400);
  ChatRequest oversized = good;
  oversized.maxTokens = 4097;
  CHECK(refused({.input = oversized}).status == 400);
  ChatRequest hot = good;
  hot.temperature = 2.5F;
  CHECK(refused({.input = hot}).status == 400);
  ChatRequest longGrammar = good;
  longGrammar.grammar = std::string(8 * 1024 + 1, 'g');
  CHECK(refused({.input = longGrammar}).status == 400);
  ChatRequest negativeUser = good;
  negativeUser.userId = -1;
  CHECK(refused({.input = negativeUser}).status == 400);
  CHECK(refused({.input = good, .deadline = std::chrono::seconds(300)}).status ==
        400);
  CHECK(refused({.input = good, .deadline = std::chrono::seconds(125)}).status ==
        400);
  CHECK(rawChat(*stub, {.input = good,
                        .deadline = std::chrono::seconds(120)}) == echo(good));
  CHECK(refused({.input = good, .credential = ""}).status == 401);

  CHECK(rawChat(*stub, {.input = good}) == echo(good));
  ChatRequest bare = good;
  bare.maxTokens = 4096;
  bare.temperature = -1.0F;
  CHECK(rawChat(*stub, {.input = bare, .deadline = std::nullopt}) ==
        echo(bare));
}

TEST_CASE("the wire refuses a request that presents two credentials")
{
  LlmRpcServer server(serverInput());
  const auto stub = rawStub(server.port());
  grpc::ClientContext context;
  context.set_deadline(Clock::now() + std::chrono::seconds(5));
  argus::client::addCallerCredential(context, kSecret);
  argus::client::addCallerCredential(context, kSecret);
  wire::CapabilitiesRequest request;
  wire::CapabilitiesResponse response;
  const auto status = stub->Capabilities(&context, request, &response);
  REQUIRE_FALSE(status.ok());
  CHECK(argus::response::fromRpcStatus(status).statusCode() == 401);
  CHECK_FALSE(response.loaded());
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

TEST_CASE("deadline and external cancellation stop generation")
{
  std::counting_semaphore<1> deadlineEntered{0};
  std::latch deadlineRelease(1);
  std::atomic<bool> deadlineFinished{false};
  const HeldEngine deadlineEngine{.entered = deadlineEntered,
                                  .release = deadlineRelease,
                                  .finished = deadlineFinished};
  LlmRpcServer deadlineServer(heldStream(deadlineEngine));
  Client deadlineClient(
      clientConfig(deadlineServer.port(), std::chrono::milliseconds(80)));
  const ChatRequest request = ask();
  {
    const LatchRelease releaseHold{deadlineRelease};
    const auto expired = refusalOf([&deadlineClient, &request] {
      deadlineClient.chatStream({.request = request,
                                 .onToken = [](const std::string&, bool) {},
                                 .stats = nullptr,
                                 .cancellation = {}});
    });
    CHECK(expired.status == 504);
    CHECK(expired.code == "DEADLINE_EXCEEDED");
  }
  deadlineServer.shutdown();
  CHECK(deadlineFinished.load());

  std::counting_semaphore<1> cancelEntered{0};
  std::latch cancelRelease(1);
  std::atomic<bool> cancelFinished{false};
  const HeldEngine cancelEngine{.entered = cancelEntered,
                                .release = cancelRelease,
                                .finished = cancelFinished};
  LlmRpcServer cancelServer(heldStream(cancelEngine));
  {
    const LatchRelease releaseHold{cancelRelease};
    const auto stub = rawStub(cancelServer.port());
    grpc::ClientContext context;
    context.set_deadline(Clock::now() + std::chrono::seconds(30));
    argus::client::addCallerCredential(context, kSecret);
    const wire::ChatRequest wire = wireRequest(request);
    const auto reader = stub->ChatStream(&context, wire);
    std::jthread canceller([&context, &cancelEntered] {
      if (!cancelEntered.try_acquire_for(kEntryWait))
        return;
      context.TryCancel();
    });
    wire::ChatToken token;
    while (reader->Read(&token)) {
    }
    const auto status = reader->Finish();
    canceller.join();
    REQUIRE_FALSE(status.ok());
    const auto stopped = argus::response::fromRpcStatus(status);
    CHECK(stopped.statusCode() == 499);
    CHECK(stopped.errorCode() == "CANCELLED");
  }
  cancelServer.shutdown();
  CHECK(cancelFinished.load());
}

TEST_CASE("the caller's role and language cross the wire, an absent role is a guest")
{
  std::mutex seenMutex;
  std::vector<ChatRequest> seen;
  auto input = serverInput();
  input.chat = [&seen, &seenMutex](const ChatRequest& request) {
    std::scoped_lock lock(seenMutex);
    seen.push_back(request);
    return LlmChatOutcome{.text = "ok",
                          .hops = 0,
                          .toolCalls = 0,
                          .generateMs = 0,
                          .toolMs = 0};
  };
  LlmRpcServer server(std::move(input));
  Client client(clientConfig(server.port()));

  ChatRequest owner = ask();
  owner.role = UserRole::Owner;
  owner.lang = "en";
  CHECK(client.chat(owner) == "ok");
  ChatRequest guard = ask();
  guard.role = UserRole::Guard;
  CHECK(client.chat(guard) == "ok");

  const auto stub = rawStub(server.port());
  CHECK(rawChat(*stub, {.input = ask()}) == "ok");

  std::scoped_lock lock(seenMutex);
  REQUIRE(seen.size() == 3);
  CHECK(seen[0].role == UserRole::Owner);
  CHECK(seen[0].lang == "en");
  CHECK(seen[1].role == UserRole::Guard);
  CHECK(seen[1].lang.empty());
  CHECK(seen[2].role == UserRole::Guest);
  CHECK(seen[2].lang.empty());
}

TEST_CASE("the wire refuses a language the tool runtime does not speak")
{
  LlmRpcServer server(serverInput());
  const auto stub = rawStub(server.port());
  grpc::ClientContext context;
  context.set_deadline(Clock::now() + std::chrono::seconds(5));
  argus::client::addCallerCredential(context, kSecret);
  wire::ChatRequest request = wireRequest(ask());
  request.set_lang("fr");
  wire::ChatResponse response;
  const auto status = stub->Chat(&context, request, &response);
  REQUIRE_FALSE(status.ok());
  CHECK(argus::response::fromRpcStatus(status).statusCode() == 400);
}

TEST_CASE("a client stop token cancels the stream and frees the slot for the next turn")
{
  std::atomic<bool> engineStopped{false};
  std::atomic<int> engineCalls{0};
  auto input = serverInput();
  input.chatStream = [&engineStopped, &engineCalls](const LlmStreamInput& stream) {
    if (engineCalls.fetch_add(1) > 0) {
      stream.onToken("next", false);
      stream.onToken("", true);
      return;
    }
    try {
      for (int i = 0; i < 400; ++i) {
        stream.onToken(" palabra", false);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    catch (...) {
      engineStopped = true;
      throw;
    }
    stream.onToken("", true);
  };
  LlmRpcServer server(std::move(input));
  Client client(clientConfig(server.port(), std::chrono::seconds(30)));

  std::stop_source stop;
  int arrived = 0;
  const auto started = std::chrono::steady_clock::now();
  const auto cancelled = refusalOf([&] {
    client.chatStream({.request = ask(),
                       .onToken =
                           [&](const std::string&, bool atEnd) {
                             if (!atEnd && ++arrived == 3)
                               stop.request_stop();
                           },
                       .stats = nullptr,
                       .cancellation = stop.get_token()});
  });
  CHECK(cancelled.status == 499);
  CHECK(cancelled.code == "CANCELLED");
  CHECK(arrived == 3);
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));

  std::string next;
  client.chatStream({.request = ask(),
                     .onToken =
                         [&next](const std::string& token, bool atEnd) {
                           if (!atEnd)
                             next += token;
                         },
                     .stats = nullptr,
                     .cancellation = {}});
  CHECK(next == "next");
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
  CHECK(engineStopped.load());

  std::stop_source early;
  early.request_stop();
  const auto refused = refusalOf([&] {
    client.chatStream({.request = ask(),
                       .onToken = [](const std::string&, bool) {},
                       .stats = nullptr,
                       .cancellation = early.get_token()});
  });
  CHECK(refused.status == 499);
  CHECK(engineCalls.load() == 2);
  server.shutdown();
}

TEST_CASE("a second call waits for the only slot instead of being refused")
{
  std::counting_semaphore<1> entered{0};
  std::latch release(1);
  std::atomic<bool> finished{false};
  const HeldEngine engine{
      .entered = entered, .release = release, .finished = finished};
  LlmRpcServer server(heldChat(engine));
  Client client(clientConfig(server.port()));
  const ChatRequest request = ask();
  std::string held;
  std::jthread holder([&client, &held, &request] {
    held = client.chat(request);
  });
  REQUIRE(entered.try_acquire_for(kEntryWait));

  std::atomic<bool> queuedDone{false};
  std::string queued;
  std::jthread waiter([&client, &queued, &queuedDone, &request] {
    queued = client.chat(request);
    queuedDone = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK_FALSE(queuedDone.load());

  release.count_down();
  holder.join();
  waiter.join();
  CHECK(held == "late");
  CHECK(queued == "late");
  server.shutdown();
}

TEST_CASE("a queued call gives up when its deadline passes")
{
  std::counting_semaphore<1> entered{0};
  std::latch release(1);
  std::atomic<bool> finished{false};
  const HeldEngine engine{
      .entered = entered, .release = release, .finished = finished};
  LlmRpcServer server(heldChat(engine));
  Client client(clientConfig(server.port()));
  Client impatient(clientConfig(server.port(), std::chrono::milliseconds(300)));
  const ChatRequest request = ask();
  std::string held;
  std::jthread holder([&client, &held, &request] {
    held = client.chat(request);
  });
  REQUIRE(entered.try_acquire_for(kEntryWait));
  {
    const LatchRelease releaseHold{release};
    const auto expired = refusalOf([&impatient, &request] {
      return impatient.chat(request);
    });
    CHECK(expired.status == 504);
  }
  holder.join();
  CHECK(held == "late");
  server.shutdown();
  CHECK(finished.load());
}

TEST_CASE("real engine answers through the gRPC leg")
{
  std::remove(kScratchConfig);
  {
    std::ofstream config(kScratchConfig);
    config << "[llm]\n"
           << "model_path = \"" << ARGUS_TEST_LLM_MODELS_DIR
           << "/LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf\"\n"
           << "context_size = 4096\n"
           << "max_tokens = 16\n"
           << "temperature = 0.3\n"
           << "top_k = 20\n"
           << "top_p = 0.8\n"
           << "penalty_last_n = 64\n"
           << "penalty_repeat = 1.10\n"
           << "seed = 42\n"
              "[drogon.app]\nnumber_of_threads = 2\n";
  }
  ConfigService::load(kScratchConfig);

  llama_backend_init();

  LlmController engine;
  engine.initEngine();
  REQUIRE_MESSAGE(engine.isEngineLoaded(),
                  "LLM engine failed to load from " ARGUS_TEST_LLM_MODELS_DIR
                  " — run scripts/setup.sh first");

  auto input = serverInput();
  input.capabilities = [&engine] {
    const auto stats = engine.service().lastPrefillStats();
    return Capabilities{.loaded = engine.isEngineLoaded(),
                        .defaultMaxTokens = engine.service().defaultMaxTokens(),
                        .defaultTemperature =
                            engine.service().defaultTemperature(),
                        .contextSize = engine.service().contextSize(),
                        .lastPromptTokens = stats.promptTokens,
                        .lastReusedTokens = stats.reusedTokens,
                        .lastDecodedTokens = stats.decodedTokens};
  };
  input.chat = [&engine](const ChatRequest& request) {
    return engine.chatSync(request);
  };
  input.chatStream = [&engine](const LlmStreamInput& stream) {
    engine.chatStreamSync(stream);
  };
  LlmRpcServer server(std::move(input));
  Client client(clientConfig(server.port(), std::chrono::seconds(120)));

  ChatRequest request;
  request.messages = {{.role = "user", .content = "Di hola en una palabra."}};
  request.maxTokens = 8;
  request.temperature = 0.1F;
  const std::string text = client.chat(request);
  CHECK_FALSE(text.empty());

  std::string streamedText;
  LlmPrefillStats stats;
  bool done = false;
  client.chatStream({.request = request,
                     .onToken = [&](const std::string& token, bool atEnd) {
                       if (atEnd)
                         done = true;
                       else
                         streamedText += token;
                     },
                     .stats = &stats,
                     .cancellation = {}});
  CHECK(done);
  CHECK_FALSE(streamedText.empty());
  CHECK(stats.promptTokens > 0);
  CHECK(stats.decodedTokens > 0);
  CHECK(client.capabilities().loaded);

  server.shutdown();
  engine.shutdownEngine();
  llama_backend_free();
}
