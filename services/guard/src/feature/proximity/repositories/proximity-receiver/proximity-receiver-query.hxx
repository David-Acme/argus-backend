#pragma once

#include <feature/proximity/vocabulary/proximity-kinds.hxx>

#include <cstdint>
#include <string>
#include <string_view>

namespace proximity_receiver_query
{

inline constexpr std::string_view SELECT_RECEIVERS =
    "SELECT id, name, room, kind, host, port, created_at FROM guard_proximity_receiver "
    "WHERE deleted_at IS NULL ORDER BY id";

inline constexpr std::string_view COUNT_RECEIVERS =
    "SELECT COUNT(*) AS total FROM guard_proximity_receiver WHERE deleted_at IS NULL";

inline constexpr std::string_view INSERT_RECEIVER =
    "INSERT INTO guard_proximity_receiver (name, room, kind, host, port, created_at) "
    "VALUES (?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view DELETE_RECEIVER =
    "UPDATE guard_proximity_receiver SET deleted_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";

}

struct ProximityReceiverRow
{
  int64_t id{0};
  std::string name;
  std::string room;
  ProximityReceiverKind kind{ProximityReceiverKind::Esphome};
  std::string host;
  int64_t port{80};
  int64_t createdAt{0};
};

struct ProximityReceiverCreateInput
{
  std::string name;
  std::string room;
  ProximityReceiverKind kind{ProximityReceiverKind::Esphome};
  std::string host;
  int64_t port{80};
  int64_t now{0};
};
