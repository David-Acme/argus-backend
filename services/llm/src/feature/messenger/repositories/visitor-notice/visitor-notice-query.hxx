#pragma once

#include <cstdint>
#include <string>

namespace visitor_notice_query
{
inline constexpr const char* INSERT_NOTICE =
    "INSERT INTO visitor_notice (camera, to_user_id, created_at) VALUES (?, ?, ?)";
inline constexpr const char* REMOVE_NOTICE = "DELETE FROM visitor_notice WHERE id = ?";
inline constexpr const char* PURGE_BEFORE = "DELETE FROM visitor_notice WHERE created_at < ?";
inline constexpr const char* COUNT_SINCE =
    "SELECT COUNT(*) FROM visitor_notice WHERE camera = ? AND to_user_id = ? AND created_at >= ?";
}

struct VisitorNoticeWriteInput
{
  std::string camera;
  int64_t toUserId{0};
  int64_t at{0};
};

struct VisitorNoticeCountInput
{
  std::string camera;
  int64_t toUserId{0};
  int64_t since{0};
};
