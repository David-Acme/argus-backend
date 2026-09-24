#pragma once

#include <feature/transport/infra/identity-sync-source.hxx>
#include <identity/identity-sync-client.hxx>
#include <memory>
#include <string>

class IdentitySyncGateway : public IdentitySyncSource
{
public:
  explicit IdentitySyncGateway(IdentitySyncClientConfig config);

  IdentitySyncGateway(const IdentitySyncGateway&) = delete;
  IdentitySyncGateway& operator=(const IdentitySyncGateway&) = delete;

  [[nodiscard]] bool serves(IdentitySyncTable table) const override;
  [[nodiscard]] std::unique_ptr<Syncable>
  sourceFor(IdentitySyncTable table, const JwtContext& ctx) const override;

private:
  class Pull;

  std::shared_ptr<IdentitySyncClient> client_;
};
