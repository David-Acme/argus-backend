#pragma once

#include <shared/services/camera-catalog/camera-catalog.hxx>
#include <shared/utils/network-address/private-address.hxx>
#include <shared/vocabulary/camera-stream-paths.hxx>

#include <optional>
#include <string>

namespace camera_address_rules
{
inline constexpr size_t kMaxSecretLength = 128;
inline constexpr int64_t kMaxRetentionDays = 3650;

inline std::optional<std::string> addressError(const std::string& ip)
{
  if (!network_address::isLiteral(ip))
    return "must be an IPv4 or IPv6 address";
  if (!network_address::isPrivate(ip))
    return "must be a private network address; camera credentials never leave the local network";
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

inline std::optional<std::string> retentionError(const std::optional<int64_t>& days)
{
  if (!days || (*days >= 0 && *days <= kMaxRetentionDays))
    return std::nullopt;
  return "must be between 0 and 3650 days";
}
}
