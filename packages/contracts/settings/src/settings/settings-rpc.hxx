#pragma once

#include <config/settings-registry.hxx>
#include <grpc/grpc-server-identity.hxx>
#include <settings/component-host.hxx>
#include <settings/module-data-host.hxx>
#include <settings/module-impact-host.hxx>
#include <settings/module-request-host.hxx>
#include <settings/owner-pin-host.hxx>
#include <settings/role-reassign-host.hxx>
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
  grpc::ServerUnaryReactor* ComponentStates(grpc::CallbackServerContext* context,
                                            const argus::settings::v1::ComponentStatesRequest* request,
                                            argus::settings::v1::ComponentStatesResponse* response) override;
  grpc::ServerUnaryReactor* InstallComponent(grpc::CallbackServerContext* context,
                                             const argus::settings::v1::ComponentRequest* request,
                                             argus::settings::v1::ComponentResponse* response) override;
  grpc::ServerUnaryReactor* CancelComponent(grpc::CallbackServerContext* context,
                                            const argus::settings::v1::ComponentRequest* request,
                                            argus::settings::v1::ComponentResponse* response) override;
  grpc::ServerUnaryReactor* RemoveComponent(grpc::CallbackServerContext* context,
                                             const argus::settings::v1::ComponentRequest* request,
                                             argus::settings::v1::ComponentResponse* response) override;

  grpc::ServerUnaryReactor* ModuleDataSummary(grpc::CallbackServerContext* context,
                                              const argus::settings::v1::ModuleDataRequest* request,
                                              argus::settings::v1::ModuleDataSummaryResponse* response) override;
  grpc::ServerUnaryReactor* PurgeModuleData(grpc::CallbackServerContext* context,
                                            const argus::settings::v1::ModuleDataRequest* request,
                                            argus::settings::v1::PurgeModuleDataResponse* response) override;

  void attachComponents(ComponentHost& host);
  grpc::ServerUnaryReactor* VerifyOwnerPin(grpc::CallbackServerContext* context,
                                           const argus::settings::v1::VerifyOwnerPinRequest* request,
                                           argus::settings::v1::VerifyOwnerPinResponse* response) override;

  grpc::ServerUnaryReactor* ModuleImpact(grpc::CallbackServerContext* context,
                                         const argus::settings::v1::ModuleImpactRequest* request,
                                         argus::settings::v1::ModuleImpactResponse* response) override;
  grpc::ServerUnaryReactor* ReassignRoles(grpc::CallbackServerContext* context,
                                          const argus::settings::v1::ReassignRolesRequest* request,
                                          argus::settings::v1::ReassignRolesResponse* response) override;
  grpc::ServerUnaryReactor* RequestModule(grpc::CallbackServerContext* context,
                                          const argus::settings::v1::RequestModuleRequest* request,
                                          argus::settings::v1::RequestModuleResponse* response) override;

  void attachModuleData(ModuleDataHost& host);
  void attachOwnerPin(OwnerPinHost& host);
  void attachModuleImpact(const ModuleImpactHost& host);
  void attachRoleReassign(RoleReassignHost& host);
  void attachModuleRequest(ModuleRequestHost& host);

private:
  using ComponentAction = ComponentStatus (ComponentHost::*)(const ComponentSpec&);

  grpc::ServerUnaryReactor* componentCall(grpc::CallbackServerContext* context,
                                          const argus::settings::v1::ComponentRequest& request,
                                          argus::settings::v1::ComponentResponse& response,
                                          ComponentAction action);

  void fillCatalog(argus::settings::v1::SettingsCatalog& catalog) const;

  SettingsRpcInput input_;
  ComponentHost* components_{nullptr};
  ModuleDataHost* moduleData_{nullptr};
  OwnerPinHost* ownerPin_{nullptr};
  const ModuleImpactHost* moduleImpact_{nullptr};
  RoleReassignHost* roleReassign_{nullptr};
  ModuleRequestHost* moduleRequest_{nullptr};
};
