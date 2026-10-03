#pragma once

#include <config/settings-registry.hxx>
#include <grpc/grpc-server-identity.hxx>
#include <settings.grpc.pb.h>

#include <string>
#include <utility>
#include <vector>

inline constexpr const char* kSettingsCaller = "settings";

std::vector<argus::client::CallerCredential> settingsCallers(
    const std::vector<std::pair<std::string, std::string>>& callers);

void withoutSettingsCaller(std::vector<std::pair<std::string, std::string>>& callers);

struct SettingsRpcInput
{
  std::string service;
  SettingsRegistry* registry{nullptr};
  std::vector<argus::client::CallerCredential> credentials;
};

class SettingsRpcService final : public argus::settings::v1::Settings::CallbackService
{
public:
  explicit SettingsRpcService(SettingsRpcInput input);

  grpc::ServerUnaryReactor* List(grpc::CallbackServerContext* context,
                                 const argus::settings::v1::ListSettingsRequest* request,
                                 argus::settings::v1::SettingsCatalog* response) override;
  grpc::ServerUnaryReactor* Update(grpc::CallbackServerContext* context,
                                   const argus::settings::v1::UpdateSettingsRequest* request,
                                   argus::settings::v1::UpdateSettingsResponse* response) override;

private:
  void fillCatalog(argus::settings::v1::SettingsCatalog& catalog) const;

  SettingsRpcInput input_;
};
