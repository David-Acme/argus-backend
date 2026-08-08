#pragma once

#include <cstdint>
#include <shared/services/tapo/tapo-transport.hxx>
#include <string>
#include <vector>

class SecurePassthroughTransport : public ITapoTransport
{
public:
  explicit SecurePassthroughTransport(TapoCredentials credentials);

  TapoResult login() override;
  TapoResult request(const Json::Value& payload) override;
  TapoTransportKind kind() const override;
  Json::Value state() const override;
  bool isAuthenticated() const override;

private:
  struct HandshakeData
  {
    std::string nonce;
    std::string deviceConfirm;
  };

  TapoResult postPlain(const Json::Value& payload, Json::Value& out);
  bool fetchHandshake(HandshakeData& handshake, TapoResult& failure);
  bool resolveHashAlgorithm(const HandshakeData& handshake);
  void deriveKeys(const std::string& nonce);
  TapoResult sendEncrypted(const Json::Value& payload);

  TapoCredentials credentials_;
  std::string cnonce_;
  std::string nonce_;
  std::string hashedPassword_;
  std::string stok_;
  std::vector<uint8_t> lsk_;
  std::vector<uint8_t> ivb_;
  TapoHashAlgorithm hashAlgorithm_{TapoHashAlgorithm::Sha256};
  int64_t seq_{0};
  bool authenticated_{false};
};
