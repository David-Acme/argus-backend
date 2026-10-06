#include "module-role-move.hxx"

#include <charconv>
#include <string>

namespace
{
constexpr std::string_view kMovedKey = ";moved=";
constexpr char kEntrySeparator = ',';
constexpr char kUserSeparator = ':';
constexpr std::string_view kRoleArrow = ">";

std::vector<std::string_view> split(std::string_view text, char separator)
{
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (start <= text.size()) {
    const auto end = text.find(separator, start);
    parts.push_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
  return parts;
}

std::string joinedNames(const std::vector<ModuleRoleMove>& moves)
{
  std::string names;
  for (const auto& move : moves) {
    if (!names.empty())
      names += ", ";
    names += move.name.empty() ? "#" + std::to_string(move.userId) : move.name;
  }
  return names;
}
}

std::string module_role_move::detailOf(const ModuleAuditDetail& detail)
{
  std::string text = detail.data;
  if (detail.moves.empty())
    return text;
  text += kMovedKey;
  bool first = true;
  for (const auto& move : detail.moves) {
    if (!first)
      text += kEntrySeparator;
    first = false;
    text += std::to_string(move.userId) + kUserSeparator + move.from + std::string(kRoleArrow) + move.to;
  }
  return text;
}

ModuleAuditDetail module_role_move::parse(std::string_view detail)
{
  ModuleAuditDetail parsed;
  const auto marker = detail.find(kMovedKey);
  parsed.data = std::string(detail.substr(0, marker));
  if (marker == std::string_view::npos)
    return parsed;
  for (const auto entry : split(detail.substr(marker + kMovedKey.size()), kEntrySeparator)) {
    const auto colon = entry.find(kUserSeparator);
    const auto arrow = entry.find(kRoleArrow);
    if (colon == std::string_view::npos || arrow == std::string_view::npos || arrow < colon)
      continue;
    std::int64_t userId = 0;
    const auto id = entry.substr(0, colon);
    if (std::from_chars(id.data(), id.data() + id.size(), userId).ec != std::errc())
      continue;
    parsed.moves.push_back({.userId = userId,
                            .name = {},
                            .from = std::string(entry.substr(colon + 1, arrow - colon - 1)),
                            .to = std::string(entry.substr(arrow + kRoleArrow.size()))});
  }
  return parsed;
}

ModuleRoleMoveNote module_role_move::noteOf(const std::vector<ModuleRoleMove>& moves)
{
  const bool many = moves.size() > 1;
  const auto names = joinedNames(moves);
  return {.es = "La desinstalación no terminó. " + names + (many ? " ya tienen su nuevo rol y lo conservan" : " ya tiene su nuevo rol y lo conserva") +
                "; el módulo sigue instalado.",
          .en = "The uninstall did not finish. " + names + (many ? " already have their new role and keep it" : " already has the new role and keeps it") +
                "; the module is still installed."};
}
