#pragma once

#include <cstdint>
#include <shared/enums.hxx>
#include <string_view>
#include <vector>

namespace portrait_access_request_query
{

inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM portrait_access_request WHERE id = ?";

inline constexpr std::string_view FIND_PENDING_FOR_PORTRAIT =
    "SELECT * FROM portrait_access_request "
    "WHERE portrait_user_id = ? AND status = ? "
    "ORDER BY created_at ASC, id ASC";

inline constexpr std::string_view INSERT =
    "INSERT INTO portrait_access_request (portrait_user_id, requester_user_id) "
    "VALUES (?, ?)";

inline constexpr std::string_view RESOLVE =
    "UPDATE portrait_access_request SET status = ?, resolved_by = ?, "
    "resolved_at = strftime('%s', 'now'), updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND status = ?";

} // namespace portrait_access_request_query

struct PortraitAccessRequestCreateInput
{
  int64_t portraitUserId{0};
  int64_t requesterUserId{0};
};

struct PortraitAccessRequestResolveInput
{
  int64_t requestId{0};
  PortraitAccessRequestStatus status{PortraitAccessRequestStatus::Denied};
  int64_t resolvedBy{0};
};
