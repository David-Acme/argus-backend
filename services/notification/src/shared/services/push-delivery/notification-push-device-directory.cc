#include "notification-push-device-directory.hxx"

drogon::Task<std::vector<PushDevice>>
NotificationPushDeviceDirectory::devicesOf(int64_t userId, drogon::orm::DbClient* client) const
{
  const auto rows = co_await repository_.findPushByUser(userId, client);
  std::vector<PushDevice> devices;
  devices.reserve(rows.size());
  for (const auto& row : rows) {
    const auto provider = pushProviderFromString(row.provider);
    if (!provider)
      continue;
    devices.push_back({.provider = *provider,
                       .token = row.token,
                       .publicKey = row.publicKey,
                       .keyId = row.keyId});
  }
  co_return devices;
}
