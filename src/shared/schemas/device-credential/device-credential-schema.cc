#include "device-credential-schema.hxx"

#include <drogon/orm/Field.h>

DeviceCredentialSchema::DeviceCredentialSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  userId = static_cast<int64_t>(row["user_id"].as<long long>());
  deviceHash = row["device_hash"].as<std::string>();
  secretHash = row["secret_hash"].as<std::string>();
  isActive = row["is_active"].as<int64_t>() != 0;
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
}
