#pragma once

#include <json/value.h>
#include <string>

struct ResponsePairingDto
{
  std::string instanceId;
  std::string caFingerprint;
  std::string serverFingerprint;
  std::string caPem;
  std::string scheme;
  int port = 7024;

  Json::Value toJson() const;
};
