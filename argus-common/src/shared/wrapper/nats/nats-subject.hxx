#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace nats_subject
{

inline constexpr const char* kSyncChange = "argus.sync.v1.change";
inline constexpr const char* kSyncChangeWildcard = "argus.*.v1.change";

// Camera-domain changes funneled from argus-camera.
inline constexpr const char* kCameraChange = "argus.camera.v1.change";

// Productivity-domain changes plus `kind: audit` user_audit_log diffs.
inline constexpr const char* kProductivityChange =
    "argus.productivity.v1.change";

// Notification-domain markAsRead effects, same payload contract as productivity.
inline constexpr const char* kNotificationChange =
    "argus.notification.v1.change";

// Identity-domain user and person writes, funneled to the memory catalog replicas.
inline constexpr const char* kIdentityChange = "argus.identity.v1.change";

// Object-detection events from the argus-camera operator; never re-emitted to /sync.
inline constexpr const char* kCameraObjectDetected =
    "argus.camera.v1.object_detected";

// Push intents fanned to the home client through the tunnel; never re-emitted to /sync.
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

} // namespace nats_subject
