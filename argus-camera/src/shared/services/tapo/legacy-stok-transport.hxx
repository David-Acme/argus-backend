#pragma once

#include <shared/services/tapo/tapo-transport.hxx>
#include <string>

class LegacyStokTransport : public ITapoTransport
{
public:
  explicit LegacyStokTransport(TapoCredentials credentials);

  TapoResult login() override;
  TapoResult request(const Json::Value& payload) override;
  TapoTransportKind kind() const override;
  Json::Value state() const override;
  bool isAuthenticated() const override;

private:
  TapoResult send(const Json::Value& payload);

  TapoCredentials credentials_;
  std::string stok_;
  bool authenticated_{false};
};
