#pragma once

#include <json/value.h>
#include <optional>
#include <shared/validation/validation_dsl.hxx>
#include <string>
#include <vector>

struct SynchronizedRangeDto
{
  std::optional<int64_t> startTime;
  std::optional<int64_t> endTime;

  static SynchronizedRangeDto fromJson(const Json::Value& json)
  {
    SynchronizedRangeDto dto;
    if (json.isMember("startTime") && json["startTime"].isInt64())
      dto.startTime = json["startTime"].asInt64();
    if (json.isMember("endTime") && !json["endTime"].isNull() &&
        json["endTime"].isInt64())
      dto.endTime = json["endTime"].asInt64();

    START_VALIDATION(SynchronizedRangeDto, dto)
    IS_POSITIVE_TIMESTAMP_OPTIONAL(startTime)
    IS_POSITIVE_TIMESTAMP_OPTIONAL(endTime)
    CUSTOM_LAMBDA(startTime,
                  [](const SynchronizedRangeDto& d)
                      -> std::optional<std::string> {
                    if (d.startTime && d.endTime &&
                        *d.startTime >= *d.endTime)
                      return "startTime must be less than endTime";
                    return std::nullopt;
                  })
    END_VALIDATION()
    return dto;
  }
};

struct SynchronizedBodyDto
{
  std::optional<SynchronizedRangeDto> created;
  std::optional<SynchronizedRangeDto> deleted;
  bool findLastCreated{false};
  bool findLastDeleted{false};
  bool requiredCreate{false};
  bool requiredDeleted{false};

  static SynchronizedBodyDto fromJson(const Json::Value& json)
  {
    SynchronizedBodyDto dto;

    if (json.isMember("created") && !json["created"].isNull() &&
        json["created"].isObject())
      dto.created = SynchronizedRangeDto::fromJson(json["created"]);

    if (json.isMember("deleted") && !json["deleted"].isNull() &&
        json["deleted"].isObject())
      dto.deleted = SynchronizedRangeDto::fromJson(json["deleted"]);

    dto.findLastCreated = json.get("findLastCreated", false).asBool();
    dto.findLastDeleted = json.get("findLastDeleted", false).asBool();
    dto.requiredCreate = json.get("requiredCreate", false).asBool();
    dto.requiredDeleted = json.get("requiredDeleted", false).asBool();

    START_VALIDATION(SynchronizedBodyDto, dto)
    IS_BOOLEAN(findLastCreated)
    IS_BOOLEAN(findLastDeleted)
    IS_BOOLEAN(requiredCreate)
    IS_BOOLEAN(requiredDeleted)
    END_VALIDATION()
    return dto;
  }
};

struct SynchronizedDto
{
  std::optional<SynchronizedBodyDto> user;
  std::optional<SynchronizedBodyDto> camera;
  std::optional<SynchronizedBodyDto> cameraStream;
  std::optional<SynchronizedBodyDto> zone;
  std::optional<SynchronizedBodyDto> reminder;
  std::optional<SynchronizedBodyDto> reminderDetail;
  std::optional<SynchronizedBodyDto> notification;

  static SynchronizedDto fromJson(const Json::Value& json)
  {
    using BodyField = std::optional<SynchronizedBodyDto> SynchronizedDto::*;

    static const std::vector<std::pair<std::string, BodyField>> kBodyFields = {
        {"user", &SynchronizedDto::user},
        {"camera", &SynchronizedDto::camera},
        {"camera_stream", &SynchronizedDto::cameraStream},
        {"zone", &SynchronizedDto::zone},
        {"reminder", &SynchronizedDto::reminder},
        {"reminder_detail", &SynchronizedDto::reminderDetail},
        {"notification", &SynchronizedDto::notification},
    };

    SynchronizedDto dto;
    for (const auto& [jsonKey, member] : kBodyFields) {
      if (json.isMember(jsonKey) && !json[jsonKey].isNull() &&
          json[jsonKey].isObject())
        dto.*member = SynchronizedBodyDto::fromJson(json[jsonKey]);
    }
    return dto;
  }
};

struct SynchronizedLogDto
{
  std::optional<int64_t> startTime;
  std::optional<int64_t> endTime;
  bool findLast{false};

  static SynchronizedLogDto fromJson(const Json::Value& json)
  {
    SynchronizedLogDto dto;
    if (json.isMember("startTime") && json["startTime"].isInt64())
      dto.startTime = json["startTime"].asInt64();
    if (json.isMember("endTime") && !json["endTime"].isNull() &&
        json["endTime"].isInt64())
      dto.endTime = json["endTime"].asInt64();
    dto.findLast = json.get("findLast", false).asBool();

    START_VALIDATION(SynchronizedLogDto, dto)
    IS_POSITIVE_TIMESTAMP_OPTIONAL(startTime)
    IS_POSITIVE_TIMESTAMP_OPTIONAL(endTime)
    IS_BOOLEAN(findLast)
    CUSTOM_LAMBDA(startTime,
                  [](const SynchronizedLogDto& d)
                      -> std::optional<std::string> {
                    if (d.startTime && d.endTime &&
                        *d.startTime >= *d.endTime)
                      return "startTime must be less than endTime";
                    return std::nullopt;
                  })
    END_VALIDATION()
    return dto;
  }
};
