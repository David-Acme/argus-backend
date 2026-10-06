#include "notification-module-request.hxx"

#include "module-request-copy.hxx"

#include <json/value.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view kType = "module_request";
constexpr std::string_view kKind = "module_request";
constexpr std::string_view kAction = "enable_module";
constexpr std::string_view kUrgency = "active";
constexpr std::string_view kEnglish = "en";
}

NotificationModuleRequest::NotificationModuleRequest(NotificationModuleRequestInput input)
    : identity_(std::move(input.identity)), service_(std::move(input.delivery))
{
}

ModuleRequestOutcome NotificationModuleRequest::request(const ModuleRequestInput& input)
{
  return drogon::sync_wait(requestAsync(input));
}

drogon::Task<ModuleRequestOutcome> NotificationModuleRequest::requestAsync(ModuleRequestInput input) const
{
  if (!identity_)
    throw std::runtime_error("module requests need the identity service to find the Owner");
  const auto users = identity_->listUsers();
  if (!users)
    throw std::runtime_error("the household could not be read from the identity service");

  std::string requester;
  std::vector<argus::identity::v1::UserIdentity> owners;
  for (const auto& user : *users) {
    if (user.user_id() == input.userId)
      requester = user.name();
    if (user.role() == "owner" && user.is_active())
      owners.push_back(user);
  }

  const std::string threadKey = std::string(kType) + ":" + input.moduleId + ":" + std::to_string(input.userId) + ":" + input.day;
  ModuleRequestOutcome outcome{.notified = 0, .duplicate = false};
  bool anyDuplicate = false;
  for (const auto& owner : owners) {
    const std::string lang = owner.lang() == kEnglish ? std::string(kEnglish) : std::string("es");
    const auto text = module_request_copy::render(
        {.lang = lang, .requester = requester, .module = lang == kEnglish ? input.moduleName.en : input.moduleName.es});
    Json::Value data(Json::objectValue);
    data["kind"] = std::string(kKind);
    data["moduleId"] = input.moduleId;
    data["requestedBy"] = static_cast<Json::Int64>(input.userId);
    data["requestedByName"] = requester;
    data["action"] = std::string(kAction);
    data["threadKey"] = threadKey;
    data["urgency"] = std::string(kUrgency);
    data["lang"] = lang;
    const auto created = co_await service_.createManyAndEmit(
        {.userIds = {owner.user_id()},
         .notification = {.userId = 0, .type = std::string(kType), .title = text.title, .body = text.body, .data = data},
         .commandId = threadKey + ":" + std::to_string(owner.user_id())});
    if (created.duplicate)
      anyDuplicate = true;
    else
      outcome.notified += static_cast<std::int32_t>(created.createdCount);
  }
  outcome.duplicate = anyDuplicate && outcome.notified == 0;
  co_return outcome;
}
