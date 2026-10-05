#include "biometric-erase-repository.hxx"

using namespace biometric_erase_query;

drogon::Task<ErasedBiometrics>
BiometricEraseRepository::erase(const BiometricEraseInput& input) const
{
  auto* client = input.client;
  const int64_t userId = input.userId;
  ErasedBiometrics erased;

  const auto embeddings = co_await client->execSqlCoro(std::string(EMBEDDINGS), userId);
  erased.embeddingIds.reserve(embeddings.size());
  for (const auto& row : embeddings) {
    erased.embeddingIds.push_back(row["id"].as<int64_t>());
    auto key = row["crop_key"].as<std::string>();
    if (!key.empty())
      erased.objectKeys.push_back(std::move(key));
  }

  const auto portraits = co_await client->execSqlCoro(std::string(PORTRAIT_KEYS), userId);
  erased.portraits = portraits.size();
  for (const auto& row : portraits)
    erased.objectKeys.push_back(row["object_key"].as<std::string>());

  co_await client->execSqlCoro(std::string(RETIRE_PORTRAIT_FILES), userId);
  co_await client->execSqlCoro(std::string(DELETE_PORTRAIT_LINK), userId);
  co_await client->execSqlCoro(std::string(DELETE_PORTRAIT_CAPABILITIES), userId);
  co_await client->execSqlCoro(std::string(DELETE_CROP_CAPABILITIES), userId);
  co_await client->execSqlCoro(std::string(DELETE_SNAPSHOTS), userId);
  co_await client->execSqlCoro(std::string(DELETE_EMBEDDINGS), userId);
  const auto privacy = co_await client->execSqlCoro(std::string(DELETE_PRIVACY), userId);
  erased.hadPrivacy = privacy.affectedRows() > 0;
  co_return erased;
}
