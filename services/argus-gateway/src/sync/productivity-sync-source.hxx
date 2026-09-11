#pragma once

#include <memory>
#include <productivity/productivity-sync-client.hxx>
#include <shared/contracts/productivity-sync-source.hxx>

// Pull source backed by argus-productivity's argus.productivity.v1.SyncService.
class ProductivitySyncGateway : public ProductivitySyncSource
{
public:
  explicit ProductivitySyncGateway(std::string target);

  ProductivitySyncGateway(const ProductivitySyncGateway&) = delete;
  ProductivitySyncGateway& operator=(const ProductivitySyncGateway&) = delete;

  bool serves(ProductivitySyncTable table) const override;
  std::unique_ptr<Syncable>
  sourceFor(ProductivitySyncTable table, const JwtContext& ctx) const override;

private:
  class Pull;

  std::shared_ptr<ProductivitySyncClient> client_;
};
