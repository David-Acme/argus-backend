#include "crop-capability-repository.hxx"

#include <sqlite/db-service.hxx>

using namespace crop_capability_query;

drogon::Task<void>
CropCapabilityRepository::create(const CropCapabilityCreateInput& input) const
{
  co_await DbService::client()->execSqlCoro(
      std::string(INSERT), input.tokenHash, input.personId, input.sampleId,
      input.requesterUserId, input.expiresAt);
}

drogon::Task<std::optional<ConsumedCropCapability>>
CropCapabilityRepository::consume(const CropCapabilityConsumeInput& input) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(
      std::string(CONSUME), input.now, input.tokenHash, input.requesterUserId);
  if (rows.empty())
    co_return std::nullopt;
  co_return ConsumedCropCapability{
      .personId = rows.front()["person_id"].as<int64_t>(),
      .sampleId = rows.front()["face_embedding_id"].as<int64_t>()};
}

drogon::Task<void> CropCapabilityRepository::purgeExpired(int64_t now) const
{
  co_await DbService::client()->execSqlCoro(std::string(PURGE_EXPIRED), now);
}
