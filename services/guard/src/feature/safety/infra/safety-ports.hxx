#pragma once

#include <feature/guard/vocabulary/safety-reach.hxx>
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
  int64_t sequence{1};
  bool spoken{false};
  int64_t saidAt{0};
};

enum class SafetyDelivery : uint8_t
{
  Sent,
  Pending,
  Refused,
  NoRecipients
};

class SafetyAlertSink
{
public:
  virtual ~SafetyAlertSink() = default;

  [[nodiscard]] virtual drogon::Task<SafetyDelivery> raise(const SafetyAlertNotice& notice) const = 0;

  [[nodiscard]] virtual drogon::Task<SafetyReach> reach(int64_t actorUserId) const = 0;
};

class SafetyActorNotifier
{
public:
  virtual ~SafetyActorNotifier() = default;

  [[nodiscard]] virtual drogon::Task<bool>
  confirmPanic(const SafetyAlertNotice& notice) const = 0;
};

struct SafetyActor
{
  bool known{false};
  bool active{true};
  std::string name;
  std::string role;
};

class SafetyActorDirectory
{
public:
  virtual ~SafetyActorDirectory() = default;

  [[nodiscard]] virtual drogon::Task<SafetyActor> actorOf(int64_t userId) const = 0;
};
