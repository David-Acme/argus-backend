#pragma once

#include <cstdint>
#include <json/value.h>
#include <shared/enums.hxx>
#include <string>

struct ResponseInvitationResolveDto
{
  UserRole role{UserRole::Guest};
  int64_t expiresAt{0};
  std::string instanceId;
  std::string caFingerprint;
  std::string serverFingerprint;
  std::string caPem;
  std::string scheme;
  int port{7024};

  Json::Value toJson() const;
};
