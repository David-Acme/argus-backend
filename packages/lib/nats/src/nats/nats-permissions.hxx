#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nats_permissions
{

enum class StreamUse : std::uint8_t
{
  Inspect,
  Create,
  Ensure
};

std::string_view streamUseToString(StreamUse use);
std::optional<StreamUse> streamUseFromString(std::string_view text);

enum class ConsumerKind : std::uint8_t
{
  Push,
  Pull
};

std::string_view consumerKindToString(ConsumerKind kind);
std::optional<ConsumerKind> consumerKindFromString(std::string_view text);

struct StreamGrant
{
  std::string_view stream;
  StreamUse use{StreamUse::Ensure};
};

struct Consumer
{
  std::string_view stream;
  std::string_view name;
  std::vector<std::string_view> subjects;
  ConsumerKind kind{ConsumerKind::Push};
  bool recreates{false};
};

struct ServicePermissions
{
  std::string_view service;
  std::vector<std::string_view> publishes;
  std::vector<std::string_view> stores;
  std::vector<std::string_view> subscribes;
  std::vector<StreamGrant> streams;
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
