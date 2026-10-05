#pragma once

#include <feature/safety/vocabulary/safety-alert-kind.hxx>

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <string>

struct SafetyAlertNotice
{
  SafetyAlertKind kind{SafetyAlertKind::Panic};
  int64_t alertId{0};
  int64_t actorUserId{0};
  std::string actorName;
  int64_t environmentId{0};
  int64_t now{0};
};

class SafetyAlertSink
{
public:
  virtual ~SafetyAlertSink() = default;

  [[nodiscard]] virtual drogon::Task<bool> raise(const SafetyAlertNotice& notice) const = 0;
};

class SafetyActorNotifier
{
public:
  virtual ~SafetyActorNotifier() = default;

  [[nodiscard]] virtual drogon::Task<bool>
  confirmPanic(const SafetyAlertNotice& notice) const = 0;
};
