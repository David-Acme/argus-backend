#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace context_note_query
{
inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM context_note WHERE id = ? AND deleted_at IS NULL";
inline constexpr std::string_view FIND_ACTIVE =
    "SELECT * FROM context_note WHERE is_active = 1 AND deleted_at IS NULL "
    "AND (valid_from IS NULL OR valid_from <= strftime('%s','now')) "
    "AND (valid_until IS NULL OR valid_until >= strftime('%s','now')) "
    "ORDER BY created_at DESC";
inline constexpr std::string_view INSERT =
    "INSERT INTO context_note (created_by, title, content, tags, "
    "valid_from, valid_until) VALUES (?, ?, ?, ?, ?, ?)";
inline constexpr std::string_view UPDATE_PREFIX = "UPDATE context_note SET ";
inline constexpr std::string_view UPDATE_COL_TITLE = "title = ?";
inline constexpr std::string_view UPDATE_COL_CONTENT = "content = ?";
inline constexpr std::string_view UPDATE_COL_TAGS = "tags = ?";
inline constexpr std::string_view UPDATE_COL_VALID_FROM = "valid_from = ?";
inline constexpr std::string_view UPDATE_COL_VALID_UNTIL = "valid_until = ?";
inline constexpr std::string_view UPDATE_COL_IS_ACTIVE = "is_active = ?";
inline constexpr std::string_view UPDATE_SUFFIX =
    ", updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";
inline constexpr std::string_view REMOVE =
    "UPDATE context_note SET deleted_at = strftime('%s', 'now'), "
    "updated_at = strftime('%s', 'now') WHERE id = ? AND deleted_at IS NULL";
} // namespace context_note_query

struct ContextNoteCreateInput
{
  std::optional<int64_t> createdBy;
  std::string title;
  std::string content;
  std::string tags;
  std::optional<int64_t> validFrom;
  std::optional<int64_t> validUntil;
};

struct ContextNoteUpdateInput
{
  std::optional<std::string> title;
  std::optional<std::string> content;
  std::optional<std::string> tags;
  std::optional<int64_t> validFrom;
  std::optional<int64_t> validUntil;
  std::optional<bool> isActive;
};
