#pragma once

#include <push/push-provider.hxx>

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <string>
#include <vector>

struct PushDevice
{
  PushProvider provider{PushProvider::Fcm};
  std::string token;
  std::string publicKey;
  std::string keyId;
};

class PushDeviceDirectory
{
public:
  virtual ~PushDeviceDirectory() = default;

  [[nodiscard]] virtual drogon::Task<std::vector<PushDevice>>
  devicesOf(int64_t userId, drogon::orm::DbClient* client = nullptr) const = 0;
};
