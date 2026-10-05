#pragma once

#include <shared/vocabulary/presence-state.hxx>

#include <cstdint>
#include <optional>
#include <vector>

class PresenceDirectory
{
public:
  PresenceDirectory() = default;
  PresenceDirectory(const PresenceDirectory&) = delete;
  PresenceDirectory& operator=(const PresenceDirectory&) = delete;
  virtual ~PresenceDirectory() = default;

  [[nodiscard]] virtual std::optional<bool>
  presenceConsent(int64_t userId) const = 0;

  [[nodiscard]] virtual std::optional<int64_t>
  userOfPerson(int64_t personId) const = 0;

  [[nodiscard]] virtual std::optional<std::vector<int64_t>>
  consentingUsers() const = 0;
};

struct PresenceChange
{
  PresenceRow row;
  PresenceState overall{PresenceState::Unknown};
};

class PresencePublisher
{
public:
  PresencePublisher() = default;
  PresencePublisher(const PresencePublisher&) = delete;
  PresencePublisher& operator=(const PresencePublisher&) = delete;
  virtual ~PresencePublisher() = default;

  virtual void publish(const PresenceChange& change) = 0;
};
