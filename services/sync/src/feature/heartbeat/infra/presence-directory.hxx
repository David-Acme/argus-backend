#pragma once

#include <feature/heartbeat/services/presence-board.hxx>

#include <optional>
#include <vector>

class PresenceDirectory
{
public:
  virtual ~PresenceDirectory() = default;

  [[nodiscard]] virtual std::optional<std::vector<PresenceEntry>> list() const = 0;
};
