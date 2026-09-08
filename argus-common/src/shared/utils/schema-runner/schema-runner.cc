#include <drogon/drogon.h>
#include <fstream>
#include <shared/utils/schema-runner/schema-runner.hxx>
#include <sqlite3.h>
#include <sstream>
#include <string>

bool runSchemaFile(sqlite3* db, const std::string& path)
{
  if (!db)
    return false;
  std::ifstream file(path);
  if (!file.is_open()) {
    LOG_WARN << "runSchemaFile: cannot open " << path;
    return false;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();

  std::string current;
  std::string line;
  while (std::getline(buffer, line)) {
    auto start = line.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
      continue;
    auto end = line.find_last_not_of(" \t\r\n");
    std::string trimmed = line.substr(start, end - start + 1);
    if (trimmed.rfind("--", 0) == 0)
      continue;
    current += trimmed + "\n";
    if (trimmed.back() == ';') {
      char* err = nullptr;
      if (sqlite3_exec(db, current.c_str(), nullptr, nullptr, &err) !=
          SQLITE_OK) {
        LOG_WARN << "runSchemaFile: statement failed: "
                 << (err ? err : "unknown");
        sqlite3_free(err);
      }
      current.clear();
    }
  }
  return true;
}
