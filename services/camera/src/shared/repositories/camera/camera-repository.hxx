#pragma once

#include "camera-query.hxx"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <sync/syncable.hxx>
#include <shared/schemas/camera/camera-schema.hxx>
#include <vector>

class CameraRepository : public Syncable
{
public:
  CameraRepository() = default;
  ~CameraRepository() override = default;

  drogon::Task<std::optional<CameraSchema>>
  findById(int64_t id, drogon::orm::DbClient* client = nullptr) const;
  drogon::Task<std::vector<CameraSchema>> findEnabled() const;
  drogon::Task<std::vector<CameraSchema>> findLive() const;
  drogon::Task<CameraSchema> create(const CameraCreateInput& input) const;
  drogon::Task<CameraSchema> update(int64_t id,
                                    const CameraUpdateInput& input) const;
  drogon::Task<bool> remove(int64_t id, drogon::orm::DbClient* client) const;

  static bool acceptTapoTrust();
  static int64_t sealPlaintextSecrets();
  static void saveTapoTrust(const CameraTapoTrustInput& input);

  drogon::Task<std::vector<Json::Value>>
  find(const SyncFilter& filter) const override;
  drogon::Task<std::vector<Json::Value>>
  findDeleted(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>>
  findLast(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>>
  findLastDeleted(const SyncFilter& filter) const override;
};
