#include "secure-delete.hxx"

#include <drogon/drogon.h>
#include <exception>
#include <mutex>
#include <sqlite/db-service.hxx>
#include <sqlite/vec-db.hxx>
#include <sqlite3.h>

namespace
{
constexpr const char* kPragma = "PRAGMA secure_delete = ON";
}

bool secure_delete::enable()
{
  bool enabled = true;
  try {
    if (const auto client = DbService::identityClient())
      client->execSqlSync(kPragma);
  }
  catch (const std::exception& error) {
    LOG_WARN << "secure_delete could not be enabled on the identity client: "
             << error.what();
    enabled = false;
  }
  auto& vec = VecDb::instance();
  const std::scoped_lock lock(vec.mutex());
  sqlite3* handle = vec.handle();
  if (handle == nullptr ||
      sqlite3_exec(handle, kPragma, nullptr, nullptr, nullptr) != SQLITE_OK) {
    LOG_WARN << "secure_delete could not be enabled on the vector index";
    enabled = false;
  }
  return enabled;
}
