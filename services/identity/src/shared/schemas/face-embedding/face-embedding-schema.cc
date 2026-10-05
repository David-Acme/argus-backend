#include "face-embedding-schema.hxx"

FaceEmbeddingSchema::FaceEmbeddingSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  personId = static_cast<int64_t>(row["person_id"].as<long long>());
  embedding = row["embedding"].as<std::string>();
  angleLabel = row["angle_label"].as<std::string>();
  quality = row["quality"].as<double>();
  model = row["model"].as<std::string>();
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
}

Json::Value FaceEmbeddingSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["personId"] = personId;
  json["angleLabel"] = angleLabel;
  json["quality"] = quality;
  json["model"] = model;
  json["createdAt"] = Json::Int64(createdAt);
  return json;
}
