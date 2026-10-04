#include "settings-gateway-service.hxx"

#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
#include <feature/settings/dtos/setting-names.hxx>
#include <feature/settings/settings-gateway-errors.hxx>
#include <runtime/blocking-task.hxx>
#include <settings/settings-errors.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <exception>
#include <ranges>
#include <thread>
#include <utility>

namespace
{
std::string joined(const std::vector<std::string>& keys)
{
  std::string text;
  for (const auto& key : keys) {
    if (!text.empty())
      text += ", ";
    text += key;
  }
  return text;
}

bool writeFailed(const SettingRejection& rejection)
{
  return rejection.reason == SettingRejectionReason::WriteFailed;
}
}

SettingsGatewayService::SettingsGatewayService(const SettingsGatewayInput& input)
{
  owners_.reserve(input.owners.size());
  for (const auto& owner : input.owners)
    owners_.push_back({.name = owner.name,
                       .reader = std::make_unique<SettingsClient>(SettingsClientConfig{
                           .target = owner.target, .credential = owner.credential, .timeout = input.timeouts.list}),
                       .writer = std::make_unique<SettingsClient>(SettingsClientConfig{
                           .target = owner.target, .credential = owner.credential, .timeout = input.timeouts.update})});
}

std::vector<OwnerCatalog> SettingsGatewayService::catalogs() const
{
  std::vector<const OwnerLink*> links;
  links.reserve(owners_.size());
  for (const auto& owner : owners_)
    links.push_back(&owner);
  return fetchAll(links);
}

std::vector<OwnerCatalog> SettingsGatewayService::catalogsOf(const std::vector<std::string>& names) const
{
  std::vector<const OwnerLink*> links;
  links.reserve(names.size());
  for (const auto& owner : owners_)
    if (std::ranges::find(names, owner.name) != names.end())
      links.push_back(&owner);
  return fetchAll(links);
}

std::vector<OwnerCatalog> SettingsGatewayService::fetchAll(const std::vector<const OwnerLink*>& links)
{
  std::vector<OwnerCatalog> catalogs(links.size());
  {
    std::vector<std::jthread> workers;
    workers.reserve(links.size());
    for (const auto index : std::views::iota(std::size_t{0}, links.size()))
      workers.emplace_back([&links, index, &catalogs] { catalogs[index] = fetch(*links[index]); });
  }
  return catalogs;
}

std::vector<OwnerWriteResult> SettingsGatewayService::write(const std::vector<OwnerWrite>& writes) const
{
  std::vector<OwnerWriteResult> results(writes.size());
  {
    std::vector<std::jthread> workers;
    workers.reserve(writes.size());
    for (const auto index : std::views::iota(std::size_t{0}, writes.size()))
      workers.emplace_back([this, &writes, index, &results] { results[index] = send(writes[index]); });
  }
  return results;
}

OwnerWriteResult SettingsGatewayService::send(const OwnerWrite& write) const
{
  OwnerWriteResult result{.owner = write.owner, .reachable = false, .applied = {}, .rejected = {}, .catalog = {}};
  const auto* owner = find(write.owner);
  if (owner == nullptr || write.changes.empty())
    return result;
  try {
    auto reply = owner->writer->update(write.changes);
    result.reachable = true;
    result.applied = std::move(reply.applied);
    result.rejected = std::move(reply.rejected);
    result.catalog = OwnerCatalog{.service = owner->name, .reachable = true, .settings = std::move(reply.catalog.settings)};
  }
  catch (const ResponseException& error) {
    LOG_WARN << "Settings owner " << owner->name << " refused or missed an update: " << error.statusCode() << " "
             << error.errorCode();
  }
  catch (const std::exception& error) {
    LOG_ERROR << "Settings owner " << owner->name << " answered an update unreadably: " << error.what();
  }
  return result;
}

drogon::Task<std::vector<OwnerCatalog>> SettingsGatewayService::catalogsAsync() const
{
  co_return co_await BlockingTask<std::vector<OwnerCatalog>>([this] { return catalogs(); });
}

SettingsUpdateOutcome SettingsGatewayService::update(const SettingsUpdateInput& input) const
{
  const auto* owner = find(input.owner);
  if (owner == nullptr)
    throw ResponseException(SettingsErrors::UnknownService);

  SettingsUpdateReply reply;
  try {
    reply = owner->writer->update(input.changes);
  }
  catch (const ResponseException& error) {
    LOG_WARN << "Settings owner " << owner->name << " refused or missed an update: " << error.statusCode() << " "
             << error.errorCode();
    throw ResponseException(SettingsErrors::Unavailable);
  }

  if (!reply.applied.empty())
    LOG_INFO << "User " << input.userId << " changed " << owner->name << " settings: " << joined(reply.applied);

  if (std::ranges::any_of(reply.rejected, writeFailed)) {
    std::vector<std::string> failed;
    for (const auto& rejection : reply.rejected | std::views::filter(writeFailed))
      failed.push_back(rejection.key);
    LOG_ERROR << "Settings owner " << owner->name << " could not persist " << joined(failed) << " for user "
              << input.userId;
    throw ResponseException(SettingsGatewayErrors::WriteFailed);
  }

  if (!reply.rejected.empty()) {
    ValidationErrors errors;
    for (const auto& rejection : reply.rejected)
      errors[rejection.key].push_back(rejectionReasonName(rejection.reason));
    throw ValidationException(errors);
  }

  return {.applied = std::move(reply.applied),
          .catalog = {.service = owner->name, .reachable = true, .settings = std::move(reply.catalog.settings)}};
}

drogon::Task<SettingsUpdateOutcome> SettingsGatewayService::updateAsync(SettingsUpdateInput input) const
{
  co_return co_await BlockingTask<SettingsUpdateOutcome>([this, input = std::move(input)] { return update(input); });
}

OwnerCatalog SettingsGatewayService::fetch(const OwnerLink& owner)
{
  try {
    auto catalog = owner.reader->list();
    return {.service = owner.name, .reachable = true, .settings = std::move(catalog.settings)};
  }
  catch (const ResponseException& error) {
    LOG_WARN << "Settings owner " << owner.name << " is unreachable: " << error.statusCode() << " "
             << error.errorCode();
    return {.service = owner.name, .reachable = false, .settings = {}};
  }
  catch (const std::exception& error) {
    LOG_ERROR << "Settings owner " << owner.name << " answered unreadably: " << error.what();
    return {.service = owner.name, .reachable = false, .settings = {}};
  }
}

const SettingsGatewayService::OwnerLink* SettingsGatewayService::find(const std::string& name) const
{
  const auto found = std::ranges::find(owners_, name, &OwnerLink::name);
  return found == owners_.end() ? nullptr : &*found;
}
