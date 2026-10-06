#include "uninstall-module-dto.hxx"

#include <auth/user-role.hxx>
#include <validation/validation_dsl.hxx>

#include <algorithm>
#include <charconv>
#include <set>

namespace
{
constexpr std::size_t kMaxPinLength = 32;
constexpr std::size_t kMaxReassignments = 64;

std::optional<std::string> shapeProblem(const UninstallModuleDto& dto)
{
  if (!dto.wellFormed)
    return "keepData must be true or false and pin a string of digits";
  return std::nullopt;
}

std::optional<std::string> reassignProblem(const UninstallModuleDto& dto)
{
  if (!dto.reassignWellFormed)
    return "reassign must map a user id to the role that person keeps";
  return std::nullopt;
}

bool readReassign(const Json::Value& value, std::vector<RoleReassignment>& out)
{
  if (!value.isObject() || value.size() > kMaxReassignments)
    return false;
  std::set<std::int64_t> seen;
  for (const auto& key : value.getMemberNames()) {
    std::int64_t userId = 0;
    const auto parsed = std::from_chars(key.data(), key.data() + key.size(), userId);
    const auto role = value[key].isString() ? parseUserRole(value[key].asString()) : std::nullopt;
    if (parsed.ec != std::errc() || parsed.ptr != key.data() + key.size() || userId <= 0 || !role ||
        *role == UserRole::Owner || !seen.insert(userId).second)
      return false;
    out.push_back({.userId = userId, .role = value[key].asString()});
  }
  return true;
}
}

UninstallModuleDto UninstallModuleDto::fromJson(const Json::Value& json)
{
  UninstallModuleDto dto;
  if (json.isObject() && json.isMember("keepData")) {
    if (json["keepData"].isBool())
      dto.keepData = json["keepData"].asBool();
    else
      dto.wellFormed = false;
  }
  if (json.isObject() && json.isMember("pin") && !json["pin"].isNull()) {
    if (json["pin"].isString() && std::ranges::all_of(json["pin"].asString(), [](char c) { return c >= '0' && c <= '9'; }))
      dto.pin = json["pin"].asString();
    else
      dto.wellFormed = false;
  }

  if (json.isObject() && json.isMember("reassign") && !json["reassign"].isNull())
    dto.reassignWellFormed = readReassign(json["reassign"], dto.reassign);

  START_VALIDATION(UninstallModuleDto, dto)
  CUSTOM_LAMBDA(keepData, shapeProblem)
  CUSTOM_LAMBDA(reassign, reassignProblem)
  MAX_LENGTH_OPTIONAL(pin, kMaxPinLength)
  END_VALIDATION()
  return dto;
}
