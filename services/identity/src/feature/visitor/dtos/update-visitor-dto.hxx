#pragma once

#include <identity/person-category.hxx>
#include <json/value.h>
#include <optional>
#include <string>

struct UpdateVisitorDto
{
  std::optional<std::string> name;
  std::optional<std::string> categoryValue;
  std::optional<std::string> note;
  std::optional<PersonCategory> category;

  static UpdateVisitorDto fromJson(const Json::Value& json);
};
