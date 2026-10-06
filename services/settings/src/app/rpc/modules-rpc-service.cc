#include "modules-rpc-service.hxx"

#include <utility>

ModulesRpcService::ModulesRpcService(std::function<ModuleStatesReply()> states,
                                     std::shared_ptr<const argus::client::FleetCallerGate> gate)
    : states_(std::move(states)), gate_(std::move(gate))
{
}

grpc::ServerUnaryReactor* ModulesRpcService::ModuleStates(grpc::CallbackServerContext* context,
                                                          const argus::settings::v1::ModuleStatesRequest*,
                                                          argus::settings::v1::ModuleStatesResponse* response)
{
  auto* reactor = context->DefaultReactor();
  const auto admission = gate_->admit(context, {});
  if (!admission.admitted()) {
    reactor->Finish(argus::client::FleetCallerGate::refusal(admission.verdict));
    return reactor;
  }
  const auto set = states_();
  for (const auto& module : set.modules) {
    auto* entry = response->add_modules();
    entry->set_id(module.id);
    entry->set_enabled(module.enabled);
    entry->set_lifecycle(module.lifecycle);
    entry->set_data_purged_at(module.dataPurgedAt);
  }
  response->set_version(set.version);
  response->set_settled(set.settled);
  reactor->Finish(grpc::Status::OK);
  return reactor;
}
