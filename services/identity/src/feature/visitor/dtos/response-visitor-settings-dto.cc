#include "response-visitor-settings-dto.hxx"

Json::Value ResponseVisitorSettingsDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["recognitionEnabled"] = recognitionEnabled;
  json["unnamedRetentionDays"] = static_cast<Json::Int64>(setting.unnamedRetentionDays);
  json["minRetentionDays"] = static_cast<Json::Int64>(kMinUnnamedRetentionDays);
  json["maxRetentionDays"] = static_cast<Json::Int64>(kMaxUnnamedRetentionDays);
  json["updatedAt"] =
      setting.updatedAt > 0 ? Json::Value(static_cast<Json::Int64>(setting.updatedAt)) : Json::Value();
  return json;
}
