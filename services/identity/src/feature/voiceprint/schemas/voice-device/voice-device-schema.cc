#include "voice-device-schema.hxx"

#include <drogon/orm/Field.h>

VoiceDeviceSchema::VoiceDeviceSchema(const drogon::orm::Row& row)
{
  deviceHash = row["device_hash"].as<std::string>();
  userId = row["user_id"].as<int64_t>();
  calls = row["calls"].as<int>();
  matched = row["matched"].as<int>();
  conflicting = row["conflicting"].as<int>();
  mixed = row["mixed"].as<int>();
  lastCallAt = row["last_call_at"].as<int64_t>();
}
