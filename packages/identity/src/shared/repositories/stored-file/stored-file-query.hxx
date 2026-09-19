#pragma once

#include <cstdint>
#include <optional>
#include <shared/enums.hxx>
#include <string>
#include <string_view>

namespace stored_file_query
{

inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM stored_file WHERE id = ? AND deleted_at IS NULL";

inline constexpr std::string_view FIND_BY_OBJECT_KEY =
    "SELECT * FROM stored_file WHERE object_key = ? AND deleted_at IS NULL";

inline constexpr std::string_view INSERT =
    "INSERT INTO stored_file "
    "(object_key, sha256, mime_type, byte_size, category, created_by) "
    "VALUES (?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view REMOVE =
    "UPDATE stored_file SET deleted_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";

} // namespace stored_file_query

struct StoredFileCreateInput
{
  std::string objectKey;
  std::string sha256;
  std::string mimeType;
  int64_t byteSize{0};
  StoredFileCategory category{StoredFileCategory::Portrait};
  std::optional<int64_t> createdBy;
};
