#pragma once
#include <cstdint>
#include <optional>
#include <shared/enums.hxx>
#include <string>
#include <string_view>

namespace project_member_query
{
/** `?` the visibility predicate spends in every sync query of this table. */
inline constexpr int OWNERSHIP_PLACEHOLDERS = 2;

inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM project_member WHERE id = ? AND deleted_at IS NULL";
inline constexpr std::string_view FIND_BY_PARENT =
    "SELECT * FROM project_member WHERE project_id = ? AND deleted_at IS NULL "
    "ORDER BY id ASC";
inline constexpr std::string_view FIND_ACCESS =
    "SELECT access FROM project_member WHERE project_id = ? AND user_id = ? "
    "AND deleted_at IS NULL";
inline constexpr std::string_view FIND_MEMBER_IDS =
    "SELECT user_id FROM project_member WHERE project_id = ? AND deleted_at IS NULL";
inline constexpr std::string_view FIND_EXISTING =
    "SELECT * FROM project_member WHERE project_id = ? AND user_id = ? "
    "AND deleted_at IS NULL";

inline constexpr std::string_view FIND =
    "SELECT * FROM project_member WHERE deleted_at IS NULL "
    "AND created_at >= ? AND created_at <= ? "
    "AND (user_id = ? OR EXISTS (SELECT 1 FROM project p "
    "WHERE p.id = project_member.project_id AND p.owner_id = ?)) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_FROM =
    "SELECT * FROM project_member WHERE deleted_at IS NULL AND created_at >= ? "
    "AND (user_id = ? OR EXISTS (SELECT 1 FROM project p "
    "WHERE p.id = project_member.project_id AND p.owner_id = ?)) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_AFTER =
    "SELECT * FROM project_member WHERE deleted_at IS NULL AND "
    "(created_at > ? OR (created_at = ? AND id > ?)) AND created_at <= ? "
    "AND (user_id = ? OR EXISTS (SELECT 1 FROM project p "
    "WHERE p.id = project_member.project_id AND p.owner_id = ?)) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_AFTER_FROM =
    "SELECT * FROM project_member WHERE deleted_at IS NULL AND "
    "(created_at > ? OR (created_at = ? AND id > ?)) "
    "AND (user_id = ? OR EXISTS (SELECT 1 FROM project p "
    "WHERE p.id = project_member.project_id AND p.owner_id = ?)) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_ALL =
    "SELECT * FROM project_member WHERE deleted_at IS NULL "
    "AND (user_id = ? OR EXISTS (SELECT 1 FROM project p "
    "WHERE p.id = project_member.project_id AND p.owner_id = ?)) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED =
    "SELECT * FROM project_member WHERE deleted_at IS NOT NULL "
    "AND deleted_at >= ? AND deleted_at <= ? "
    "AND (user_id = ? OR EXISTS (SELECT 1 FROM project p "
    "WHERE p.id = project_member.project_id AND p.owner_id = ?)) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_FROM =
    "SELECT * FROM project_member WHERE deleted_at IS NOT NULL AND deleted_at >= ? "
    "AND (user_id = ? OR EXISTS (SELECT 1 FROM project p "
    "WHERE p.id = project_member.project_id AND p.owner_id = ?)) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_AFTER =
    "SELECT * FROM project_member WHERE deleted_at IS NOT NULL AND "
    "(deleted_at > ? OR (deleted_at = ? AND id > ?)) AND deleted_at <= ? "
    "AND (user_id = ? OR EXISTS (SELECT 1 FROM project p "
    "WHERE p.id = project_member.project_id AND p.owner_id = ?)) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_AFTER_FROM =
    "SELECT * FROM project_member WHERE deleted_at IS NOT NULL AND "
    "(deleted_at > ? OR (deleted_at = ? AND id > ?)) "
    "AND (user_id = ? OR EXISTS (SELECT 1 FROM project p "
    "WHERE p.id = project_member.project_id AND p.owner_id = ?)) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_ALL =
    "SELECT * FROM project_member WHERE deleted_at IS NOT NULL "
    "AND (user_id = ? OR EXISTS (SELECT 1 FROM project p "
    "WHERE p.id = project_member.project_id AND p.owner_id = ?)) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_LAST =
    "SELECT * FROM project_member WHERE deleted_at IS NULL "
    "AND (user_id = ? OR EXISTS (SELECT 1 FROM project p "
    "WHERE p.id = project_member.project_id AND p.owner_id = ?)) "
    "ORDER BY created_at DESC LIMIT 1";
inline constexpr std::string_view FIND_LAST_DELETED =
    "SELECT * FROM project_member WHERE deleted_at IS NOT NULL "
    "AND (user_id = ? OR EXISTS (SELECT 1 FROM project p "
    "WHERE p.id = project_member.project_id AND p.owner_id = ?)) "
    "ORDER BY deleted_at DESC LIMIT 1";

inline constexpr std::string_view INSERT =
    "INSERT INTO project_member (project_id, user_id, access) VALUES (?, ?, ?)";
inline constexpr std::string_view UPDATE_ACCESS =
    "UPDATE project_member SET access = ?, updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";
inline constexpr std::string_view REMOVE =
    "UPDATE project_member SET deleted_at = strftime('%s', 'now'), "
    "updated_at = strftime('%s', 'now') WHERE id = ? AND deleted_at IS NULL";
} // namespace project_member_query

struct ProjectMemberCreateInput
{
  int64_t projectId{0};
  int64_t userId{0};
  ShareAccess access{ShareAccess::View};
};
