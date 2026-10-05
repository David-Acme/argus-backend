#include "update-duty-dto.hxx"

#include <validation/validation_dsl.hxx>

UpdateDutyDto UpdateDutyDto::fromJson(const Json::Value& json)
{
  UpdateDutyDto dto;
  dto.typed = json["onDuty"].isBool();
  dto.onDuty = dto.typed && json["onDuty"].asBool();

  START_VALIDATION(UpdateDutyDto, dto)
  CUSTOM_LAMBDA(onDuty,
                [](const UpdateDutyDto& value) -> std::optional<std::string> {
                  if (value.typed)
                    return std::nullopt;
                  return "must be true or false";
                })
  END_VALIDATION()
  return dto;
}
