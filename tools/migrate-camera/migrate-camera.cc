#include "camera-migration.hxx"

#include <toml++/toml.hpp>

#include <filesystem>
#include <iostream>
#include <string>

namespace
{

std::string cameraDbFromConfig(const std::string& configPath)
{
  std::error_code ec;
  if (!std::filesystem::is_regular_file(configPath, ec))
    return {};
  try {
    const auto table = toml::parse_file(configPath);
    return table["camera"]["db"].value_or(std::string());
  }
  catch (const toml::parse_error&) {
    return {};
  }
}

} // namespace

int main(int argc, char** argv)
{
  CameraMigrationOptions options;
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
      std::cerr << "usage: argus-migrate-camera --source <argus.db> "
                   "--target <camera.db> [--schema <camera-schema.sql>] "
                   "[--config <config.toml>]\n";
      return 2;
    }
  }

  if (options.targetPath.empty() && !configPath.empty())
    options.targetPath = cameraDbFromConfig(configPath);

  const auto report = migrateCamera(options);
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
    std::cerr << "camera migration FAILED: " << report.error << '\n';
    return 1;
  }
  if (report.noop) {
    std::cout << "camera.db is already schema-current; nothing copied\n";
    return 0;
  }
  std::cout << "camera migration verified: " << report.tables.size()
            << " tables copied\n";
  return 0;
}
