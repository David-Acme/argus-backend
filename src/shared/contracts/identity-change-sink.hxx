#pragma once

#include <cstdint>
#include <json/value.h>
#include <string>

// Input of the identity-domain change publication (Ruling BX): one user or
// person row written by the identity surface. The row is the post-write
// snapshot; `deleted` marks a soft delete so the memory catalog replica can
// tombstone it.
struct IdentityChangeInput
{
  std::string table;
  int64_t id{0};
  bool deleted{false};
  Json::Value row;
};

// Substrate of the identity-domain change events. The legacy binds the NATS
// funnel shared with the gateway so the memory service catalog replicas stay
// fed from whichever process owns the identity writes.
class IdentityChangeSink
{
public:
  virtual ~IdentityChangeSink() = default;

  virtual void publish(const IdentityChangeInput& input) const = 0;
};

namespace identity_change
{
inline const IdentityChangeSink*& sink()
{
  static const IdentityChangeSink* instance = nullptr;
  return instance;
}

inline void setSink(const IdentityChangeSink* value)
{
  sink() = value;
}

inline const IdentityChangeSink* getSink()
{
  return sink();
}
} // namespace identity_change
