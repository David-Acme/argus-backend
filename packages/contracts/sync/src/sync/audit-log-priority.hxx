#pragma once

#include <cstdint>
#include <optional>

enum class AuditLogPriority : uint8_t
{
  Low = 0,
  Medium = 1,
  High = 2
};

inline std::optional<AuditLogPriority> auditLogPriorityFromInt(int value)
{
  if (value < static_cast<int>(AuditLogPriority::Low) ||
      value > static_cast<int>(AuditLogPriority::High))
    return std::nullopt;
  return static_cast<AuditLogPriority>(value);
}
