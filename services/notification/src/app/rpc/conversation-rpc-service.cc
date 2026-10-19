#include "conversation-rpc-service.hxx"

#include <config/config-service.hxx>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <trantor/utils/Logger.h>

#include <exception>
#include <string>
#include <utility>

namespace
{
namespace v1 = argus::notification::v1;

std::optional<MessageKind> kindOf(v1::MessageKind kind)
{
  switch (kind) {
    case v1::MESSAGE_KIND_RELAY:
      return MessageKind::Relay;
    case v1::MESSAGE_KIND_REPLY:
      return MessageKind::Reply;
    case v1::MESSAGE_KIND_VISITOR:
      return MessageKind::Visitor;
    default:
      return std::nullopt;
  }
}

std::optional<MessageDirection> directionOf(v1::TurnSpeaker speaker)
{
  switch (speaker) {
    case v1::TURN_SPEAKER_USER:
      return MessageDirection::Out;
    case v1::TURN_SPEAKER_ASSISTANT:
      return MessageDirection::In;
    default:
      return std::nullopt;
  }
}

grpc::ServerUnaryReactor* finished(grpc::CallbackServerContext* context, const grpc::Status& status)
{
  auto* reactor = context->DefaultReactor();
  reactor->Finish(status);
  return reactor;
}

grpc::Status statusOf(const ResponseException& error)
{
  if (error.statusCode() >= 400 && error.statusCode() < 500)
    return {grpc::StatusCode::INVALID_ARGUMENT, error.what()};
  return {grpc::StatusCode::INTERNAL, error.what()};
}

template <typename Work>
void runOnLoop(grpc::ServerUnaryReactor* reactor, Work work)
{
  drogon::app().getLoop()->queueInLoop([reactor, work = std::move(work)]() mutable {
    drogon::async_run([reactor, work = std::move(work)]() mutable -> drogon::Task<void> {
      try {
        co_await work();
        reactor->Finish(grpc::Status::OK);
      }
      catch (const ResponseException& error) {
        reactor->Finish(statusOf(error));
      }
      catch (const std::exception& error) {
        LOG_WARN << "Conversation RPC failed: " << error.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, error.what()));
      }
      co_return;
    });
  });
}
}

std::optional<RelayLetter> relayLetterOf(const v1::RelayMessageRequest& request)
{
  const auto kind = kindOf(request.kind());
  if (!kind)
    return std::nullopt;
  RelayLetter letter{.fromUserId = request.from_user_id(),
                     .fromName = request.from_name(),
                     .toUserId = request.to_user_id(),
                     .toName = request.to_name(),
                     .kind = *kind,
                     .body = request.body(),
                     .spokenLine = request.spoken_line(),
                     .title = request.title(),
                     .replyToKey = request.reply_to_key().empty() ? std::nullopt
                                                                  : std::optional<std::string>(request.reply_to_key()),
                     .commandId = request.command_id(),
                     .expiresAt = request.expires_at()};
  if (!validLetter(letter))
    return std::nullopt;
  return letter;
}

std::optional<SettleLetter> settleLetterOf(const v1::SettleMessageRequest& request)
{
  if (request.message_key().empty() || request.message_key().size() > ConversationInboxService::kMaxKeyBytes)
    return std::nullopt;
  switch (request.settlement()) {
    case v1::MESSAGE_SETTLEMENT_ANSWERED:
      return SettleLetter{.messageKey = request.message_key(), .state = MessageState::Answered};
    case v1::MESSAGE_SETTLEMENT_EXPIRED:
      return SettleLetter{.messageKey = request.message_key(), .state = MessageState::Expired};
    default:
      return std::nullopt;
  }
}

std::optional<TranscriptInput> transcriptOf(const v1::AppendTurnsRequest& request)
{
  std::optional<ConversationLegKind> leg;
  if (!request.leg().empty()) {
    leg = conversationLegKindFromString(request.leg());
    if (!leg)
      return std::nullopt;
  }
  TranscriptInput input{.userId = request.user_id(),
                        .leg = leg,
                        .lines = {},
                        .expiresAt = request.expires_at(),
                        .commandId = request.command_id()};
  input.lines.reserve(static_cast<std::size_t>(request.turns_size()));
  for (const auto& turn : request.turns()) {
    const auto direction = directionOf(turn.speaker());
    if (!direction)
      return std::nullopt;
    input.lines.push_back({.direction = *direction, .body = turn.body()});
  }
  if (!validTranscript(input))
    return std::nullopt;
  return input;
}

ConversationRpcService::ConversationRpcService(std::shared_ptr<const ConversationInboxService> inbox)
    : assistantCallers_({argus::client::CallerCredential{.service = "argus-llm",
                                                          .secret = ConfigService::getString("grpc.caller_llm")}}),
      inbox_(std::move(inbox))
{
}

bool ConversationRpcService::admitted(grpc::CallbackServerContext* context) const
{
  return argus::client::authorizeCaller(context, assistantCallers_).has_value();
}

grpc::ServerUnaryReactor* ConversationRpcService::RelayMessage(grpc::CallbackServerContext* context,
                                                               const v1::RelayMessageRequest* request,
                                                               v1::RelayMessageResponse* response)
{
  if (!admitted(context))
    return finished(context, {grpc::StatusCode::UNAUTHENTICATED, "argus-llm caller credential required"});
  auto letter = relayLetterOf(*request);
  if (!letter)
    return finished(context, {grpc::StatusCode::INVALID_ARGUMENT, "the message is not valid"});
  auto* reactor = context->DefaultReactor();
  runOnLoop(reactor, [inbox = inbox_, letter = std::move(*letter), response]() -> drogon::Task<void> {
    const RelayReceipt receipt = co_await inbox->relay(letter);
    response->set_message_key(receipt.messageKey);
    response->set_channel(receipt.route == RelayRoute::Conversation ? v1::MESSAGE_CHANNEL_CONVERSATION
                                                                    : v1::MESSAGE_CHANNEL_NOTIFICATION);
    response->set_duplicate(receipt.duplicate);
  });
  return reactor;
}

grpc::ServerUnaryReactor* ConversationRpcService::SettleMessage(grpc::CallbackServerContext* context,
                                                                const v1::SettleMessageRequest* request,
                                                                v1::SettleMessageResponse* response)
{
  if (!admitted(context))
    return finished(context, {grpc::StatusCode::UNAUTHENTICATED, "argus-llm caller credential required"});
  auto letter = settleLetterOf(*request);
  if (!letter)
    return finished(context, {grpc::StatusCode::INVALID_ARGUMENT, "the settlement is not valid"});
  auto* reactor = context->DefaultReactor();
  runOnLoop(reactor, [inbox = inbox_, letter = std::move(*letter), response]() -> drogon::Task<void> {
    response->set_updated(static_cast<int32_t>(co_await inbox->settle(letter)));
  });
  return reactor;
}

grpc::ServerUnaryReactor* ConversationRpcService::AppendTurns(grpc::CallbackServerContext* context,
                                                              const v1::AppendTurnsRequest* request,
                                                              v1::AppendTurnsResponse* response)
{
  if (!admitted(context))
    return finished(context, {grpc::StatusCode::UNAUTHENTICATED, "argus-llm caller credential required"});
  auto input = transcriptOf(*request);
  if (!input)
    return finished(context, {grpc::StatusCode::INVALID_ARGUMENT, "the turns are not valid"});
  auto* reactor = context->DefaultReactor();
  runOnLoop(reactor, [inbox = inbox_, input = std::move(*input), response]() -> drogon::Task<void> {
    const TranscriptOutcome outcome = co_await inbox->appendTranscript(input);
    response->set_appended(static_cast<int32_t>(outcome.appended));
    response->set_duplicate(outcome.duplicate);
  });
  return reactor;
}
