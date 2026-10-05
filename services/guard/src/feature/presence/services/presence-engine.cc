#include "presence-engine.hxx"

namespace
{
bool homeSignal(PresenceSignalKind kind)
{
  return kind != PresenceSignalKind::TunnelSession;
}

bool mobile(SessionPlatform platform)
{
  return platform == SessionPlatform::Android ||
         platform == SessionPlatform::Ios;
}

PresenceOutcome unchanged(const std::optional<PresenceRow>& current)
{
  return {.row = current.value_or(PresenceRow{}),
          .write = false,
          .changed = false};
}
}

PresenceSource presence_engine::sourceOf(PresenceSignalKind kind)
{
  switch (kind) {
    case PresenceSignalKind::LanSession:
      return PresenceSource::LanSession;
    case PresenceSignalKind::TunnelSession:
      return PresenceSource::TunnelSession;
    case PresenceSignalKind::AppActivity:
      return PresenceSource::AppActivity;
    case PresenceSignalKind::Camera:
      return PresenceSource::Camera;
  }
  return PresenceSource::None;
}

std::optional<PresenceSignalKind>
presence_engine::kindOf(const SessionSignalInput& input)
{
  if (input.origin == SessionOrigin::Tunnel)
    return PresenceSignalKind::TunnelSession;
  if (input.origin != SessionOrigin::Lan)
    return std::nullopt;
  return mobile(input.platform) ? PresenceSignalKind::LanSession
                                : PresenceSignalKind::AppActivity;
}

PresenceOutcome presence_engine::apply(const PresenceApplyInput& input)
{
  const auto& current = input.current;
  const PresenceSignal& signal = input.signal;
  if (signal.userId <= 0 || signal.environmentId <= 0 || signal.at <= 0)
    return unchanged(current);
  if (current && signal.at < current->lastSignalAt)
    return unchanged(current);

  const PresenceSource source = sourceOf(signal.kind);
  if (homeSignal(signal.kind)) {
    const bool wasHome = current && current->state == PresenceState::Home;
    return {.row = {.userId = signal.userId,
                    .environmentId = signal.environmentId,
                    .state = PresenceState::Home,
                    .source = source,
                    .since = wasHome ? current->since : signal.at,
                    .lastHomeAt = signal.at,
                    .lastSignalAt = signal.at},
            .write = true,
            .changed = !wasHome};
  }

  if (current && current->state == PresenceState::Home &&
      current->source != PresenceSource::Timeout &&
      signal.at - current->lastHomeAt < input.rules.tunnelGraceSeconds)
    return unchanged(current);

  const bool wasAway = current && current->state == PresenceState::Away;
  const bool strengthened = wasAway && current->source != source;
  return {.row = {.userId = signal.userId,
                  .environmentId = signal.environmentId,
                  .state = PresenceState::Away,
                  .source = source,
                  .since = wasAway ? current->since : signal.at,
                  .lastHomeAt = current ? current->lastHomeAt : 0,
                  .lastSignalAt = signal.at},
          .write = true,
          .changed = !wasAway || strengthened};
}
