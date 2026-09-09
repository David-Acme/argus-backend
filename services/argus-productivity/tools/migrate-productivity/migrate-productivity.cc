#include "productivity-migration.hxx"

#include <toml++/toml.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace
{

struct ConfigDbResult
{
  bool ok = false;
  std::string value;
  std::string error;
};

ConfigDbResult productivityDbFromConfig(const std::string& configPath)
{
  ConfigDbResult result;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(configPath, ec)) {
    result.error = "config file not found: " + configPath;
    return result;
  }
  try {
    const auto table = toml::parse_file(configPath);
    const auto* productivity = table["productivity"].as_table();
    if (!productivity || !productivity->contains("db")) {
      result.error = "config file has no [productivity] db key: " + configPath;
      return result;
    }
    result.value = productivity->at("db").value_or(std::string());
    if (result.value.empty()) {
      result.error =
          "config file has an empty [productivity] db key: " + configPath;
      return result;
    }
  }
  catch (const toml::parse_error& error) {
    result.error = "cannot parse config file " + configPath + ": "
                   + std::string(error.description());
    return result;
  }
  result.ok = true;
  return result;
}

} // namespace

int main(int argc, char** argv)
{
  ProductivityMigrationOptions options;
  std::string configPath;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto value = [&](int index) -> std::string {
      return (index + 1 < argc) ? argv[index + 1] : std::string();
    };
    if (arg == "--source" && i + 1 < argc) {
      options.sourcePath = value(i);
      ++i;
    } else if (arg == "--target" && i + 1 < argc) {
      options.targetPath = value(i);
      ++i;
    } else if (arg == "--schema" && i + 1 < argc) {
      options.schemaPath = value(i);
      ++i;
    } else if (arg == "--config" && i + 1 < argc) {
      configPath = value(i);
      ++i;
    } else {
      std::cerr << "usage: argus-migrate-productivity --source <argus.db> "
                   "--target <productivity.db> "
                   "[--schema <productivity-schema.sql>] "
                   "[--config <config.toml>]\n";
      return 2;
    }
  }

  if (options.targetPath.empty() && !configPath.empty()) {
    const auto db = productivityDbFromConfig(configPath);
    if (!db.ok) {
      std::cerr << "productivity migration FAILED: --config error: " << db.error
                << '\n';
      return 2;
    }
    options.targetPath = db.value;
  }

  const auto report = migrateProductivity(options);
  for (const auto& table : report.tables) {
    if (report.noop)
      std::cout << table.table << '\t' << table.targetRows
                << " rows (no-op)\tchecksum " << table.targetChecksum << '\n';
    else
      std::cout << table.table << '\t' << table.sourceRows << " rows -> "
                << table.targetRows << " rows\tchecksum "
                << table.sourceChecksum << " -> " << table.targetChecksum
                << '\n';
  }
  if (!report.ok) {
    std::cerr << "productivity migration FAILED: " << report.error << '\n';
    return 1;
  }
  if (report.noop) {
    const bool empty =
        std::all_of(report.tables.begin(), report.tables.end(),
                    [](const ProductivityTableReport& entry) {
                      return entry.targetRows == 0;
                    });
    if (empty)
      std::cerr << "warning: productivity.db holds the schema but no rows were "
                   "ever copied; if this is unexpected, remove it and re-run "
                   "the migration\n";
    std::cout << "productivity.db is already schema-current; nothing copied\n";
    return 0;
  }
  std::cout << "productivity migration verified: " << report.tables.size()
            << " tables copied\n";
  return 0;
}
