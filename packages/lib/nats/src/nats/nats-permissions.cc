#include "nats-permissions.hxx"

#include "nats-subject.hxx"

#include <algorithm>
#include <cctype>
#include <initializer_list>

namespace nats_permissions
{
namespace
{
using namespace nats_subject;

constexpr std::string_view kModuleFeed = kSettingsModuleStream;
constexpr std::string_view kGuardDeliveryAdvisory =
    "$JS.EVENT.ADVISORY.CONSUMER.MAX_DELIVERIES.ARGUS_CAMERA.argus-guard";

Consumer modulesOf(std::string_view name)
{
  return {.stream = kModuleFeed,
          .name = name,
          .subjects = {kSettingsModule},
          .kind = ConsumerKind::Push,
          .recreates = true};
}

std::vector<ServicePermissions> rows()
{
  return {
      {.service = "auth",
       .publishes = {kAuthPresenceSignal},
       .stores = {kAuthUserAction, kAuthSession},
       .subscribes = {},
       .streams = {{.stream = kAuthChangeStream, .use = StreamUse::Ensure},
                   {.stream = kAuthSessionStream, .use = StreamUse::Ensure}},
       .consumers = {{.stream = kIdentityChangeStream,
                      .name = "argus-auth-identity",
                      .subjects = {kIdentityChange},
                      .kind = ConsumerKind::Push,
                      .recreates = false}}},
      {.service = "camera",
       .publishes = {kCameraHealth},
       .stores = {kCameraChange, kCameraObjectDetected},
       .subscribes = {},
       .streams = {{.stream = kCameraStream, .use = StreamUse::Ensure}},
       .consumers = {{.stream = kAuthSessionStream,
                      .name = "argus-camera-auth-session",
                      .subjects = {kAuthSession},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     {.stream = kIdentityChangeStream,
                      .name = "argus-camera-identity-user",
                      .subjects = {kIdentityChange},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     modulesOf("argus-camera-modules")}},
      {.service = "guard",
       .publishes = {kGuardHeartbeat, kGuardKnownSeen, kGuardPresenceChanged},
       .stores = {kGuardChange, kGuardEncounterClosed},
       .subscribes = {kAuthPresenceSignal, kCameraHealth, kGuardKnownSeen,
                      kIdentityChange, kGuardDeliveryAdvisory},
       .streams = {{.stream = kGuardStream, .use = StreamUse::Ensure},
                   {.stream = kNotificationVerdictStream,
                    .use = StreamUse::Ensure}},
       .consumers = {{.stream = kCameraStream,
                      .name = "argus-guard",
                      .subjects = {kCameraObjectDetected},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     {.stream = kNotificationVerdictStream,
                      .name = "argus-guard-verdicts",
                      .subjects = {kNotificationResponseVerdict},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     modulesOf("argus-guard-modules")}},
      {.service = "identity",
       .publishes = {},
       .stores = {kIdentityChange, kIdentityUserAction},
       .subscribes = {},
       .streams = {{.stream = kIdentityChangeStream, .use = StreamUse::Ensure}},
       .consumers = {modulesOf("argus-identity-modules")}},
      {.service = "llm",
       .publishes = {},
       .stores = {},
       .subscribes = {},
       .streams = {},
       .consumers = {{.stream = kGuardStream,
                      .name = "argus-llm-encounters",
                      .subjects = {kGuardEncounterClosed},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     {.stream = kCameraStream,
                      .name = "argus-llm-catalog-camera",
                      .subjects = {kCameraChange},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     {.stream = kIdentityChangeStream,
                      .name = "argus-llm-catalog-identity",
                      .subjects = {kIdentityChange},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     {.stream = kSettingsModuleStream,
                      .name = "argus-llm-intents",
                      .subjects = {kSettingsModule},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     modulesOf("argus-llm-modules")}},
      {.service = "notification",
       .publishes = {kNotificationPushIntent},
       .stores = {kNotificationChange, kNotificationDelivery,
                  kNotificationResponseVerdict},
       .subscribes = {kCameraObjectDetected, kGuardHeartbeat},
       .streams =
           {{.stream = kNotificationChangeStream, .use = StreamUse::Ensure},
            {.stream = kNotificationDeliveryStream, .use = StreamUse::Ensure},
            {.stream = kNotificationVerdictStream, .use = StreamUse::Ensure}},
       .consumers = {{.stream = kGuardStream,
                      .name = "notification-known-seen",
                      .subjects = {kGuardKnownSeen},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     {.stream = kAuthSessionStream,
                      .name = "notification-auth-session",
                      .subjects = {kAuthSession},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     {.stream = kIdentityChangeStream,
                      .name = "notification-identity-user",
                      .subjects = {kIdentityChange},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     modulesOf("argus-notification-modules")}},
      {.service = "productivity",
       .publishes = {},
       .stores = {kProductivityChange, kProductivityUserAction},
       .subscribes = {},
       .streams = {{.stream = kProductivityChangeStream,
                    .use = StreamUse::Ensure},
                   {.stream = kProductivityActionStream,
                    .use = StreamUse::Ensure}},
       .consumers = {modulesOf("argus-productivity-modules")}},
      {.service = "recorder",
       .publishes = {"argus.recorder.v1.>"},
       .stores = {},
       .subscribes = {kCameraChange, kSettingsModule},
       .streams = {{.stream = kGuardStream, .use = StreamUse::Inspect},
                   {.stream = "ARGUS_RECORDER_CHANGE",
                    .use = StreamUse::Create}},
       .consumers = {{.stream = kGuardStream,
                      .name = "argus-recorder-guard",
                      .subjects = {"argus.guard.v1.episode",
                                   "argus.guard.v1.safety_alert"},
                      .kind = ConsumerKind::Pull,
                      .recreates = false},
                     {.stream = kSettingsModuleStream,
                      .name = "argus-recorder-modules",
                      .subjects = {kSettingsModule},
                      .kind = ConsumerKind::Pull,
                      .recreates = false}}},
      {.service = "relay",
       .publishes = {},
       .stores = {},
       .subscribes = {kNotificationPushIntent},
       .streams = {},
       .consumers = {}},
      {.service = "settings",
       .publishes = {},
       .stores = {kSettingsModule, kSettingsUserAction, kSettingsChange},
       .subscribes = {},
       .streams = {{.stream = kSettingsModuleStream, .use = StreamUse::Ensure},
                   {.stream = kSettingsActionStream, .use = StreamUse::Ensure},
                   {.stream = kSettingsChangeStream, .use = StreamUse::Ensure}},
       .consumers = {}},
      {.service = "sync",
       .publishes = {kNotificationPushIntent},
       .stores = {},
       .subscribes = {kGuardHeartbeat, kGuardPresenceChanged},
       .streams = {},
       .consumers = {{.stream = kCameraStream,
                      .name = "argus-sync-camera",
                      .subjects = {kCameraChange},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     {.stream = kNotificationChangeStream,
                      .name = "argus-sync-notification",
                      .subjects = {kNotificationChange},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     {.stream = kProductivityChangeStream,
                      .name = "argus-sync-productivity",
                      .subjects = {kProductivityChange},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     {.stream = kProductivityActionStream,
                      .name = "argus-sync-productivity-action",
                      .subjects = {kProductivityUserAction},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     {.stream = kIdentityChangeStream,
                      .name = "argus-sync-identity",
                      .subjects = {kIdentityChange},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     {.stream = kIdentityChangeStream,
                      .name = "argus-sync-identity-action",
                      .subjects = {kIdentityUserAction},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     {.stream = kAuthChangeStream,
                      .name = "argus-sync-auth-action",
                      .subjects = {kAuthUserAction},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     {.stream = kAuthSessionStream,
                      .name = "argus-sync-auth-session",
                      .subjects = {kAuthSession},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     {.stream = kSettingsModuleStream,
                      .name = "argus-sync-settings-module",
                      .subjects = {kSettingsModule},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     {.stream = kSettingsActionStream,
                      .name = "argus-sync-settings-action",
                      .subjects = {kSettingsUserAction},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     {.stream = kSettingsChangeStream,
                      .name = "argus-sync-settings",
                      .subjects = {kSettingsChange},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     {.stream = kGuardStream,
                      .name = "argus-sync-guard",
                      .subjects = {kGuardChange},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     {.stream = kNotificationDeliveryStream,
                      .name = "argus-sync-delivery",
                      .subjects = {kNotificationDelivery},
                      .kind = ConsumerKind::Push,
                      .recreates = false},
                     modulesOf("argus-sync-modules")}},
  };
}

std::string dotted(std::initializer_list<std::string_view> tokens)
{
  std::string subject;
  for (const std::string_view token : tokens) {
    if (!subject.empty())
      subject += '.';
    subject += token;
  }
  return subject;
}

void addStreamGrants(const StreamGrant& grant,
                     std::vector<std::string>& publish)
{
  publish.push_back(dotted({"$JS.API.STREAM.INFO", grant.stream}));
  if (grant.use == StreamUse::Inspect)
    return;
  publish.push_back(dotted({"$JS.API.STREAM.CREATE", grant.stream}));
  if (grant.use == StreamUse::Ensure)
    publish.push_back(dotted({"$JS.API.STREAM.UPDATE", grant.stream}));
}

void addConsumerGrants(const Consumer& consumer,
                       std::vector<std::string>& publish)
{
  publish.push_back(
      dotted({"$JS.API.CONSUMER.INFO", consumer.stream, consumer.name}));
  publish.push_back(dotted({"$JS.ACK", consumer.stream, consumer.name, ">"}));
  if (consumer.kind == ConsumerKind::Push) {
    publish.push_back(dotted(
        {"$JS.API.CONSUMER.DURABLE.CREATE", consumer.stream, consumer.name}));
  }
  else {
    publish.push_back(
        dotted({"$JS.API.CONSUMER.CREATE", consumer.stream, consumer.name}));
    publish.push_back(dotted(
        {"$JS.API.CONSUMER.CREATE", consumer.stream, consumer.name, ">"}));
    publish.push_back(
        dotted({"$JS.API.CONSUMER.MSG.NEXT", consumer.stream, consumer.name}));
  }
  if (consumer.recreates)
    publish.push_back(
        dotted({"$JS.API.CONSUMER.DELETE", consumer.stream, consumer.name}));
}

void sortUnique(std::vector<std::string>& subjects)
{
  std::ranges::sort(subjects);
  const auto [first, last] = std::ranges::unique(subjects);
  subjects.erase(first, last);
}

struct Direction
{
  std::string_view name;
  const std::vector<std::string>& subjects;
};

std::string renderDirection(const Direction& direction)
{
  std::string out = "  ";
  out += direction.name;
  out += ": {\n";
  out += direction.subjects.empty() ? "    deny: [\n" : "    allow: [\n";
  if (direction.subjects.empty())
    out += "      \">\"\n";
  for (const std::string& subject : direction.subjects) {
    out += "      \"";
    out += subject;
    out += "\"\n";
  }
  out += "    ]\n  }\n";
  return out;
}
}

std::string_view streamUseToString(StreamUse use)
{
  switch (use) {
    case StreamUse::Inspect:
      return "inspect";
    case StreamUse::Create:
      return "create";
    case StreamUse::Ensure:
      return "ensure";
  }
  return "ensure";
}

std::optional<StreamUse> streamUseFromString(std::string_view text)
{
  for (const StreamUse use :
       {StreamUse::Inspect, StreamUse::Create, StreamUse::Ensure}) {
    if (streamUseToString(use) == text)
      return use;
  }
  return std::nullopt;
}

std::string_view consumerKindToString(ConsumerKind kind)
{
  return kind == ConsumerKind::Pull ? "pull" : "push";
}

std::optional<ConsumerKind> consumerKindFromString(std::string_view text)
{
  for (const ConsumerKind kind : {ConsumerKind::Push, ConsumerKind::Pull}) {
    if (consumerKindToString(kind) == text)
      return kind;
  }
  return std::nullopt;
}

const std::vector<ServicePermissions>& table()
{
  static const std::vector<ServicePermissions> permissions = rows();
  return permissions;
}

std::string inboxPrefix(std::string_view service)
{
  return dotted({kInboxRoot, service});
}

std::string variableName(std::string_view service)
{
  std::string name(kVariablePrefix);
  for (const char c : service)
    name +=
        c == '-'
            ? '_'
            : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return name;
}

bool needsInbox(const ServicePermissions& row)
{
  return !row.stores.empty() || !row.streams.empty() || !row.consumers.empty();
}

Grants grantsFor(const ServicePermissions& row)
{
  Grants grants;
  grants.publish.assign(row.publishes.begin(), row.publishes.end());
  grants.publish.insert(grants.publish.end(), row.stores.begin(),
                        row.stores.end());
  for (const StreamGrant& grant : row.streams)
    addStreamGrants(grant, grants.publish);
  for (const Consumer& consumer : row.consumers)
    addConsumerGrants(consumer, grants.publish);
  grants.subscribe.assign(row.subscribes.begin(), row.subscribes.end());
  if (needsInbox(row))
    grants.subscribe.push_back(dotted({inboxPrefix(row.service), ">"}));
  sortUnique(grants.publish);
  sortUnique(grants.subscribe);
  return grants;
}

std::string render()
{
  std::string out;
  for (const ServicePermissions& row : table()) {
    if (!out.empty())
      out += '\n';
    const Grants grants = grantsFor(row);
    out += variableName(row.service);
    out += ": {\n";
    out += renderDirection({.name = "publish", .subjects = grants.publish});
    out += renderDirection({.name = "subscribe", .subjects = grants.subscribe});
    out += "}\n";
  }
  return out;
}

}
