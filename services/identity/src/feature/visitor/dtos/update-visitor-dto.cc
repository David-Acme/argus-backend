#include "update-visitor-dto.hxx"

#include <validation/validation_dsl.hxx>

UpdateVisitorDto UpdateVisitorDto::fromJson(const Json::Value& json)
{
  UpdateVisitorDto dto;
  if (json.isMember("name") && json["name"].isString())
    dto.name = json["name"].asString();
  if (json.isMember("category") && json["category"].isString())
    dto.categoryValue = json["category"].asString();
  if (json.isMember("note") && json["note"].isString())
    dto.note = json["note"].asString();

  START_VALIDATION(UpdateVisitorDto, dto)
  MAX_LENGTH_OPTIONAL(name, 60)
  MAX_LENGTH_OPTIONAL(note, 280)
  IS_IN_OPTIONAL(categoryValue, "", "neighbor", "delivery", "service", "family",
                 "acquaintance", "watchlist")
  CUSTOM_LAMBDA(body, [](const UpdateVisitorDto& value)
                         -> std::optional<std::string> {
    if (!value.name && !value.categoryValue && !value.note)
      return "at least one editable field is required";
    return std::nullopt;
  })
  END_VALIDATION()
  if (dto.categoryValue)
    dto.category = personCategoryFromString(*dto.categoryValue);
  return dto;
}
