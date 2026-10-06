#include "llm-client.hxx"

#include <errors/response-exception.hxx>
#include <grpc/grpc-client-base.hxx>
#include <llm.grpc.pb.h>
#include <llm/llm-errors.hxx>
#include <response/response-rpc.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <stop_token>
#include <utility>
#include <vector>

namespace argus::llm
{
namespace
{
namespace wire = argus::llm::v1;
using Clock = std::chrono::system_clock;

ResponseException errorFor(const grpc::Status& status,
                           Clock::time_point deadlineAt)
{
  if (status.error_code() != grpc::StatusCode::CANCELLED)
    return argus::response::fromRpcStatus(status);
  if (Clock::now() >= deadlineAt)
    return {504, LlmErrors::DeadlineExceeded};
  return {499, LlmErrors::Cancelled};
}

void check(const grpc::Status& status, Clock::time_point deadlineAt)
{
  if (!status.ok())
    throw errorFor(status, deadlineAt);
}

constexpr std::size_t kMaxMessages = 64;
constexpr std::size_t kMaxRoleBytes = 32;
constexpr std::size_t kMaxContentBytes = std::size_t{32} * 1024;
constexpr std::size_t kMaxGrammarBytes = std::size_t{8} * 1024;
constexpr std::size_t kMaxSessionBytes = 128;
constexpr int32_t kMaxTokensBound = 4096;
constexpr int64_t kMaxContextSize = 1 << 22;
constexpr int32_t kMaxPrefillTokens = 1 << 20;

bool validMessages(const std::vector<ChatMessage>& messages)
{
  if (messages.empty() || messages.size() > kMaxMessages)
    return false;
  return std::ranges::all_of(messages, [](const ChatMessage& message) {
    return !message.role.empty() && message.role.size() <= kMaxRoleBytes &&
           !message.content.empty() &&
           message.content.size() <= kMaxContentBytes;
  });
}

bool validLang(const std::string& lang)
{
  return lang.empty() || lang == "es" || lang == "en";
}

bool validRequest(const ChatRequest& request)
{
  return validMessages(request.messages) && request.maxTokens >= 0 &&
         request.maxTokens <= kMaxTokensBound && request.temperature >= -1.0F &&
         request.temperature <= 2.0F &&
         request.grammar.size() <= kMaxGrammarBytes && request.userId >= 0 &&
         validLang(request.lang) && request.sessionId.size() <= kMaxSessionBytes;
}

wire::CallerRole wireRole(UserRole role)
{
  switch (role) {
    case UserRole::Owner:
      return wire::CALLER_ROLE_OWNER;
    case UserRole::Resident:
      return wire::CALLER_ROLE_RESIDENT;
    case UserRole::Guard:
      return wire::CALLER_ROLE_GUARD;
    case UserRole::Guest:
      return wire::CALLER_ROLE_GUEST;
    case UserRole::Unknown:
      break;
  }
  return wire::CALLER_ROLE_UNSPECIFIED;
}

bool validCapabilities(const wire::CapabilitiesResponse& response)
{
  return response.default_max_tokens() >= 0 &&
         response.default_max_tokens() <= kMaxTokensBound &&
         std::isfinite(response.default_temperature()) &&
         response.default_temperature() >= 0.0F &&
         response.default_temperature() <= 2.0F &&
         response.context_size() > 0 &&
         response.context_size() <= kMaxContextSize &&
         response.last_prompt_tokens() >= 0 &&
         response.last_prompt_tokens() <= kMaxPrefillTokens &&
         response.last_reused_tokens() >= 0 &&
         response.last_reused_tokens() <= kMaxPrefillTokens &&
         response.last_decoded_tokens() >= 0 &&
         response.last_decoded_tokens() <= kMaxPrefillTokens;
}

wire::ChatRequest wireRequest(const ChatRequest& request)
{
  wire::ChatRequest wire;
  for (const auto& message : request.messages) {
    auto* entry = wire.add_messages();
    entry->set_role(message.role);
    entry->set_content(message.content);
  }
  wire.set_max_tokens(request.maxTokens);
  wire.set_temperature(request.temperature);
  wire.set_reset_context(request.resetContext);
  wire.set_tools(request.toolsEnabled);
  wire.set_user_id(request.userId);
  wire.set_grammar(request.grammar);
  wire.set_grammar_required(request.grammarRequired);
  wire.set_caller_role(wireRole(request.role));
  wire.set_lang(request.lang);
  wire.set_client_actions(request.clientActions);
  wire.set_session_id(request.sessionId);
  wire.set_prefill_only(request.prefillOnly);
  return wire;
}
}

struct Client::Impl
{
  ClientConfig config;
  std::unique_ptr<wire::Chat::Stub> stub;
};

Client::Client(ClientConfig config)
{
  if (config.target.empty() || config.credential.empty())
    throw ResponseException(400, LlmErrors::InvalidRequest);
  if (config.timeout.count() <= 0 || config.timeout > kMaxTimeout)
    throw ResponseException(
        400, LlmErrors::InvalidRequest.withMessage(
                 "timeout must be within 1 and 120000 ms"));
  auto stub = wire::Chat::NewStub(argus::client::makeChannel(config.target));
  impl_ = std::make_unique<Impl>(
      Impl{.config = std::move(config), .stub = std::move(stub)});
}

Client::~Client() = default;

Capabilities Client::capabilities() const
{
  grpc::ClientContext context;
  const auto deadlineAt = Clock::now() + impl_->config.timeout;
  context.set_deadline(deadlineAt);
  argus::client::addCallerCredential(context, impl_->config.credential);
  wire::CapabilitiesRequest request;
  wire::CapabilitiesResponse response;
  check(impl_->stub->Capabilities(&context, request, &response), deadlineAt);
  if (!validCapabilities(response))
    throw ResponseException(502, LlmErrors::InvalidResponse);
  return {.loaded = response.loaded(),
          .defaultMaxTokens = response.default_max_tokens(),
          .defaultTemperature = response.default_temperature(),
          .contextSize = response.context_size(),
          .lastPromptTokens = response.last_prompt_tokens(),
          .lastReusedTokens = response.last_reused_tokens(),
          .lastDecodedTokens = response.last_decoded_tokens()};
}

std::string Client::chat(const ChatRequest& request) const
{
  if (!validRequest(request))
    throw ResponseException(400, LlmErrors::InvalidRequest);
  grpc::ClientContext context;
  const auto deadlineAt = Clock::now() + impl_->config.timeout;
  context.set_deadline(deadlineAt);
  argus::client::addCallerCredential(context, impl_->config.credential);
  const wire::ChatRequest wire = wireRequest(request);
  wire::ChatResponse response;
  check(impl_->stub->Chat(&context, wire, &response), deadlineAt);
  return response.text();
}

void Client::chatStream(const LlmStreamInput& input) const
{
  if (!validRequest(input.request))
    throw ResponseException(400, LlmErrors::InvalidRequest);
  if (input.cancellation.stop_requested())
    throw ResponseException(499, LlmErrors::Cancelled);
  grpc::ClientContext context;
  const auto deadlineAt = Clock::now() + impl_->config.timeout;
  context.set_deadline(deadlineAt);
  argus::client::addCallerCredential(context, impl_->config.credential);
  const wire::ChatRequest wire = wireRequest(input.request);
  const std::stop_callback cancel(input.cancellation,
                                  [&context] { context.TryCancel(); });
  const auto reader = impl_->stub->ChatStream(&context, wire);

  wire::ChatToken token;
  bool sentinel = false;
  while (!input.cancellation.stop_requested() && reader->Read(&token)) {
    if (!token.done()) {
      if (token.has_action()) {
        if (input.onAction)
          input.onAction({.name = token.action().name(), .arguments = token.action().arguments()});
        continue;
      }
      input.onToken(token.text(), false);
      continue;
    }
    if (input.stats) {
      input.stats->promptTokens = token.prompt_tokens();
      input.stats->reusedTokens = token.reused_tokens();
      input.stats->decodedTokens = token.decoded_tokens();
    }
    input.onToken("", true);
    sentinel = true;
  }
  if (input.cancellation.stop_requested()) {
    context.TryCancel();
    static_cast<void>(reader->Finish());
    if (sentinel)
      return;
    throw ResponseException(499, LlmErrors::Cancelled);
  }
  check(reader->Finish(), deadlineAt);
  if (!sentinel)
    throw ResponseException(502, LlmErrors::InvalidResponse);
}
}
