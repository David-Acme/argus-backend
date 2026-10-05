#pragma once

#include <shared/services/camera-catalog/camera-catalog.hxx>
#include <shared/utils/network-address/private-address.hxx>
#include <shared/vocabulary/camera-stream-paths.hxx>

#include <optional>
#include <string>

namespace camera_address_rules
{
inline constexpr size_t kMaxSecretLength = 128;
inline constexpr int64_t kMaxRetentionDays = 60;
inline constexpr int64_t kMaxIncidentRetentionDays = 120;

inline std::optional<std::string> addressError(const std::string& ip)
{
  if (!network_address::isLiteral(ip))
    return "must be an IPv4 or IPv6 address";
  if (!network_address::isPrivate(ip))
    return "must be a private network address; camera credentials never leave the local network";
  if (network_address::isHostLocal(ip))
    return "must be a camera on the local network, not a loopback or link-local address";
  return std::nullopt;
}

inline std::optional<std::string> pathError(const std::optional<std::string>& path)
{
  if (!path || camera_stream_paths::isValid(*path))
    return std::nullopt;
  return "must start with / and use only letters, digits and / . _ ~ - ? = & % + , ; :";
}

inline std::optional<std::string> catalogError(const std::optional<std::string>& catalogId)
{
  if (!catalogId || catalogId->empty() || camera_catalog::byId(*catalogId) != nullptr)
    return std::nullopt;
  return "must be a model id from GET /camera/catalog";
}

struct RetentionRule
{
  std::optional<int64_t> days;
  bool incident{false};
};

inline int64_t retentionCap(bool incident)
{
  return incident ? kMaxIncidentRetentionDays : kMaxRetentionDays;
}

inline std::optional<std::string> retentionError(const RetentionRule& rule)
{
  if (!rule.days || (*rule.days >= 0 && *rule.days <= retentionCap(rule.incident)))
    return std::nullopt;
  return rule.incident ? "must be between 0 and 120 days while an incident is open"
                       : "must be between 0 and 60 days (up to 120 with retentionIncident)";
}
}
