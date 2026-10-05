#include "token-revocation.hxx"

#include <drogon/drogon.h>
#include <nats/nats-subject.hxx>
#include <sync/sync-change.hxx>
#include <sync/table-name.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <functional>
#include <utility>
#include <vector>

namespace
{
constexpr double kSubscribeRetryS = 5.0;
constexpr int kMaxDeliver = 10;
constexpr const char* kIsActiveField = "isActive";

int64_t integerField(const Json::Value& change, const char* field)
{
  const Json::Value& value = change[field];
  return value.isIntegral() ? value.asInt64() : 0;
}

void settle(const std::function<void()>& action)
{
  if (action)
    action();
}

using Parser = std::optional<TokenRevocation> (*)(const Json::Value&);

struct FeedSpec
{
  const char* stream;
  const char* subject;
  const char* durable;
  Parser parse;
};

void handle(const token_revocation::FeedInput& input, const FeedSpec& spec,
            const NatsBus::DurableMessage& message,
            NatsBus::DurableSettlement settlement)
{
  const auto revocation =
      spec.parse(json_util::fromString(std::string(message.payload)));
  if (!revocation) {
    settle(settlement.ack);
    return;
  }
  drogon::app().getLoop()->queueInLoop(
      [service = input.service, tasks = input.tasks, revocation = *revocation,
       settlement = std::move(settlement)]() {
        drogon::async_run([service, tasks, revocation,
                           settlement]() -> drogon::Task<void> {
          const auto ticket = TaskGate::enter(tasks);
          if (!ticket) {
            settle(settlement.nak);
            co_return;
          }
          bool handled = true;
          try {
            const int64_t removed = co_await service->revoke(revocation);
            if (removed > 0)
              LOG_INFO << "Push tokens: removed " << removed << " of user "
                       << revocation.userId
                       << (revocation.sessionId.empty() ? " (account)"
                                                        : " (session)");
          }
          catch (const std::exception& error) {
            handled = false;
            LOG_WARN << "Push tokens: revocation for user "
                     << revocation.userId << " failed: " << error.what();
          }
          settle(handled ? settlement.ack : settlement.nak);
        });
      });
}

bool trySubscribe(const token_revocation::FeedInput& input, const FeedSpec& spec)
{
  return input.bus
      ->subscribeDurable(
          {.stream = spec.stream,
           .durable = spec.durable,
           .subject = spec.subject,
           .deliverAll = false,
           .maxDeliver = kMaxDeliver,
           .maxAckPending = NatsBus::kOrderedMaxAckPending,
           .handler =
               [input, spec](const NatsBus::DurableMessage& message,
                             NatsBus::DurableSettlement settlement) {
                 handle(input, spec, message, std::move(settlement));
               }})
      .has_value();
}

void subscribeWithRetry(const token_revocation::FeedInput& input,
                        const FeedSpec& spec)
{
  if (trySubscribe(input, spec)) {
    LOG_INFO << "Push tokens: durable " << spec.durable << " on " << spec.subject;
    return;
  }
  LOG_WARN << "Push tokens: " << spec.subject << " durable unavailable; retrying every "
           << kSubscribeRetryS << " s";
  auto timer = std::make_shared<std::optional<trantor::TimerId>>();
  *timer = drogon::app().getLoop()->runEvery(
      kSubscribeRetryS, [input, spec, timer]() {
        if (!timer->has_value() || (input.tasks && input.tasks->stopping()) ||
            !trySubscribe(input, spec))
          return;
        drogon::app().getLoop()->invalidateTimer(**timer);
        timer->reset();
        LOG_INFO << "Push tokens: durable " << spec.durable << " on "
                 << spec.subject;
      });
}
}

std::optional<TokenRevocation>
token_revocation::fromSessionChange(const Json::Value& change)
{
  if (!change.isObject() ||
      change.get(sync_change::kActionField, "").asString() !=
          sync_change::kActionDisconnectSession)
    return std::nullopt;
  const int64_t userId = integerField(change, sync_change::kUserField);
  const Json::Value& session = change[sync_change::kSessionField];
  if (userId <= 0 || !session.isString() || session.asString().empty())
    return std::nullopt;
  return TokenRevocation{.userId = userId, .sessionId = session.asString()};
}

std::optional<TokenRevocation>
token_revocation::fromIdentityChange(const Json::Value& change)
{
  if (!change.isObject() ||
      change.get(sync_change::kKindField, "").asString() !=
          sync_change::kKindIdentity ||
      change.get(sync_change::kTableField, "").asString() !=
          tableNameToString(TableName::User))
    return std::nullopt;
  const int64_t userId = integerField(change, sync_change::kRecordIdField);
  if (userId <= 0)
    return std::nullopt;
  const Json::Value& row = change[sync_change::kRowField];
  const bool deleted = change.get(sync_change::kDeletedField, false).asBool();
  const bool disabled =
      row.isObject() && !row.get(kIsActiveField, true).asBool();
  if (!deleted && !disabled)
    return std::nullopt;
  return TokenRevocation{.userId = userId, .sessionId = ""};
}

void token_revocation::subscribe(const FeedInput& input)
{
  if (!input.bus || !input.service)
    return;
  subscribeWithRetry(input, {.stream = nats_subject::kAuthSessionStream,
                             .subject = nats_subject::kAuthSession,
                             .durable = kSessionDurable,
                             .parse = &fromSessionChange});
  subscribeWithRetry(input, {.stream = nats_subject::kIdentityChangeStream,
                             .subject = nats_subject::kIdentityChange,
                             .durable = kIdentityDurable,
                             .parse = &fromIdentityChange});
}

drogon::Task<int64_t>
TokenRevocationService::revoke(const TokenRevocation& revocation) const
{
  if (revocation.userId <= 0)
    co_return 0;
  if (revocation.sessionId.empty())
    co_return co_await repository_.removeForUser(revocation.userId);
  co_return co_await repository_.removeForSession(
      {.userId = revocation.userId, .sessionId = revocation.sessionId});
}
