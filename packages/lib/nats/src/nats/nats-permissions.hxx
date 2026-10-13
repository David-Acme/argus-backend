#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace nats_permissions
{

struct Consumer
{
  std::string_view stream;
  std::string_view name;
  std::string_view subject;
  bool recreates{false};
};

struct ServicePermissions
{
  std::string_view service;
  std::vector<std::string_view> publishes;
  std::vector<std::string_view> stores;
  std::vector<std::string_view> subscribes;
  std::vector<std::string_view> streams;
  std::vector<Consumer> consumers;
};

struct Grants
{
  std::vector<std::string> publish;
  std::vector<std::string> subscribe;
};

inline constexpr std::string_view kInboxRoot = "_INBOX";
inline constexpr std::string_view kVariablePrefix = "ARGUS_NATS_";

const std::vector<ServicePermissions>& table();

std::string inboxPrefix(std::string_view service);
std::string variableName(std::string_view service);
bool needsInbox(const ServicePermissions& row);
Grants grantsFor(const ServicePermissions& row);
std::string render();

}
