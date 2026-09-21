#pragma once

// The page size every bounded sync and audit query ends with: `SYNC_LIMIT = 200`
// (docs/architecture/wire-sync-tables.md). It is a wire invariant, not a tuning
// knob: the client pages by re-asking with a new cursor, and the number is part
// of the frozen contract. Declared once, here, so the SQL constants in the sync
// and audit repositories concatenate the same digits -- a C string, because
// every call site appends it to a std::string.
namespace SyncLimits
{
inline constexpr char kMaxRows[]{"200"};
} // namespace SyncLimits
