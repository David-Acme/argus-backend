#include "db-service.hxx"

#include <fstream>
#include <sstream>

drogon::Task<void> DbService::runScriptFile(const std::string& path)
{
  std::ifstream file(path);
  if (!file.is_open()) {
    LOG_WARN << "SQLite script not found: " << path;
    co_return;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  auto sql = buffer.str();

  // SQLite via Drogon executes statements sequentially through the client.
  auto client = DbService::client();
  try {
    co_await client->execSqlCoro(sql);
    LOG_INFO << "Applied SQLite script: " << path;
  } catch (const std::exception& e) {
    LOG_WARN << "SQLite script error (" << path << "): " << e.what();
  }
}
