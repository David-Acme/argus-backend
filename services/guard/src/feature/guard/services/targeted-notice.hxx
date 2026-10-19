#pragma once

#include <feature/guard/guard-copy.hxx>
#include <feature/guard/services/response-plan.hxx>

#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <string>
#include <vector>

class NotificationClient;

struct TargetedNoticeInput
{
  NotificationClient* client{nullptr};
  std::vector<ResponseUser> users;
  GuardNotice notice;
  Json::Value data;
  std::string commandId;
};

namespace targeted_notice
{
[[nodiscard]] drogon::Task<bool> send(TargetedNoticeInput input);
}
