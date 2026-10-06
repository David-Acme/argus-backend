#pragma once

#include <cstdint>

namespace module_journal_query
{
inline constexpr const char* SELECT_CURSOR = "SELECT published_through FROM module_journal WHERE id = 1";
inline constexpr const char* INSERT_CURSOR = "INSERT OR IGNORE INTO module_journal (id, published_through) VALUES (1, ?)";
inline constexpr const char* ADVANCE_CURSOR =
    "UPDATE module_journal SET published_through = ? WHERE id = 1 AND published_through < ?";
}

struct ModuleJournalAdvanceInput
{
  std::int64_t auditId{0};
};
