#pragma once

#include <auth/session-origin.hxx>
#include <auth/session-platform.hxx>
#include <shared/vocabulary/presence-state.hxx>

#include <cstdint>
#include <optional>

enum class PresenceSignalKind : uint8_t
{
  LanSession = 0,
  TunnelSession,
  AppActivity,
  Camera
};

struct PresenceSignal
{
  int64_t userId{0};
  int64_t environmentId{0};
  PresenceSignalKind kind{PresenceSignalKind::LanSession};
  int64_t at{0};
};

struct PresenceRules
{
  int64_t awayTimeoutSeconds{2700};
  int64_t tunnelGraceSeconds{90};
  int64_t retentionSeconds{2592000};
};

struct PresenceApplyInput
{
  const std::optional<PresenceRow>& current;
  const PresenceSignal& signal;
  const PresenceRules& rules;
};

struct PresenceOutcome
{
  PresenceRow row;
  bool write{false};
  bool changed{false};
};

struct SessionSignalInput
{
  SessionOrigin origin{SessionOrigin::Unknown};
  SessionPlatform platform{SessionPlatform::Unknown};
};

namespace presence_engine
{

[[nodiscard]] PresenceSource sourceOf(PresenceSignalKind kind);

[[nodiscard]] std::optional<PresenceSignalKind>
kindOf(const SessionSignalInput& input);

[[nodiscard]] PresenceOutcome apply(const PresenceApplyInput& input);

}
