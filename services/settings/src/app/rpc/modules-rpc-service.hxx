#pragma once

#include <grpc/fleet-caller-gate.hxx>
#include <settings.grpc.pb.h>
#include <settings/component-vocabulary.hxx>

#include <functional>
#include <memory>

class ModulesRpcService final : public argus::settings::v1::Modules::CallbackService
{
public:
  ModulesRpcService(std::function<ModuleStatesReply()> states,
                    std::shared_ptr<const argus::client::FleetCallerGate> gate);

  grpc::ServerUnaryReactor* ModuleStates(grpc::CallbackServerContext* context,
                                         const argus::settings::v1::ModuleStatesRequest* request,
                                         argus::settings::v1::ModuleStatesResponse* response) override;

private:
  std::function<ModuleStatesReply()> states_;
  std::shared_ptr<const argus::client::FleetCallerGate> gate_;
};
