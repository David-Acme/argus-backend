#include "context-note-schema.hxx"

ContextNoteSchema::ContextNoteSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  if (!row["created_by"].isNull())
    createdBy = static_cast<int64_t>(row["created_by"].as<long long>());
  title = row["title"].as<std::string>();
  content = row["content"].as<std::string>();
  tags = row["tags"].as<std::string>();
  if (!row["valid_from"].isNull())
    validFrom = static_cast<int64_t>(row["valid_from"].as<long long>());
  if (!row["valid_until"].isNull())
    validUntil = static_cast<int64_t>(row["valid_until"].as<long long>());
  isActive = row["is_active"].as<int>() != 0;
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
  if (!row["updated_at"].isNull())
    updatedAt = static_cast<int64_t>(row["updated_at"].as<long long>());
  if (!row["deleted_at"].isNull())
    deletedAt = static_cast<int64_t>(row["deleted_at"].as<long long>());
}

Json::Value ContextNoteSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["createdBy"] = createdBy ? Json::Int64(*createdBy) : Json::nullValue;
  json["title"] = title;
  json["content"] = content;
  json["tags"] = tags;
  json["validFrom"] = validFrom ? Json::Int64(*validFrom) : Json::nullValue;
  json["validUntil"] = validUntil ? Json::Int64(*validUntil) : Json::nullValue;
  json["isActive"] = isActive;
  json["createdAt"] = Json::Int64(createdAt);
  json["updatedAt"] = updatedAt ? Json::Int64(*updatedAt) : Json::nullValue;
  json["deletedAt"] = deletedAt ? Json::Int64(*deletedAt) : Json::nullValue;
  return json;
}
