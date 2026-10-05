#pragma once

#include <feature/pairing/dtos/response-pairing-dto.hxx>
#include <feature/pairing/dtos/response-pairing-status-dto.hxx>
#include <shared/repositories/user/user-repository.hxx>

#include <drogon/utils/coroutine.h>
#include <string>

struct PairingRequestInput
{
  std::string code;
  std::string nonce;
  std::string proof;
  std::string deviceHash;
};

class PairingFeatureService
{
public:
  [[nodiscard]] drogon::Task<ResponsePairingDto> pair(PairingRequestInput input) const;
  [[nodiscard]] drogon::Task<ResponsePairingStatusDto> status() const;

private:
  UserRepository userRepository_;
};
