#include "modules-rpc-service.hxx"

#include <utility>

ModulesRpcService::ModulesRpcService(ModulesRpcInput input)
    : states_(std::move(input.states)), catalog_(std::move(input.catalog)), gate_(std::move(input.gate))
{
}

namespace
{
void fill(argus::settings::v1::ModuleIntroLine* out, const ModuleIntroLine& line)
{
  out->set_what(line.what);
  for (const auto& example : line.examples)
    out->add_examples(example);
}
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
    for (const auto& role : module.roles)
      entry->add_roles(role);
    entry->set_name_es(module.name.es);
    entry->set_name_en(module.name.en);
    entry->set_summary_es(module.summary.es);
    entry->set_summary_en(module.summary.en);
    fill(entry->mutable_intro_es(), module.intro.es);
    fill(entry->mutable_intro_en(), module.intro.en);
    entry->set_kind(module.kind);
  }
  response->set_version(set.version);
  response->set_settled(set.settled);
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

grpc::ServerUnaryReactor* ModulesRpcService::OwnerCatalog(grpc::CallbackServerContext* context,
                                                          const argus::settings::v1::OwnerCatalogRequest*,
                                                          argus::settings::v1::OwnerCatalogResponse* response)
{
  auto* reactor = context->DefaultReactor();
  const auto admission = gate_->admit(context, {});
  if (!admission.admitted()) {
    reactor->Finish(argus::client::FleetCallerGate::refusal(admission.verdict));
    return reactor;
  }
  if (!catalog_) {
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "the module catalog is not loaded"));
    return reactor;
  }
  const auto catalog = catalog_();
  response->set_modules_json(catalog.modulesJson);
  response->set_version(catalog.version);
  reactor->Finish(grpc::Status::OK);
  return reactor;
}
