#include "sync-migration.hxx"

#include <iostream>
#include <string>

int main(int argc, char** argv)
{
  SyncMigrationOptions options;
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
    } else {
      std::cerr << "usage: argus-migrate-sync --source <identity.db> "
                   "--target <sync.db> [--schema <schema.sql>]\n";
      return 2;
    }
  }

  const auto report = migrateSync(options);
  int64_t tables = 0;
  for (const auto& table : report.tables) {
    if (!table.present) {
      std::cout << table.table << "\tabsent in the source\n";
      continue;
    }
    ++tables;
    std::cout << table.table << '\t' << table.copiedRows
              << " rows copied, " << table.skippedRows << " kept\tchecksum "
              << table.sourceChecksum << " -> " << table.targetChecksum << '\n';
  }
  if (report.sourceReadWrite)
    std::cerr << "warning: the source was attached read-write because its "
                 "write-ahead log needed recovery; no statement targeted it\n";
  if (!report.ok) {
    std::cerr << "sync migration FAILED: " << report.error << '\n';
    return 1;
  }
  std::cout << "sync migration verified: " << tables << " tables\n";
  return 0;
}
