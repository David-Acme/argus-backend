#include "config-service.hxx"

#include <drogon/drogon.h>
#include <json/reader.h>
#include <json/value.h>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <toml++/toml.hpp>
#include <vector>

namespace
{

std::optional<toml::table> gConfig;
std::string gConfigPath;
std::mutex gConfigMutex;

const toml::node* resolvePath(const std::string& keyPath)
{
  if (!gConfig)
    return nullptr;

  const toml::node* current = &*gConfig;
  std::stringstream ss(keyPath);
  std::string segment;
  while (std::getline(ss, segment, '.')) {
    if (!current->is_table())
      return nullptr;
    auto& tbl = *current->as_table();
    auto it = tbl.find(segment);
    if (it == tbl.end())
      return nullptr;
    current = &it->second;
  }
  return current;
}

template <typename T>
T getValue(const std::string& keyPath, T defaultVal)
{
  std::lock_guard lock(gConfigMutex);
  const auto* node = resolvePath(keyPath);
  if (!node)
    return defaultVal;
  if constexpr (std::is_same_v<T, int>)
    return static_cast<int>(node->value_or(static_cast<int64_t>(defaultVal)));
  else if constexpr (std::is_same_v<T, bool>)
    return static_cast<bool>(node->value_or(static_cast<bool>(defaultVal)));
  else
    return node->value_or(defaultVal);
}

std::string trim(const std::string& s)
{
  const auto start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos)
    return "";
  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

std::pair<std::string, std::string> splitSectionKey(const std::string& keyPath)
{
  const auto dot = keyPath.rfind('.');
  if (dot == std::string::npos)
    return {"", keyPath};
  return {keyPath.substr(0, dot), keyPath.substr(dot + 1)};
}

std::string escapeTomlString(const std::string& value)
{
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  for (const char c : value) {
    if (c == '\\' || c == '"')
      out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

// Patches `key = literal` inside `[section]`, preserving every other line
// (comments, blank lines, formatting). If the key is missing it is inserted
// right after the section header; if the section is missing it is appended.
std::string patchContent(const std::string& content, const std::string& section,
                         const std::string& key, const std::string& literal)
{
  std::vector<std::string> lines;
  std::istringstream ss(content);
  std::string line;
  while (std::getline(ss, line))
    lines.push_back(line);

  const std::string sectionHeader = "[" + section + "]";
  bool inSection = false;
  bool replaced = false;

  for (auto& l : lines) {
    const std::string t = trim(l);
    if (!t.empty() && t.front() == '[' && t.back() == ']') {
      inSection = (t == sectionHeader);
      continue;
    }
    if (!inSection || replaced)
      continue;
    const auto eq = l.find('=');
    if (eq == std::string::npos)
      continue;
    if (trim(l.substr(0, eq)) != key)
      continue;
    const auto lead = l.find_first_not_of(" \t");
    const std::string indent =
        lead == std::string::npos ? "" : l.substr(0, lead);
    l = indent + key + " = " + literal;
    replaced = true;
  }

  if (!replaced) {
    bool found = false;
    for (auto& l : lines) {
      if (trim(l) == sectionHeader) {
        l += "\n" + key + " = " + literal;
        found = true;
        break;
      }
    }
    if (!found)
      lines.push_back(sectionHeader + "\n" + key + " = " + literal);
  }

  std::ostringstream out;
  for (const auto& l : lines)
    out << l << '\n';
  return out.str();
}

bool applyValue(const std::string& keyPath, const std::string& literal)
{
  std::lock_guard lock(gConfigMutex);
  if (gConfigPath.empty())
    return false;

  std::ifstream in(gConfigPath);
  if (!in.is_open())
    return false;
  const std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());

  const auto [section, key] = splitSectionKey(keyPath);
  const std::string patched = patchContent(content, section, key, literal);

  try {
    gConfig = toml::parse(patched);
  }
  catch (const toml::parse_error&) {
    return false;
  }

  std::ofstream out(gConfigPath, std::ios::trunc);
  if (!out.is_open())
    return false;
  out << patched;
  return out.good();
}

} // namespace

void ConfigService::load(const std::string& path)
{
  try {
    std::lock_guard lock(gConfigMutex);
    gConfig = toml::parse_file(path);
    gConfigPath = path;
    LOG_INFO << "Configuration loaded from " << path;
  }
  catch (const toml::parse_error& e) {
    auto& src = e.source();
    std::string detail{e.description()};
    std::string filePath =
        src.path ? std::string{*src.path} : std::string("(unknown)");
    LOG_FATAL << "Failed to parse config " << path << ": " << detail << " at "
              << filePath << ":" << src.begin.line << ":" << src.begin.column;
    throw std::runtime_error("ConfigService: failed to parse " + path);
  }
}

std::string ConfigService::getString(const std::string& keyPath)
{
  return getValue<std::string>(keyPath, "");
}

int ConfigService::getInt(const std::string& keyPath)
{
  return getValue<int>(keyPath, 0);
}

bool ConfigService::getBool(const std::string& keyPath)
{
  return getValue<bool>(keyPath, false);
}

double ConfigService::getDouble(const std::string& keyPath)
{
  return getValue<double>(keyPath, 0.0);
}

bool ConfigService::setBool(const std::string& keyPath, bool value)
{
  return applyValue(keyPath, value ? "true" : "false");
}

bool ConfigService::setString(const std::string& keyPath,
                              const std::string& value)
{
  return applyValue(keyPath, escapeTomlString(value));
}

bool ConfigService::setInt(const std::string& keyPath, int value)
{
  return applyValue(keyPath, std::to_string(value));
}

bool ConfigService::setDouble(const std::string& keyPath, double value)
{
  return applyValue(keyPath, std::to_string(value));
}

std::vector<std::pair<std::string, std::string>>
ConfigService::getStringPairs(const std::string& keyPath)
{
  std::lock_guard lock(gConfigMutex);
  std::vector<std::pair<std::string, std::string>> result;
  const auto* node = resolvePath(keyPath);
  if (!node || !node->is_table())
    return result;

  for (const auto& [key, value] : *node->as_table()) {
    auto str = value.value<std::string>();
    if (str)
      result.emplace_back(key, std::move(*str));
  }
  return result;
}

Json::Value ConfigService::drogonConfig()
{
  std::lock_guard lock(gConfigMutex);
  if (!gConfig)
    throw std::runtime_error("ConfigService: not loaded");

  auto drogonNode = gConfig->at_path("drogon");
  auto* drogonTbl = drogonNode.as_table();
  if (!drogonTbl)
    return Json::Value{};

  std::stringstream ss;
  ss << toml::json_formatter{*drogonTbl};

  Json::Value root;
  Json::Reader reader;
  if (!reader.parse(ss.str(), root)) {
    LOG_ERROR << "Failed to parse Drogon config JSON: "
              << reader.getFormattedErrorMessages();
    return Json::Value{};
  }
  return root;
}
