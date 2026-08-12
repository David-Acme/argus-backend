#pragma once

#include <mutex>
#include <string>

namespace kuzu::main
{
class Database;
class Connection;
} // namespace kuzu::main

// Single serialized Kùzu access point, mirroring VecDb's contract:
// callers take mutex() around every handle() use, handle() never blocks,
// the mutex is non-recursive. One Database + one Connection for the whole
// process — the house pattern (AGENTS.md §13d); Argus has no concurrent
// multi-store requirement, and the Vela fork that provides it crashes any
// second connection (DirectedCSRIndex OOB, see COGNITIVE_MEMORY_PLAN.md §0).
class KuzuDb
{
public:
  KuzuDb() = delete;
  ~KuzuDb() = delete;

  // Opens (or reopens) the database file. No-op if already open. Idempotent.
  static void init(const std::string& dbPath);
  static void shutdown();

  // Callers take this mutex around every handle() use. Non-recursive.
  static std::mutex& mutex();
  // The single serialized connection. Never locks; valid only after init()
  // and while the caller holds mutex().
  static kuzu::main::Connection* handle();

  // Applies the cognitive-memory schema (COGNITIVE_MEMORY_PLAN.md §4),
  // idempotent. Locks internally; safe to call at boot.
  static void applySchema();

private:
  static void applySchemaUnlocked();
};
