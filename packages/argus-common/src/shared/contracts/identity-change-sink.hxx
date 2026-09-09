#pragma once

#include <cstdint>
#include <json/value.h>
#include <string>

// Post-write snapshot of one user or person row; `deleted` marks a soft delete.
struct IdentityChangeInput
{
  std::string table;
  int64_t id{0};
  bool deleted{false};
  Json::Value row;
};

// Identity-domain change sink feeding the memory catalog replicas.
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
