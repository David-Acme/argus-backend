#include "refresh-token-schema.hxx"

RefreshTokenSchema::RefreshTokenSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  userId = static_cast<int64_t>(row["user_id"].as<long long>());
  accessToken = row["access_token"].as<std::string>();
  refreshToken = row["refresh_token"].as<std::string>();
  deviceHash = row["device_hash"].as<std::string>();
  userAgent = row["user_agent"].as<std::string>();
  isValid = row["is_valid"].as<int>() != 0;
  isUsed = row["is_used"].as<int>() != 0;
  expiresAt = static_cast<int64_t>(row["expires_at"].as<long long>());
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
}

Json::Value RefreshTokenSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["userId"] = userId;
  json["deviceHash"] = deviceHash;
  json["userAgent"] = userAgent;
  json["isValid"] = isValid;
  json["isUsed"] = isUsed;
  json["expiresAt"] = Json::Int64(expiresAt);
  json["createdAt"] = Json::Int64(createdAt);
  return json;
}
