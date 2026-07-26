#include "person-event-schema.hxx"

PersonEventSchema::PersonEventSchema(const drogon::orm::Row& row)
{
  personId = static_cast<int64_t>(row["person_id"].as<long long>());
  eventId = static_cast<int64_t>(row["event_id"].as<long long>());
  confidence = row["confidence"].as<double>();
}

Json::Value PersonEventSchema::toJson() const
{
  Json::Value json;
  json["personId"] = personId;
  json["eventId"] = eventId;
  json["confidence"] = confidence;
  return json;
}
