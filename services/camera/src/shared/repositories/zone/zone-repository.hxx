#pragma once

#include "zone-query.hxx"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <sync/syncable.hxx>
#include <shared/schemas/zone/zone-schema.hxx>
#include <vector>

class ZoneRepository : public Syncable
{
public:
  ZoneRepository() = default;
  ~ZoneRepository() override = default;

  drogon::Task<std::optional<ZoneSchema>>
  findById(int64_t id, drogon::orm::DbClient* client = nullptr) const;
  drogon::Task<std::vector<ZoneSchema>> findByCamera(int64_t cameraId) const;
  drogon::Task<ZoneSchema> create(const ZoneCreateInput& input) const;
  drogon::Task<ZoneSchema> update(int64_t id,
                                  const ZoneUpdateInput& input) const;
  drogon::Task<bool> remove(int64_t id, drogon::orm::DbClient* client) const;
  static bool acceptPrivacyZones();

  drogon::Task<std::vector<Json::Value>>
  find(const SyncFilter& filter) const override;
  drogon::Task<std::vector<Json::Value>>
  findDeleted(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>>
  findLast(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>>
  findLastDeleted(const SyncFilter& filter) const override;
};
