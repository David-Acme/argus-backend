#include "auth-action-sink.hxx"

#include <auth/auth-errors.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <sqlite/db-service.hxx>
#include <sync/stream-retention.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>
#include <utility>

namespace
{
std::string orDefault(const std::string& configured, std::string_view fallback)
{
  return configured.empty() ? std::string(fallback) : configured;
}

NatsBus::StreamInput streamOf(std::string name, std::string subject)
{
  return {.name = std::move(name),
          .subjects = {std::move(subject)},
          .maxAgeNs = stream_retention::kRetentionNs,
          .duplicatesNs = stream_retention::kDuplicatesNs};
}

outbox::OutboxConfig outboxConfig(const AuthActionSink::Config& config)
{
  const std::string actionSubject =
      orDefault(config.actionSubject, nats_subject::kAuthUserAction);
  NatsBus::StreamInput actions = streamOf(
      orDefault(config.streamName, nats_subject::kAuthChangeStream),
      actionSubject);
  NatsBus::StreamInput sessions = streamOf(
      orDefault(config.sessionStreamName, nats_subject::kAuthSessionStream),
      orDefault(config.sessionSubject, nats_subject::kAuthSession));
  return {.label = "Auth action outbox",
          .client = [] { return DbService::client(); },
          .defaultSubject = actionSubject,
          .legacyIdPrefix = std::string(AuthActionSink::kActionIdPrefix),
          .refusal = AuthErrors::ChangeNotRecorded,
          .ensureStreams =
              [actions = std::move(actions),
               sessions = std::move(sessions)](NatsBus& bus) {
                if (!bus.ensureStream(actions) || !bus.ensureStream(sessions))
                  return false;
                LOG_INFO << "Auth action outbox: streams " << actions.name
                         << " and " << sessions.name << " ready";
                return true;
              },
          .timing = {.retryMs = config.retryMs,
                     .maxRetryMs = outbox::kMaxRetryMs,
                     .progressMs = outbox::kProgressMs,
                     .batch = outbox::kBatch},
          .retention = {.keepSentMs = stream_retention::kRetentionMs,
                        .purgeEveryMs = stream_retention::kSettledPurgeIntervalMs,
                        .purgeRetryMs = stream_retention::kSettledPurgeRetryMs}};
}
}

AuthActionSink::AuthActionSink(std::shared_ptr<NatsBus> bus,
                               const Config& config)
    : actionSubject_(
          orDefault(config.actionSubject, nats_subject::kAuthUserAction)),
      sessionSubject_(
          orDefault(config.sessionSubject, nats_subject::kAuthSession)),
      outbox_(std::move(bus), outboxConfig(config))
{
}

outbox::OutboxRepository AuthActionSink::repository()
{
  return outbox::OutboxRepository([] { return DbService::client(); });
}

drogon::Task<void>
AuthActionSink::publishAction(const AuthActionPublishInput& input) const
{
  co_await outbox_.append({.idPrefix = kActionIdPrefix,
                           .subject = actionSubject_,
                           .payload = json_util::toString(input.event.toJson()),
                           .client = input.client});
}

drogon::Task<void>
AuthActionSink::publishSessionChange(const AuthSessionChangeInput& input) const
{
  co_await outbox_.append({.idPrefix = kSessionIdPrefix,
                           .subject = sessionSubject_,
                           .payload = json_util::toString(input.payload),
                           .client = input.client});
}

void AuthActionSink::reconcile()
{
  outbox_.reconcile();
}

void AuthActionSink::requestStop()
{
  outbox_.requestStop();
}

bool AuthActionSink::drained() const
{
  return outbox_.drained();
}
