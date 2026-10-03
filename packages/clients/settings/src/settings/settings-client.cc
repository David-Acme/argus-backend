#include "settings-client.hxx"

#include <errors/response-exception.hxx>
#include <grpc/grpc-client-base.hxx>
#include <response/response-rpc.hxx>
#include <settings.grpc.pb.h>
#include <settings/settings-errors.hxx>

#include <optional>
#include <utility>

namespace
{
namespace wire = argus::settings::v1;

constexpr auto kMaxTimeout = std::chrono::seconds(120);

void check(const grpc::Status& status)
{
  if (!status.ok())
    throw argus::response::fromRpcStatus(status);
}

std::optional<SettingType> typeOf(wire::SettingType type)
{
  switch (type) {
  case wire::SETTING_TYPE_TOGGLE: return SettingType::Toggle;
  case wire::SETTING_TYPE_INTEGER: return SettingType::Integer;
  case wire::SETTING_TYPE_DECIMAL: return SettingType::Decimal;
  case wire::SETTING_TYPE_CHOICE: return SettingType::Choice;
  case wire::SETTING_TYPE_TEXT: return SettingType::Text;
  default: return std::nullopt;
  }
}

SettingLevel levelOf(wire::SettingLevel level)
{
  return level == wire::SETTING_LEVEL_BASIC ? SettingLevel::Basic : SettingLevel::Advanced;
}

SettingApply applyOf(wire::SettingApply apply)
{
  switch (apply) {
  case wire::SETTING_APPLY_LIVE: return SettingApply::Live;
  case wire::SETTING_APPLY_NEXT_SESSION: return SettingApply::NextSession;
  default: return SettingApply::Restart;
  }
}

SettingRejectionReason reasonOf(wire::RejectionReason reason)
{
  switch (reason) {
  case wire::REJECTION_REASON_UNKNOWN_KEY: return SettingRejectionReason::Unknown;
  case wire::REJECTION_REASON_OUT_OF_RANGE: return SettingRejectionReason::OutOfRange;
  case wire::REJECTION_REASON_NOT_A_CHOICE: return SettingRejectionReason::NotAChoice;
  case wire::REJECTION_REASON_WRITE_FAILED: return SettingRejectionReason::WriteFailed;
  default: return SettingRejectionReason::Invalid;
  }
}

SettingsCatalog catalogOf(const wire::SettingsCatalog& catalog)
{
  SettingsCatalog result{.service = catalog.service(), .settings = {}};
  result.settings.reserve(static_cast<std::size_t>(catalog.settings_size()));
  for (const auto& setting : catalog.settings()) {
    const auto type = typeOf(setting.type());
    if (!type)
      continue;
    result.settings.push_back(
        {.spec = {.key = setting.key(),
                  .group = setting.group(),
                  .type = *type,
                  .level = levelOf(setting.level()),
                  .apply = applyOf(setting.apply()),
                  .range = {.min = setting.min(), .max = setting.max(), .step = setting.step()},
                  .choices = {setting.choices().begin(), setting.choices().end()},
                  .fallback = setting.fallback()},
         .value = setting.value()});
  }
  return result;
}
}

struct SettingsClient::Impl
{
  SettingsClientConfig config;
  std::unique_ptr<wire::Settings::Stub> stub;

  void prepare(grpc::ClientContext& context) const
  {
    context.set_deadline(std::chrono::system_clock::now() + config.timeout);
    argus::client::addCallerCredential(context, config.credential);
  }
};

SettingsClient::SettingsClient(SettingsClientConfig config)
{
  if (config.target.empty() || config.credential.empty() || config.timeout.count() <= 0 ||
      config.timeout > kMaxTimeout)
    throw ResponseException(400, SettingsErrors::InvalidRequest);
  auto stub = wire::Settings::NewStub(argus::client::makeChannel(config.target));
  impl_ = std::make_unique<Impl>(Impl{.config = std::move(config), .stub = std::move(stub)});
}

SettingsClient::~SettingsClient() = default;

SettingsCatalog SettingsClient::list() const
{
  grpc::ClientContext context;
  impl_->prepare(context);
  wire::SettingsCatalog response;
  check(impl_->stub->List(&context, wire::ListSettingsRequest{}, &response));
  return catalogOf(response);
}

SettingsUpdateReply SettingsClient::update(const std::vector<SettingChange>& changes) const
{
  grpc::ClientContext context;
  impl_->prepare(context);
  wire::UpdateSettingsRequest request;
  for (const auto& change : changes) {
    auto* entry = request.add_changes();
    entry->set_key(change.key);
    entry->set_value(change.value);
  }
  wire::UpdateSettingsResponse response;
  check(impl_->stub->Update(&context, request, &response));

  SettingsUpdateReply reply{.applied = {response.applied().begin(), response.applied().end()},
                            .rejected = {},
                            .catalog = catalogOf(response.catalog())};
  reply.rejected.reserve(static_cast<std::size_t>(response.rejected_size()));
  for (const auto& rejection : response.rejected())
    reply.rejected.push_back({.key = rejection.key(), .reason = reasonOf(rejection.reason())});
  return reply;
}
