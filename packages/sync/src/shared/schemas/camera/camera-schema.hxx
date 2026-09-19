#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <shared/enums.hxx>
#include <string>

// Camera row schema; cloudPassword stays server-side, never sent to a client.
struct CameraSchema
{
  int64_t id{0};
  std::string name;
  std::string manufacturer;
  std::string model;
  std::string ip;
  int32_t port{554};
  std::string username;
  std::string password;
  std::string cloudUsername;
  std::string cloudPassword;
  CameraDriver driver{CameraDriver::Tapo};
  std::string icon{"video"};
  CameraRecordMode recordMode{CameraRecordMode::Events};
  std::optional<int64_t> retentionDays;
  std::string capabilities;
  std::string config;
  bool isEnabled{true};
  bool isOnline{false};
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;
  std::optional<int64_t> deletedAt;

  CameraSchema() = default;
  explicit CameraSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
