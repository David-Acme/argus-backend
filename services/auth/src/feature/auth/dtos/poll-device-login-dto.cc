#include "poll-device-login-dto.hxx"

PollDeviceLoginDto PollDeviceLoginDto::fromRequest(const drogon::HttpRequestPtr& request)
{
  PollDeviceLoginDto dto;
  dto.proof = request->getHeader(std::string(kProofHeader));

  START_VALIDATION(PollDeviceLoginDto, dto)
  MAX_LENGTH(proof, 128)
  END_VALIDATION()
  return dto;
}
