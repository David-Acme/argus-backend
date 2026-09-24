#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace nats_subject
{

inline constexpr const char* kSyncChange = "argus.sync.v1.change";

inline constexpr const char* kCameraChange = "argus.camera.v1.change";
inline constexpr const char* kCameraStream = "ARGUS_CAMERA";

inline constexpr const char* kProductivityChange =
    "argus.productivity.v1.change";
inline constexpr const char* kProductivityChangeStream =
    "ARGUS_PRODUCTIVITY_CHANGE";

inline constexpr const char* kNotificationChange =
    "argus.notification.v1.change";
inline constexpr const char* kNotificationChangeStream =
    "ARGUS_NOTIFICATION_CHANGE";

inline constexpr const char* kIdentityChange = "argus.identity.v1.change";

inline constexpr const char* kIdentityUserAction =
    "argus.identity.v1.user-action";
inline constexpr const char* kIdentityChangeStream = "ARGUS_IDENTITY_CHANGE";

inline constexpr const char* kCameraObjectDetected =
    "argus.camera.v1.object_detected";

inline constexpr const char* kCameraHealth = "argus.camera.v1.health";

inline constexpr const char* kGuardHeartbeat = "argus.guard.v1.heartbeat";

inline constexpr const char* kGuardEncounterClosed =
    "argus.guard.v1.encounter_closed";

inline constexpr const char* kGuardStream = "ARGUS_GUARD";

inline constexpr const char* kGuardSubjectFilter = "argus.guard.v1.>";

inline constexpr const char* kNotificationDeliveryStream = "ARGUS_NOTIFICATION";
inline constexpr const char* kNotificationDelivery =
    "argus.notification.v1.delivery";

inline constexpr const char* kNotificationPushIntent =
    "argus.notification.v1.push_intent";

enum class SubjectKind
{
  Publish,
  Subscribe
};

inline bool isValidSubject(std::string_view subject, SubjectKind kind)
{
  if (subject.empty())
    return false;

  size_t tokenStart = 0;
  while (true) {
    const size_t dot = subject.find('.', tokenStart);
    const std::string_view token = dot == std::string::npos
                                       ? subject.substr(tokenStart)
                                       : subject.substr(tokenStart, dot - tokenStart);

    if (token.empty())
      return false;

    const bool isStarWildcard = token.size() == 1 && token[0] == '*';
    const bool isGreaterWildcard = token.size() == 1 && token[0] == '>';

    if (kind == SubjectKind::Publish &&
        (isStarWildcard || isGreaterWildcard))
      return false;
    if (isGreaterWildcard && dot != std::string::npos)
      return false;

    if (!isStarWildcard && !isGreaterWildcard) {
      for (const char c : token) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' ||
              c == '_'))
          return false;
      }
    }

    if (dot == std::string::npos)
      return true;
    tokenStart = dot + 1;
  }
}

}
