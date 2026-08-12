#include "kuzu-db.hxx"

#include <drogon/drogon.h>
#include <main/connection.h>
#include <main/database.h>
#include <main/query_result.h>
#include <memory>
#include <string>
#include <vector>

namespace
{

std::mutex gMutex;
std::unique_ptr<kuzu::main::Database> gDb;
std::unique_ptr<kuzu::main::Connection> gConn;

// DDL from COGNITIVE_MEMORY_PLAN.md §4.1-4.2. embedding FLOAT[384] is fixed
// at DDL time in Kùzu: 384 is the embedding dimension of
// multilingual-e5-small, the only embedding model in Argus
// (memory.embedding_dim default). Decided before Phase 1 — no rebuild path
// needed for the current model.
const std::vector<std::string> kSchema = {
    "CREATE NODE TABLE IF NOT EXISTS Scope ("
    "  id SERIAL, kind STRING, ref_id INT64,"
    "  PRIMARY KEY (id))",
    "CREATE NODE TABLE IF NOT EXISTS Entity ("
    "  id SERIAL, kind STRING, canonical STRING, lang STRING,"
    "  created_at INT64, updated_at INT64,"
    "  PRIMARY KEY (id))",
    "CREATE NODE TABLE IF NOT EXISTS Alias ("
    "  id SERIAL, surface STRING, norm STRING, lang STRING,"
    "  person_frame STRING, confidence FLOAT,"
    "  PRIMARY KEY (id))",
    "CREATE NODE TABLE IF NOT EXISTS Fact ("
    "  id SERIAL, predicate STRING, value STRING, canonical STRING,"
    "  type STRING, priority INT32, confidence FLOAT, lang STRING,"
    "  valid_from INT64, valid_to INT64, hit_count INT64,"
    "  created_at INT64, updated_at INT64,"
    "  embedding FLOAT[384],"
    "  PRIMARY KEY (id))",
    "CREATE NODE TABLE IF NOT EXISTS Episode ("
    "  id SERIAL, kind STRING, summary STRING, actor STRING,"
    "  occurred_at INT64, session_id STRING, lang STRING,"
    "  salience FLOAT, hit_count INT64, last_recalled_at INT64,"
    "  decayed_at INT64, rolled_up BOOLEAN,"
    "  embedding FLOAT[384],"
    "  PRIMARY KEY (id))",
    "CREATE NODE TABLE IF NOT EXISTS Procedure ("
    "  id SERIAL, name STRING, goal STRING, steps STRING,"
    "  uses INT64, successes INT64, updated_at INT64,"
    "  PRIMARY KEY (id))",
    "CREATE NODE TABLE IF NOT EXISTS Source ("
    "  id SERIAL, channel STRING, turn_ref STRING, at INT64,"
    "  PRIMARY KEY (id))",
    "CREATE REL TABLE IF NOT EXISTS ALIAS_OF (FROM Alias TO Entity)",
    "CREATE REL TABLE IF NOT EXISTS ABOUT (FROM Fact TO Entity, role STRING)",
    "CREATE REL TABLE IF NOT EXISTS RELATED (FROM Entity TO Entity,"
    "  predicate STRING, since INT64, until INT64)",
    "CREATE REL TABLE IF NOT EXISTS IN_SCOPE (FROM Fact TO Scope)",
    "CREATE REL TABLE IF NOT EXISTS EP_SCOPE (FROM Episode TO Scope)",
    "CREATE REL TABLE IF NOT EXISTS MENTIONS (FROM Episode TO Entity)",
    "CREATE REL TABLE IF NOT EXISTS DERIVED (FROM Fact TO Source)",
    "CREATE REL TABLE IF NOT EXISTS EP_DERIVED (FROM Episode TO Source)",
    "CREATE REL TABLE IF NOT EXISTS SUPERSEDES (FROM Fact TO Fact, at INT64)",
    "CREATE REL TABLE IF NOT EXISTS ROLLED_UP (FROM Fact TO Episode)",
    "CREATE REL TABLE IF NOT EXISTS PROC_STEP (FROM Procedure TO Procedure,"
    "  ord INT32)",
};

bool runUnlocked(const std::string& sql)
{
  auto result = gConn->query(sql);
  if (!result->isSuccess()) {
    LOG_WARN << "KuzuDb: query failed: " << result->getErrorMessage()
             << " | sql: " << sql;
    return false;
  }
  return true;
}

} // namespace

std::mutex& KuzuDb::mutex()
{
  return gMutex;
}

kuzu::main::Connection* KuzuDb::handle()
{
  return gConn.get();
}

void KuzuDb::init(const std::string& dbPath)
{
  std::scoped_lock lock(gMutex);
  if (gDb)
    return;
  gDb = std::make_unique<kuzu::main::Database>(dbPath);
  gConn = std::make_unique<kuzu::main::Connection>(gDb.get());
  LOG_INFO << "KuzuDb: graph database ready (" << dbPath << ")";
}

void KuzuDb::shutdown()
{
  std::scoped_lock lock(gMutex);
  gConn.reset();
  gDb.reset();
}

void KuzuDb::applySchema()
{
  std::scoped_lock lock(gMutex);
  applySchemaUnlocked();
}

void KuzuDb::applySchemaUnlocked()
{
  if (!gConn)
    return;
  for (const auto& ddl : kSchema)
    runUnlocked(ddl);
}
