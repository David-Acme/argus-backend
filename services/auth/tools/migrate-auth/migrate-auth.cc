#include "auth-migration.hxx"

#include <iostream>
#include <string>

int main(int argc, char** argv)
{
  AuthMigrationOptions options;
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
      std::cerr << "usage: argus-migrate-auth --source <identity.db> "
                   "--target <auth.db> [--schema <schema.sql>]\n";
      return 2;
    }
  }

  const auto report = migrateAuth(options);
  for (const auto& table : report.tables) {
    std::cout << table.table << '\t' << table.sourceRows << " rows -> "
              << table.targetRows << " rows\tchecksum "
              << table.sourceChecksum << " -> " << table.targetChecksum << '\n';
  }
  if (!report.ok) {
    std::cerr << "auth migration FAILED: " << report.error << '\n';
    return 1;
  }
  std::cout << "auth migration verified: " << report.tables.size()
            << " tables copied\n";
  return 0;
}
