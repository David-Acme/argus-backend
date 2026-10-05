#include "pairing-dto.hxx"

#include <feature/pairing/infra/pairing-code.hxx>

#include <algorithm>
#include <cctype>

namespace
{
bool isHex(const std::string& value)
{
  return std::ranges::all_of(value, [](char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
  });
}
}

PairingDto PairingDto::fromJson(const Json::Value& json)
{
  PairingDto dto;
  dto.code = json.get("code", "").asString();
  dto.nonce = json.get("nonce", "").asString();
  dto.proof = json.get("proof", "").asString();

  START_VALIDATION(PairingDto, dto)
  CUSTOM_LAMBDA(code, [](const PairingDto& value) -> std::optional<std::string> {
    if (!value.proof.empty())
      return std::nullopt;
    if (!pairing_code::wellFormed(pairing_code::normalize(value.code)))
      return "code must be the server's pairing code";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(nonce, [](const PairingDto& value) -> std::optional<std::string> {
    if (value.proof.empty())
      return std::nullopt;
    if (value.nonce.size() < 32 || value.nonce.size() > 64 || !isHex(value.nonce))
      return "nonce must be 32 to 64 hex characters";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(proof, [](const PairingDto& value) -> std::optional<std::string> {
    if (value.proof.empty())
      return std::nullopt;
    if (value.proof.size() != 64 || !isHex(value.proof))
      return "proof must be 64 hex characters";
    return std::nullopt;
  })
  END_VALIDATION()

  return dto;
}
