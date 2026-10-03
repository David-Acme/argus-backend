#include "settings-rpc.hxx"

#include <response/response-rpc.hxx>
#include <settings/settings-errors.hxx>

#include <stdexcept>
#include <utility>

namespace
{
namespace wire = argus::settings::v1;

constexpr int kMaxChanges = 64;

wire::SettingType typeOf(SettingType type)
{
  switch (type) {
  case SettingType::Toggle: return wire::SETTING_TYPE_TOGGLE;
  case SettingType::Integer: return wire::SETTING_TYPE_INTEGER;
  case SettingType::Decimal: return wire::SETTING_TYPE_DECIMAL;
  case SettingType::Choice: return wire::SETTING_TYPE_CHOICE;
  case SettingType::Text: return wire::SETTING_TYPE_TEXT;
  }
  return wire::SETTING_TYPE_UNSPECIFIED;
}

wire::SettingLevel levelOf(SettingLevel level)
{
  return level == SettingLevel::Basic ? wire::SETTING_LEVEL_BASIC : wire::SETTING_LEVEL_ADVANCED;
}

wire::SettingApply applyOf(SettingApply apply)
{
  switch (apply) {
  case SettingApply::Live: return wire::SETTING_APPLY_LIVE;
  case SettingApply::NextSession: return wire::SETTING_APPLY_NEXT_SESSION;
  case SettingApply::Restart: return wire::SETTING_APPLY_RESTART;
  }
  return wire::SETTING_APPLY_UNSPECIFIED;
}

wire::RejectionReason reasonOf(SettingRejectionReason reason)
{
  switch (reason) {
  case SettingRejectionReason::Unknown: return wire::REJECTION_REASON_UNKNOWN_KEY;
  case SettingRejectionReason::Invalid: return wire::REJECTION_REASON_INVALID;
  case SettingRejectionReason::OutOfRange: return wire::REJECTION_REASON_OUT_OF_RANGE;
  case SettingRejectionReason::NotAChoice: return wire::REJECTION_REASON_NOT_A_CHOICE;
  case SettingRejectionReason::WriteFailed: return wire::REJECTION_REASON_WRITE_FAILED;
  }
  return wire::REJECTION_REASON_UNSPECIFIED;
}

grpc::ServerUnaryReactor* finish(grpc::CallbackServerContext* context, grpc::Status status)
{
  auto* reactor = context->DefaultReactor();
  reactor->Finish(std::move(status));
  return reactor;
}
}

std::vector<argus::client::CallerCredential> settingsCallers(
    const std::vector<std::pair<std::string, std::string>>& callers)
{
  std::vector<std::pair<std::string, std::string>> settings;
  for (const auto& caller : callers)
    if (caller.first == kSettingsCaller)
      settings.push_back(caller);
  return argus::client::callerCredentialsFromPairs(settings);
}

void withoutSettingsCaller(std::vector<std::pair<std::string, std::string>>& callers)
{
  std::erase_if(callers, [](const auto& caller) { return caller.first == kSettingsCaller; });
}

SettingsRpcService::SettingsRpcService(SettingsRpcInput input) : input_(std::move(input))
{
  if (input_.service.empty() || input_.registry == nullptr || input_.credentials.empty())
    throw std::invalid_argument("Invalid settings RPC configuration");
}

grpc::ServerUnaryReactor* SettingsRpcService::List(grpc::CallbackServerContext* context,
                                                   const wire::ListSettingsRequest*,
                                                   wire::SettingsCatalog* response)
{
  if (!argus::client::authorizeCaller(context, input_.credentials))
    return finish(context, argus::response::toRpcStatus(ResponseException(SettingsErrors::Unauthorized)));
  fillCatalog(*response);
  return finish(context, grpc::Status::OK);
}

grpc::ServerUnaryReactor* SettingsRpcService::Update(grpc::CallbackServerContext* context,
                                                     const wire::UpdateSettingsRequest* request,
                                                     wire::UpdateSettingsResponse* response)
{
  if (!argus::client::authorizeCaller(context, input_.credentials))
    return finish(context, argus::response::toRpcStatus(ResponseException(SettingsErrors::Unauthorized)));
  if (request->changes_size() == 0 || request->changes_size() > kMaxChanges)
    return finish(context, argus::response::toRpcStatus(ResponseException(SettingsErrors::InvalidRequest)));

  std::vector<SettingChange> changes;
  changes.reserve(static_cast<std::size_t>(request->changes_size()));
  for (const auto& change : request->changes())
    changes.push_back({.key = change.key(), .value = change.value()});

  const auto result = input_.registry->update(changes);
  for (const auto& key : result.applied)
    response->add_applied(key);
  for (const auto& rejection : result.rejected) {
    auto* entry = response->add_rejected();
    entry->set_key(rejection.key);
    entry->set_reason(reasonOf(rejection.reason));
  }
  fillCatalog(*response->mutable_catalog());
  return finish(context, grpc::Status::OK);
}

void SettingsRpcService::fillCatalog(wire::SettingsCatalog& catalog) const
{
  catalog.set_service(input_.service);
  for (const auto& entry : input_.registry->list()) {
    auto* setting = catalog.add_settings();
    setting->set_key(entry.spec.key);
    setting->set_group(entry.spec.group);
    setting->set_type(typeOf(entry.spec.type));
    setting->set_level(levelOf(entry.spec.level));
    setting->set_apply(applyOf(entry.spec.apply));
    setting->set_min(entry.spec.range.min);
    setting->set_max(entry.spec.range.max);
    setting->set_step(entry.spec.range.step);
    for (const auto& choice : entry.spec.choices)
      setting->add_choices(choice);
    setting->set_value(entry.value);
    setting->set_fallback(entry.spec.fallback);
  }
}
