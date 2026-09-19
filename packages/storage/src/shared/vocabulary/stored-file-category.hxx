#pragma once

#include <cstdint>
#include <string>

// Private objects stay out of the sync stream; the category drives retention
// and access policy.
enum class StoredFileCategory : uint8_t
{
  Portrait = 0,
  Attachment
};

inline std::string storedFileCategoryToString(StoredFileCategory category)
{
  return category == StoredFileCategory::Attachment ? "attachment" : "portrait";
}

inline StoredFileCategory storedFileCategoryFromString(const std::string& value)
{
  return value == "attachment" ? StoredFileCategory::Attachment
                               : StoredFileCategory::Portrait;
}
