#include "update-camera-context-dto.hxx"

#include <feature/guard/guard-schedule.hxx>
#include <validation/validation_dsl.hxx>

namespace
{
constexpr int kMaxWindowsLength = 200;
}

UpdateCameraContextDto UpdateCameraContextDto::fromJson(const Json::Value& json)
{
  UpdateCameraContextDto dto;
  dto.role = json.get("role", "").asString();
  dto.outdoor = json.get("outdoor", false).asBool();
  dto.publicArea = json.get("publicArea", false).asBool();
  dto.activeHours = json.get("activeHours", "").asString();

  START_VALIDATION(UpdateCameraContextDto, dto)
  IS_NOT_EMPTY(role)
  IS_IN(role, "other", "entrance", "perimeter", "garage", "living", "kitchen",
        "office", "register", "storage", "public_area")
  MAX_LENGTH(activeHours, kMaxWindowsLength)
  CUSTOM_LAMBDA(activeHours,
                [](const UpdateCameraContextDto& value)
                    -> std::optional<std::string> {
                  if (guard_schedule::validWindows(value.activeHours))
                    return std::nullopt;
                  return "must look like \"mon-fri 08:00-18:00\"";
                })
  END_VALIDATION()
  return dto;
}
