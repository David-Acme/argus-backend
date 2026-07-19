#include "app-env-context.hxx"

#include <fstream>
#include <iostream>

std::unordered_map<std::string, std::string> AppEnvContext::values_;

void AppEnvContext::load(const std::string& path)
{
  std::ifstream file(path);
  if (!file.is_open()) {
    std::cerr << "[AppEnvContext] .env not found at " << path << std::endl;
    return;
  }

  std::string line;
  while (std::getline(file, line)) {
    line.erase(0, line.find_first_not_of(" \t"));
    line.erase(line.find_last_not_of(" \t") + 1);
    if (line.empty() || line.starts_with('#'))
      continue;

    auto eq = line.find('=');
    if (eq == std::string::npos)
      continue;

    auto key = line.substr(0, eq);
    auto val = line.substr(eq + 1);
    key.erase(0, key.find_first_not_of(" \t"));
    key.erase(key.find_last_not_of(" \t") + 1);
    val.erase(0, val.find_first_not_of(" \t\""));
    val.erase(val.find_last_not_of(" \t\"") + 1);
    values_[key] = val;
  }
}

std::string AppEnvContext::get(const std::string& key)
{
  auto it = values_.find(key);
  return it == values_.end() ? "" : it->second;
}

std::optional<std::string> AppEnvContext::getOptional(const std::string& key)
{
  auto it = values_.find(key);
  if (it == values_.end())
    return std::nullopt;
  return it->second;
}
