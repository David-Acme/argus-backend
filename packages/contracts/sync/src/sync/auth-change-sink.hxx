#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <sync/user-action-event.hxx>

struct AuthActionPublishInput
{
  UserActionEvent event;
  drogon::orm::DbClient* client{nullptr};
};

struct AuthSessionChangeInput
{
  Json::Value payload;
  drogon::orm::DbClient* client{nullptr};
};

class AuthChangeSink
{
public:
  virtual ~AuthChangeSink() = default;

  [[nodiscard]] virtual drogon::Task<void>
  publishAction(const AuthActionPublishInput& input) const = 0;

  [[nodiscard]] virtual drogon::Task<void>
  publishSessionChange(const AuthSessionChangeInput& input) const = 0;
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
