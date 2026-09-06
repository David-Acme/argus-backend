#include "notification-migration.hxx"

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

ConfigDbResult notificationDbFromConfig(const std::string& configPath)
{
  ConfigDbResult result;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(configPath, ec)) {
    result.error = "config file not found: " + configPath;
    return result;
  }
  try {
    const auto table = toml::parse_file(configPath);
    const auto* notifications = table["notifications"].as_table();
    if (!notifications || !notifications->contains("db")) {
      result.error = "config file has no [notifications] db key: " + configPath;
      return result;
    }
    result.value = notifications->at("db").value_or(std::string());
    if (result.value.empty()) {
      result.error =
          "config file has an empty [notifications] db key: " + configPath;
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
  NotificationMigrationOptions options;
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
      std::cerr << "usage: argus-migrate-notification --source <argus.db> "
                   "--target <notification.db> "
                   "[--schema <notification-schema.sql>] "
                   "[--config <config.toml>]\n";
      return 2;
    }
  }

  if (options.targetPath.empty() && !configPath.empty()) {
    const auto db = notificationDbFromConfig(configPath);
    if (!db.ok) {
      std::cerr << "notification migration FAILED: --config error: " << db.error
                << '\n';
      return 2;
    }
    options.targetPath = db.value;
  }

  const auto report = migrateNotification(options);
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
    std::cerr << "notification migration FAILED: " << report.error << '\n';
    return 1;
  }
  if (report.noop) {
    const bool empty =
        std::all_of(report.tables.begin(), report.tables.end(),
                    [](const NotificationTableReport& entry) {
                      return entry.targetRows == 0;
                    });
    if (empty)
      std::cerr << "warning: notification.db holds the schema but no rows were "
                   "ever copied; if this is unexpected, remove it and re-run "
                   "the migration\n";
    std::cout << "notification.db is already schema-current; nothing copied\n";
    return 0;
  }
  std::cout << "notification migration verified: " << report.tables.size()
            << " tables copied\n";
  return 0;
}