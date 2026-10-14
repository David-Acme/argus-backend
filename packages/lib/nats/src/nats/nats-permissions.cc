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
          .subject = kSettingsModule,
          .recreates = true};
}

std::vector<ServicePermissions> rows()
{
  return {
      {.service = "auth",
       .publishes = {kAuthPresenceSignal},
       .stores = {kAuthUserAction, kAuthSession},
       .subscribes = {},
       .streams = {kAuthChangeStream, kAuthSessionStream},
       .consumers = {{.stream = kIdentityChangeStream,
                      .name = "argus-auth-identity",
                      .subject = kIdentityChange,
                      .recreates = false}}},
      {.service = "camera",
       .publishes = {kCameraHealth},
       .stores = {kCameraChange, kCameraObjectDetected},
       .subscribes = {},
       .streams = {kCameraStream},
       .consumers = {{.stream = kAuthSessionStream,
                      .name = "argus-camera-auth-session",
                      .subject = kAuthSession,
                      .recreates = false},
                     {.stream = kIdentityChangeStream,
                      .name = "argus-camera-identity-user",
                      .subject = kIdentityChange,
                      .recreates = false},
                     modulesOf("argus-camera-modules")}},
      {.service = "guard",
       .publishes = {kGuardHeartbeat, kGuardKnownSeen, kGuardPresenceChanged},
       .stores = {kGuardChange, kGuardEncounterClosed},
       .subscribes = {kAuthPresenceSignal, kCameraHealth, kGuardKnownSeen,
                      kIdentityChange, kGuardDeliveryAdvisory},
       .streams = {kGuardStream, kNotificationVerdictStream},
       .consumers = {{.stream = kCameraStream,
                      .name = "argus-guard",
                      .subject = kCameraObjectDetected,
                      .recreates = false},
                     {.stream = kNotificationVerdictStream,
                      .name = "argus-guard-verdicts",
                      .subject = kNotificationResponseVerdict,
                      .recreates = false},
                     modulesOf("argus-guard-modules")}},
      {.service = "identity",
       .publishes = {},
       .stores = {kIdentityChange, kIdentityUserAction},
       .subscribes = {},
       .streams = {kIdentityChangeStream},
       .consumers = {modulesOf("argus-identity-modules")}},
      {.service = "llm",
       .publishes = {},
       .stores = {},
       .subscribes = {kGuardChange},
       .streams = {},
       .consumers = {{.stream = kGuardStream,
                      .name = "argus-llm-encounters",
                      .subject = kGuardEncounterClosed,
                      .recreates = false},
                     {.stream = kCameraStream,
                      .name = "argus-llm-catalog-camera",
                      .subject = kCameraChange,
                      .recreates = false},
                     {.stream = kIdentityChangeStream,
                      .name = "argus-llm-catalog-identity",
                      .subject = kIdentityChange,
                      .recreates = false},
                     {.stream = kSettingsModuleStream,
                      .name = "argus-llm-intents",
                      .subject = kSettingsModule,
                      .recreates = false},
                     modulesOf("argus-llm-modules")}},
      {.service = "notification",
       .publishes = {kNotificationPushIntent, kNotificationPushDelivery},
       .stores = {kNotificationChange, kNotificationDelivery,
                  kNotificationResponseVerdict},
       .subscribes = {kCameraObjectDetected, kGuardHeartbeat,
                      kNotificationPushResult},
       .streams = {kNotificationChangeStream, kNotificationDeliveryStream,
                   kNotificationVerdictStream},
       .consumers = {{.stream = kGuardStream,
                      .name = "notification-known-seen",
                      .subject = kGuardKnownSeen,
                      .recreates = false},
                     {.stream = kAuthSessionStream,
                      .name = "notification-auth-session",
                      .subject = kAuthSession,
                      .recreates = false},
                     {.stream = kIdentityChangeStream,
                      .name = "notification-identity-user",
                      .subject = kIdentityChange,
                      .recreates = false},
                     modulesOf("argus-notification-modules")}},
      {.service = "productivity",
       .publishes = {},
       .stores = {kProductivityChange, kProductivityUserAction},
       .subscribes = {},
       .streams = {kProductivityChangeStream, kProductivityActionStream},
       .consumers = {modulesOf("argus-productivity-modules")}},
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
       .streams = {kSettingsModuleStream, kSettingsActionStream,
                   kSettingsChangeStream},
       .consumers = {}},
      {.service = "sync",
       .publishes = {kNotificationPushIntent},
       .stores = {},
       .subscribes = {kGuardHeartbeat, kGuardPresenceChanged, kTunnelLink},
       .streams = {},
       .consumers = {{.stream = kCameraStream,
                      .name = "argus-sync-camera",
                      .subject = kCameraChange,
                      .recreates = false},
                     {.stream = kNotificationChangeStream,
                      .name = "argus-sync-notification",
                      .subject = kNotificationChange,
                      .recreates = false},
                     {.stream = kProductivityChangeStream,
                      .name = "argus-sync-productivity",
                      .subject = kProductivityChange,
                      .recreates = false},
                     {.stream = kProductivityActionStream,
                      .name = "argus-sync-productivity-action",
                      .subject = kProductivityUserAction,
                      .recreates = false},
                     {.stream = kIdentityChangeStream,
                      .name = "argus-sync-identity",
                      .subject = kIdentityChange,
                      .recreates = false},
                     {.stream = kIdentityChangeStream,
                      .name = "argus-sync-identity-action",
                      .subject = kIdentityUserAction,
                      .recreates = false},
                     {.stream = kAuthChangeStream,
                      .name = "argus-sync-auth-action",
                      .subject = kAuthUserAction,
                      .recreates = false},
                     {.stream = kAuthSessionStream,
                      .name = "argus-sync-auth-session",
                      .subject = kAuthSession,
                      .recreates = false},
                     {.stream = kSettingsModuleStream,
                      .name = "argus-sync-settings-module",
                      .subject = kSettingsModule,
                      .recreates = false},
                     {.stream = kSettingsActionStream,
                      .name = "argus-sync-settings-action",
                      .subject = kSettingsUserAction,
                      .recreates = false},
                     {.stream = kSettingsChangeStream,
                      .name = "argus-sync-settings",
                      .subject = kSettingsChange,
                      .recreates = false},
                     {.stream = kGuardStream,
                      .name = "argus-sync-guard",
                      .subject = kGuardChange,
                      .recreates = false},
                     {.stream = kNotificationDeliveryStream,
                      .name = "argus-sync-delivery",
                      .subject = kNotificationDelivery,
                      .recreates = false},
                     modulesOf("argus-sync-modules")}},
      {.service = "tunnel",
       .publishes = {kTunnelLink, kNotificationPushResult},
       .stores = {},
       .subscribes = {kNotificationPushDelivery},
       .streams = {},
       .consumers = {}},
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

void addStreamGrants(std::string_view stream, std::vector<std::string>& publish)
{
  publish.push_back(dotted({"$JS.API.STREAM.CREATE", stream}));
  publish.push_back(dotted({"$JS.API.STREAM.INFO", stream}));
  publish.push_back(dotted({"$JS.API.STREAM.UPDATE", stream}));
}

void addConsumerGrants(const Consumer& consumer,
                       std::vector<std::string>& publish)
{
  publish.push_back(dotted({"$JS.API.CONSUMER.CREATE", consumer.stream,
                            consumer.name, consumer.subject}));
  publish.push_back(
      dotted({"$JS.API.CONSUMER.INFO", consumer.stream, consumer.name}));
  publish.push_back(dotted({"$JS.ACK", consumer.stream, consumer.name, ">"}));
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
  for (const std::string_view stream : row.streams)
    addStreamGrants(stream, grants.publish);
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
