#pragma once

#include <grpc/fleet-caller-gate.hxx>
#include <settings.grpc.pb.h>
#include <settings/component-vocabulary.hxx>

#include <functional>
#include <memory>

struct ModulesRpcInput
{
  std::function<ModuleStatesReply()> states;
  std::function<OwnerCatalogReply()> catalog;
  std::shared_ptr<const argus::client::FleetCallerGate> gate;
};

class ModulesRpcService final : public argus::settings::v1::Modules::CallbackService
{
public:
  explicit ModulesRpcService(ModulesRpcInput input);

  grpc::ServerUnaryReactor* ModuleStates(grpc::CallbackServerContext* context,
                                         const argus::settings::v1::ModuleStatesRequest* request,
                                         argus::settings::v1::ModuleStatesResponse* response) override;
  grpc::ServerUnaryReactor* OwnerCatalog(grpc::CallbackServerContext* context,
                                         const argus::settings::v1::OwnerCatalogRequest* request,
                                         argus::settings::v1::OwnerCatalogResponse* response) override;

private:
  std::function<ModuleStatesReply()> states_;
  std::function<OwnerCatalogReply()> catalog_;
  std::shared_ptr<const argus::client::FleetCallerGate> gate_;
};
