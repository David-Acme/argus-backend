#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <sync/user-action-event.hxx>

struct AuthActionPublishInput
{
  UserActionEvent event;
  drogon::orm::DbClient* client{nullptr};
};

class AuthChangeSink
{
public:
  virtual ~AuthChangeSink() = default;

  [[nodiscard]] virtual drogon::Task<void>
  publishAction(const AuthActionPublishInput& input) const = 0;
};

namespace auth_change
{
inline const AuthChangeSink*& sink()
{
  static const AuthChangeSink* instance = nullptr;
  return instance;
}

inline void setSink(const AuthChangeSink* value)
{
  sink() = value;
}

inline const AuthChangeSink* getSink()
{
  return sink();
}
}
