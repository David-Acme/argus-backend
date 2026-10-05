#include "llm-rpc-server.hxx"

#include <errors/response-exception.hxx>
#include <grpc/fleet-caller-gate.hxx>
#include <llm.grpc.pb.h>
#include <llm/llm-errors.hxx>
#include <response/response-rpc.hxx>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
namespace wire = argus::llm::v1;
using Clock = std::chrono::system_clock;

constexpr std::size_t kMaxMessages = 64;
constexpr std::size_t kMaxRoleBytes = 32;
constexpr std::size_t kMaxContentBytes = std::size_t{32} * 1024;
constexpr std::size_t kMaxGrammarBytes = std::size_t{8} * 1024;
constexpr std::size_t kMaxSessionBytes = 128;
constexpr int32_t kMaxTokensBound = 4096;
constexpr std::chrono::seconds kTimeoutHeaderRounding{1};
constexpr auto kMaxDeadline = argus::llm::kMaxTimeout + kTimeoutHeaderRounding;
constexpr int kMaxReceiveBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxQueuedTokens = 64;
constexpr std::chrono::milliseconds kSlotPoll{20};
constexpr std::chrono::seconds kShutdownGrace{2};

struct StreamItem
{
  std::string text;
  std::optional<ClientAction> action;
};

struct StreamQueue
{
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<StreamItem> tokens;
  bool done{false};
  bool stopped{false};
  grpc::Status status;
  LlmPrefillStats stats;
};

ResponseException stoppedError(const grpc::ServerContext& context)
{
  if (Clock::now() >= context.deadline())
    return {504, LlmErrors::DeadlineExceeded};
  return {499, LlmErrors::Cancelled};
}

bool validRequest(const wire::ChatRequest& request)
{
  const auto count = static_cast<std::size_t>(request.messages_size());
  if (count == 0 || count > kMaxMessages)
    return false;
  for (const auto& message : request.messages()) {
    if (message.role().empty() || message.role().size() > kMaxRoleBytes)
      return false;
    if (message.content().empty() || message.content().size() > kMaxContentBytes)
      return false;
  }
  return request.max_tokens() >= 0 && request.max_tokens() <= kMaxTokensBound &&
         (!request.has_temperature() ||
          (request.temperature() >= -1.0F && request.temperature() <= 2.0F)) &&
         request.grammar().size() <= kMaxGrammarBytes && request.user_id() >= 0 &&
         (request.lang().empty() || request.lang() == "es" ||
          request.lang() == "en") &&
         request.session_id().size() <= kMaxSessionBytes;
}

UserRole callerRole(wire::CallerRole role)
{
  switch (role) {
    case wire::CALLER_ROLE_OWNER:
      return UserRole::Owner;
    case wire::CALLER_ROLE_RESIDENT:
      return UserRole::Resident;
    case wire::CALLER_ROLE_GUARD:
      return UserRole::Guard;
    default:
      break;
  }
  return UserRole::Guest;
}

ChatRequest chatRequest(const wire::ChatRequest& wireRequest, std::string_view caller)
{
  ChatRequest request;
  request.messages.reserve(wireRequest.messages_size());
  for (const auto& message : wireRequest.messages())
    request.messages.push_back(
        {.role = message.role(), .content = message.content()});
  request.maxTokens = wireRequest.max_tokens();
  request.temperature =
      wireRequest.has_temperature() ? wireRequest.temperature() : -1.0F;
  request.resetContext = wireRequest.reset_context();
  request.toolsEnabled = wireRequest.has_tools() ? wireRequest.tools() : true;
  request.userId = wireRequest.user_id();
  request.grammar = wireRequest.grammar();
  request.grammarRequired = wireRequest.grammar_required();
  request.role = callerRole(wireRequest.caller_role());
  request.lang = wireRequest.lang();
  request.clientActions = wireRequest.client_actions();
  request.sessionId = wireRequest.session_id();
  request.prefillOnly = wireRequest.prefill_only();
  return boundToCaller(std::move(request), caller);
}

bool deadlineTooFar(const grpc::ServerContext& context)
{
  return context.deadline() != Clock::time_point::max() &&
         context.deadline() > Clock::now() + kMaxDeadline;
}

bool stopped(const grpc::ServerContext& context)
{
  return context.IsCancelled() || Clock::now() >= context.deadline();
}
}

struct LlmRpcServer::Impl final : wire::Chat::Service
{
  explicit Impl(LlmRpcInput input)
      : input_(std::move(input)),
        gate_({.expectedCallers = {},
               .callerPairs = input_.credentials,
               .legacySecret = {},
               .onFirstLegacy = {}}),
        slots_(std::max(1, input_.slots))
  {
    if (!input_.capabilities || !input_.chat || !input_.chatStream ||
        gate_.pairedCount() == 0)
      throw std::invalid_argument("Invalid LLM RPC configuration");
    grpc::ServerBuilder builder;
    builder.SetMaxReceiveMessageSize(kMaxReceiveBytes);
    builder.AddListeningPort(input_.address, grpc::InsecureServerCredentials(),
                             &port_);
    builder.RegisterService(this);
    for (auto* service : input_.services)
      builder.RegisterService(service);
    server_ = builder.BuildAndStart();
    if (!server_)
      throw std::runtime_error("LLM RPC listener failed");
  }

  grpc::Status Capabilities(grpc::ServerContext* context,
                            const wire::CapabilitiesRequest*,
                            wire::CapabilitiesResponse* response) override
  {
    if (!gate_.admit(context, {}).admitted())
      return argus::response::toRpcStatus(
          ResponseException(401, LlmErrors::Unauthorized));
    argus::llm::Capabilities capabilities;
    try {
      capabilities = input_.capabilities();
    }
    catch (const ResponseException& error) {
      return argus::response::toRpcStatus(error);
    }
    catch (...) {
      return argus::response::toRpcStatus(
          ResponseException(500, LlmErrors::InternalError));
    }
    response->set_loaded(capabilities.loaded);
    response->set_default_max_tokens(capabilities.defaultMaxTokens);
    response->set_default_temperature(capabilities.defaultTemperature);
    response->set_context_size(capabilities.contextSize);
    response->set_last_prompt_tokens(capabilities.lastPromptTokens);
    response->set_last_reused_tokens(capabilities.lastReusedTokens);
    response->set_last_decoded_tokens(capabilities.lastDecodedTokens);
    return grpc::Status::OK;
  }

  grpc::Status Chat(grpc::ServerContext* context,
                    const wire::ChatRequest* request,
                    wire::ChatResponse* response) override
  {
    const auto admission = gate_.admit(context, {});
    if (!admission.admitted())
      return argus::response::toRpcStatus(
          ResponseException(401, LlmErrors::Unauthorized));
    if (!validRequest(*request))
      return argus::response::toRpcStatus(
          ResponseException(400, LlmErrors::InvalidRequest));
    if (deadlineTooFar(*context))
      return argus::response::toRpcStatus(
          ResponseException(400, LlmErrors::InvalidRequest));
    if (stopped(*context))
      return argus::response::toRpcStatus(stoppedError(*context));
    if (const auto refusal = acquireSlot(*context))
      return argus::response::toRpcStatus(*refusal);
    struct Release
    {
      std::counting_semaphore<>& slots;
      ~Release() { slots.release(); }
    } release{slots_};
    const ChatRequest chat = chatRequest(*request, admission.caller);
    try {
      response->set_text(input_.chat(chat).text);
    }
    catch (const ResponseException& error) {
      return argus::response::toRpcStatus(error);
    }
    catch (...) {
      return argus::response::toRpcStatus(
          ResponseException(500, LlmErrors::InternalError));
    }
    return grpc::Status::OK;
  }

  grpc::Status ChatStream(grpc::ServerContext* context,
                          const wire::ChatRequest* request,
                          grpc::ServerWriter<wire::ChatToken>* writer) override
  {
    const auto admission = gate_.admit(context, {});
    if (!admission.admitted())
      return argus::response::toRpcStatus(
          ResponseException(401, LlmErrors::Unauthorized));
    if (!validRequest(*request))
      return argus::response::toRpcStatus(
          ResponseException(400, LlmErrors::InvalidRequest));
    if (deadlineTooFar(*context))
      return argus::response::toRpcStatus(
          ResponseException(400, LlmErrors::InvalidRequest));
    if (stopped(*context))
      return argus::response::toRpcStatus(stoppedError(*context));
    if (const auto refusal = acquireSlot(*context))
      return argus::response::toRpcStatus(*refusal);
    struct Release
    {
      std::counting_semaphore<>& slots;
      ~Release() { slots.release(); }
    } release{slots_};
    const ChatRequest chat = chatRequest(*request, admission.caller);
    StreamQueue queue;
    std::jthread producer([&] {
      const TokenCallback emit = [&](const std::string& token, bool done) {
        if (done)
          return;
        std::unique_lock lock(queue.mutex);
        while (queue.tokens.size() >= kMaxQueuedTokens && !queue.stopped &&
               !stopped(*context))
          queue.ready.wait_for(lock, std::chrono::milliseconds(10));
        if (queue.stopped || stopped(*context))
          throw stoppedError(*context);
        queue.tokens.push_back({.text = token, .action = std::nullopt});
        queue.ready.notify_all();
      };
      const ActionCallback act = [&](const ClientAction& action) {
        std::scoped_lock lock(queue.mutex);
        if (queue.stopped)
          return;
        queue.tokens.push_back({.text = {}, .action = action});
        queue.ready.notify_all();
      };
      try {
        if (stopped(*context))
          throw stoppedError(*context);
        input_.chatStream(
            {.request = chat,
             .onToken = emit,
             .stats = &queue.stats,
             .cancellation = {},
             .onAction = act});
      }
      catch (const ResponseException& error) {
        std::scoped_lock lock(queue.mutex);
        queue.status = argus::response::toRpcStatus(error);
      }
      catch (...) {
        std::scoped_lock lock(queue.mutex);
        queue.status = argus::response::toRpcStatus(
            ResponseException(500, LlmErrors::InternalError));
      }
      std::scoped_lock lock(queue.mutex);
      queue.done = true;
      queue.ready.notify_all();
    });

    std::uint32_t sequence = 0;
    for (;;) {
      std::unique_lock lock(queue.mutex);
      queue.ready.wait_for(lock, std::chrono::milliseconds(10), [&] {
        return queue.done || !queue.tokens.empty();
      });
      if (stopped(*context)) {
        queue.stopped = true;
        queue.ready.notify_all();
        break;
      }
      if (queue.tokens.empty()) {
        if (queue.done)
          break;
        continue;
      }
      auto token = std::move(queue.tokens.front());
      queue.tokens.pop_front();
      queue.ready.notify_all();
      lock.unlock();
      wire::ChatToken chunk;
      chunk.set_sequence(sequence++);
      if (token.action) {
        chunk.mutable_action()->set_name(token.action->name);
        chunk.mutable_action()->set_arguments(token.action->arguments);
      }
      else {
        chunk.set_text(std::move(token.text));
      }
      if (!writer->Write(chunk)) {
        std::scoped_lock stoppedLock(queue.mutex);
        queue.stopped = true;
        queue.ready.notify_all();
        break;
      }
    }
    producer.join();
    if (queue.stopped)
      return argus::response::toRpcStatus(stoppedError(*context));
    if (!queue.status.ok())
      return queue.status;
    wire::ChatToken final;
    final.set_sequence(sequence);
    final.set_done(true);
    final.set_prompt_tokens(queue.stats.promptTokens);
    final.set_reused_tokens(queue.stats.reusedTokens);
    final.set_decoded_tokens(queue.stats.decodedTokens);
    if (!writer->Write(final))
      return argus::response::toRpcStatus(stoppedError(*context));
    return grpc::Status::OK;
  }

  std::optional<ResponseException>
  acquireSlot(const grpc::ServerContext& context)
  {
    const auto giveUpAt = Clock::now() + kMaxDeadline;
    while (!slots_.try_acquire_for(kSlotPoll)) {
      if (stopped(context))
        return stoppedError(context);
      if (Clock::now() >= giveUpAt)
        return ResponseException(429, LlmErrors::Busy);
    }
    return std::nullopt;
  }

  LlmRpcInput input_;
  argus::client::FleetCallerGate gate_;
  std::counting_semaphore<> slots_;
  int port_{0};
  std::unique_ptr<grpc::Server> server_;
};

LlmRpcServer::LlmRpcServer(LlmRpcInput input)
    : impl_(std::make_unique<Impl>(std::move(input)))
{
}

LlmRpcServer::~LlmRpcServer() { shutdown(); }
int LlmRpcServer::port() const { return impl_->port_; }

void LlmRpcServer::requestStop()
{
  if (stopper_.joinable() || !impl_->server_)
    return;
  stopper_ = std::jthread([this] {
    impl_->server_->Shutdown(Clock::now() + kShutdownGrace);
    impl_->server_->Wait();
    stopped_.store(true);
  });
}

bool LlmRpcServer::drained() const
{
  return stopped_.load() || !impl_->server_;
}

void LlmRpcServer::shutdown()
{
  if (stopper_.joinable()) {
    stopper_.join();
    impl_->server_.reset();
    return;
  }
  if (impl_->server_) {
    impl_->server_->Shutdown(Clock::now());
    impl_->server_->Wait();
    impl_->server_.reset();
  }
}
