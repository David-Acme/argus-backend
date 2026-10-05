#include "response-feature-service.hxx"

#include <algorithm>
#include <ctime>
#include <errors/response-exception.hxx>
#include <feature/guard/controllers/guard-errors.hxx>
#include <feature/guard/guard-schedule.hxx>
#include <runtime/blocking-task.hxx>
#include <utility>

namespace
{
Json::Value memberJson(const ResponseMember& member, bool staffedNow)
{
  Json::Value json(Json::objectValue);
  json["userId"] = static_cast<Json::Int64>(member.userId);
  json["name"] = member.name;
  json["role"] = userRoleToString(member.role);
  json["mode"] = recipientModeToString(member.mode);
  json["step"] = member.step;
  json["onDuty"] = member.onDuty;
  json["customized"] = member.customized;
  json["mandatory"] = member.role == UserRole::Guard &&
                      member.mode != RecipientMode::Off &&
                      (member.onDuty || staffedNow);
  return json;
}
}

ResponseFeatureService::ResponseFeatureService(
    ResponseFeatureDependencies dependencies)
    : dependencies_(std::move(dependencies))
{
}

int64_t ResponseFeatureService::now() const
{
  return dependencies_.clock ? dependencies_.clock()
                             : static_cast<int64_t>(std::time(nullptr));
}

drogon::Task<std::vector<ResponseUser>> ResponseFeatureService::users() const
{
  if (!dependencies_.directory)
    throw ResponseException(GuardErrors::DirectoryUnavailable);
  const auto directory = dependencies_.directory;
  const auto users =
      co_await BlockingTask<std::optional<std::vector<ResponseUser>>>(
          [directory]() { return directory->users(); });
  if (!users)
    throw ResponseException(GuardErrors::DirectoryUnavailable);
  co_return *users;
}

drogon::Task<Json::Value>
ResponseFeatureService::view(const ViewInput& input) const
{
  const auto environment =
      co_await environmentRepository_.find(input.environmentId);
  if (!environment)
    throw ResponseException(GuardErrors::EnvironmentNotFound);
  const auto everyone = co_await users();
  const auto config = co_await repository_.forEnvironment(input.environmentId);
  const auto members =
      response_plan::members({.users = everyone, .rows = config.recipients});
  const auto at = static_cast<std::time_t>(now());
  std::tm local{};
  localtime_r(&at, &local);
  const bool staffedNow =
      response_plan::staffedAt(guard_schedule::fromEnvironment(*environment),
                               local);

  Json::Value json(Json::objectValue);
  json["environmentId"] = static_cast<Json::Int64>(input.environmentId);
  json["emergencyNumber"] = config.setting.emergencyNumber;
  json["stepSeconds"] = config.setting.stepSeconds;
  json["staffedNow"] = staffedNow;
  Json::Value recipients(Json::arrayValue);
  for (const auto& member : members) {
    if (input.role == UserRole::Owner || member.userId == input.userId)
      recipients.append(memberJson(member, staffedNow));
  }
  json["recipients"] = std::move(recipients);
  Json::Value contacts(Json::arrayValue);
  for (const auto& contact : config.contacts) {
    Json::Value item(Json::objectValue);
    item["id"] = static_cast<Json::Int64>(contact.id);
    item["name"] = contact.name;
    item["phone"] = contact.phone;
    item["note"] = contact.note;
    contacts.append(std::move(item));
  }
  json["contacts"] = std::move(contacts);
  co_return json;
}

drogon::Task<Json::Value>
ResponseFeatureService::replace(const ReplaceInput& input) const
{
  if (!co_await environmentRepository_.find(input.environmentId))
    throw ResponseException(GuardErrors::EnvironmentNotFound);
  const auto everyone = co_await users();
  std::vector<ResponseRecipientInput> recipients;
  recipients.reserve(input.body.recipients.size());
  for (const auto& recipient : input.body.recipients) {
    const auto user =
        std::ranges::find(everyone, recipient.userId, &ResponseUser::userId);
    if (user == everyone.end() || !user->active)
      throw ResponseException(GuardErrors::RecipientUnknown);
    recipients.push_back(
        {.userId = recipient.userId,
         .mode = recipientModeFromString(recipient.mode)
                     .value_or(RecipientMode::Off),
         .step = recipient.step,
         .onDuty = recipient.onDuty && user->role == UserRole::Guard});
  }
  std::vector<ResponseContactInput> contacts;
  contacts.reserve(input.body.contacts.size());
  for (const auto& contact : input.body.contacts)
    contacts.push_back(
        {.name = contact.name, .phone = contact.phone, .note = contact.note});
  co_await repository_.replace({.environmentId = input.environmentId,
                                .emergencyNumber = input.body.emergencyNumber,
                                .stepSeconds = input.body.stepSeconds,
                                .recipients = std::move(recipients),
                                .contacts = std::move(contacts),
                                .at = now()});
  co_return co_await view({.environmentId = input.environmentId,
                           .userId = input.userId,
                           .role = UserRole::Owner});
}

drogon::Task<Json::Value>
ResponseFeatureService::setDuty(const DutyInput& input) const
{
  if (!co_await environmentRepository_.find(input.environmentId))
    throw ResponseException(GuardErrors::EnvironmentNotFound);
  const auto everyone = co_await users();
  const auto user =
      std::ranges::find(everyone, input.userId, &ResponseUser::userId);
  if (user == everyone.end() || user->role != UserRole::Guard)
    throw ResponseException(GuardErrors::DutyForGuardsOnly);
  co_await repository_.setDuty({.environmentId = input.environmentId,
                                .userId = input.userId,
                                .onDuty = input.onDuty,
                                .at = now()});
  co_return co_await view({.environmentId = input.environmentId,
                           .userId = input.userId,
                           .role = input.role});
}
