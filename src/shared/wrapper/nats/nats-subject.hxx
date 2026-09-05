#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace nats_subject
{

inline constexpr const char* kSyncChange = "argus.sync.v1.change";
inline constexpr const char* kSyncChangeWildcard = "argus.*.v1.change";

// Camera-domain changes funnel from argus-camera over their own subject so the
// gateway can distinguish an audit event it must persist first from a plain
// legacy fan-out event (F2-2).
inline constexpr const char* kCameraChange = "argus.camera.v1.change";

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
    // ">" matches one or more trailing tokens and is only valid as the last
    // token; this stays valid across every nats-server version.
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
