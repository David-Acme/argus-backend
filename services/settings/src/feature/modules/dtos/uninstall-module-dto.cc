#include "uninstall-module-dto.hxx"

#include <validation/validation_dsl.hxx>

#include <algorithm>

namespace
{
constexpr std::size_t kMaxPinLength = 32;

std::optional<std::string> shapeProblem(const UninstallModuleDto& dto)
{
  if (!dto.wellFormed)
    return "keepData must be true or false and pin a string of digits";
  return std::nullopt;
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

  START_VALIDATION(UninstallModuleDto, dto)
  CUSTOM_LAMBDA(keepData, shapeProblem)
  MAX_LENGTH_OPTIONAL(pin, kMaxPinLength)
  END_VALIDATION()
  return dto;
}
