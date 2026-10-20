#include "user-minor-schema.hxx"

#include <drogon/orm/Field.h>

UserMinorSchema::UserMinorSchema(const drogon::orm::Row& row)
    : userId(row["user_id"].as<int64_t>()),
      guardianConsent(row["guardian_consent"].as<int64_t>() != 0),
      setBy(row["set_by"].as<int64_t>()),
      setAt(row["set_at"].as<int64_t>())
{
  if (!row["guardian_id"].isNull())
    guardianId = row["guardian_id"].as<int64_t>();
}
