#include "person-schema.hxx"

namespace
{

PersonStatus readStatus(const drogon::orm::Row& row)
{
  try {
    const auto field = row["status"];
    return field.isNull() ? PersonStatus::Known
                          : personStatusFromString(field.as<std::string>());
  }
  catch (...) {
    return PersonStatus::Known;
  }
}

template <typename T> T optionalColumn(const drogon::orm::Row& row,
                                       const char* column, T fallback)
{
  try {
    const auto field = row[column];
    return field.isNull() ? fallback : field.as<T>();
  }
  catch (...) {
    return fallback;
  }
}

}

PersonSchema::PersonSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  if (!row["user_id"].isNull())
    userId = static_cast<int64_t>(row["user_id"].as<long long>());
  name = row["name"].as<std::string>();
  alias = row["alias"].as<std::string>();
  observation = row["observation"].as<std::string>();
  status = readStatus(row);
  category = personCategoryFromString(
                 optionalColumn<std::string>(row, "category", ""))
                 .value_or(PersonCategory::None);
  note = optionalColumn<std::string>(row, "note", "");
  if (const auto number = optionalColumn<int64_t>(row, "visitor_number", 0);
      number > 0)
    visitorNumber = number;
  visitCount = optionalColumn<int64_t>(row, "visit_count", 0);
  firstSeenAt = static_cast<int64_t>(row["first_seen_at"].as<long long>());
  lastSeenAt = static_cast<int64_t>(row["last_seen_at"].as<long long>());
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
  if (!row["updated_at"].isNull())
    updatedAt = static_cast<int64_t>(row["updated_at"].as<long long>());
  if (!row["deleted_at"].isNull())
    deletedAt = static_cast<int64_t>(row["deleted_at"].as<long long>());
}

Json::Value PersonSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["userId"] = userId ? Json::Value(static_cast<Json::Int64>(*userId)) : Json::Value();
  json["name"] = name;
  json["alias"] = alias;
  json["observation"] = observation;
  json["status"] = personStatusToString(status);
  json["category"] = std::string(personCategoryToString(category));
  json["note"] = note;
  json["visitorNumber"] =
      visitorNumber ? Json::Value(static_cast<Json::Int64>(*visitorNumber)) : Json::Value();
  json["visitCount"] = static_cast<Json::Int64>(visitCount);
  json["firstSeenAt"] = static_cast<Json::Int64>(firstSeenAt);
  json["lastSeenAt"] = static_cast<Json::Int64>(lastSeenAt);
  json["createdAt"] = static_cast<Json::Int64>(createdAt);
  json["updatedAt"] = updatedAt ? Json::Value(static_cast<Json::Int64>(*updatedAt)) : Json::Value();
  json["deletedAt"] = deletedAt ? Json::Value(static_cast<Json::Int64>(*deletedAt)) : Json::Value();
  return json;
}
