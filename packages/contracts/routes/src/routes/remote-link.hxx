#pragma once

#include <json/value.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace routes
{
enum class RemoteLink : std::uint8_t
{
  Up,
  Connecting,
  Down
};

constexpr std::string_view remoteLinkToString(RemoteLink link)
{
  switch (link) {
    case RemoteLink::Up:
      return "up";
    case RemoteLink::Connecting:
      return "connecting";
    case RemoteLink::Down:
      return "down";
  }
  return "down";
}

constexpr std::optional<RemoteLink> remoteLinkFromString(std::string_view text)
{
  if (text == "up")
    return RemoteLink::Up;
  if (text == "connecting")
    return RemoteLink::Connecting;
  if (text == "down")
    return RemoteLink::Down;
  return std::nullopt;
}

struct RemoteLinkState
{
  std::string homeId;
  std::string domain;
  RemoteLink link{RemoteLink::Down};
  int64_t since{0};

  bool operator==(const RemoteLinkState&) const = default;
};

inline constexpr std::string_view kRemoteHomeIdField = "homeId";
inline constexpr std::string_view kRemoteDomainField = "domain";
inline constexpr std::string_view kRemoteLinkField = "link";
inline constexpr std::string_view kRemoteSinceField = "since";

inline constexpr std::size_t kHomeIdLength = 32;
inline constexpr std::size_t kMaxDomainLength = 253;
inline constexpr std::size_t kMaxLabelLength = 63;

constexpr bool isHomeId(std::string_view text)
{
  return text.size() == kHomeIdLength && std::ranges::all_of(text, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

constexpr bool isDomainLabel(std::string_view label, bool last)
{
  if (label.empty() || label.size() > kMaxLabelLength)
    return false;
  const auto lower = [](char c) { return c >= 'a' && c <= 'z'; };
  const auto digit = [](char c) { return c >= '0' && c <= '9'; };
  if (last)
    return label.size() >= 2 && std::ranges::all_of(label, lower);
  if (label.front() == '-' || label.back() == '-')
    return false;
  return std::ranges::all_of(label, [&](char c) { return lower(c) || digit(c) || c == '-'; });
}

constexpr bool isDomainName(std::string_view text)
{
  if (text.empty() || text.size() > kMaxDomainLength)
    return false;
  std::size_t labels = 0;
  while (true) {
    const std::size_t dot = text.find('.');
    const bool last = dot == std::string_view::npos;
    if (!isDomainLabel(last ? text : text.substr(0, dot), last))
      return false;
    ++labels;
    if (last)
      return labels >= 2;
    text.remove_prefix(dot + 1);
  }
}

inline Json::Value remoteLinkToJson(const RemoteLinkState& state)
{
  Json::Value json(Json::objectValue);
  json[std::string(kRemoteHomeIdField)] = state.homeId;
  json[std::string(kRemoteDomainField)] = state.domain;
  json[std::string(kRemoteLinkField)] = std::string(remoteLinkToString(state.link));
  json[std::string(kRemoteSinceField)] = static_cast<Json::Int64>(state.since);
  return json;
}

enum class RemoteLinkRefusal : std::uint8_t
{
  None,
  NotAnObject,
  HomeId,
  Domain,
  Link,
  Since
};

constexpr std::string_view remoteLinkRefusalToString(RemoteLinkRefusal refusal)
{
  switch (refusal) {
    case RemoteLinkRefusal::None:
      return "none";
    case RemoteLinkRefusal::NotAnObject:
      return "the frame is not a JSON object";
    case RemoteLinkRefusal::HomeId:
      return "homeId is not 32 lowercase hexadecimal characters";
    case RemoteLinkRefusal::Domain:
      return "domain is not a DNS name";
    case RemoteLinkRefusal::Link:
      return "link is not up, connecting or down";
    case RemoteLinkRefusal::Since:
      return "since is not a positive integer";
  }
  return "unknown";
}

struct RemoteLinkReading
{
  std::optional<RemoteLinkState> state;
  RemoteLinkRefusal refusal{RemoteLinkRefusal::None};
};

inline RemoteLinkReading readRemoteLink(const Json::Value& json)
{
  if (!json.isObject())
    return {.state = std::nullopt, .refusal = RemoteLinkRefusal::NotAnObject};
  const Json::Value& homeId = json[std::string(kRemoteHomeIdField)];
  if (!homeId.isString() || !isHomeId(homeId.asString()))
    return {.state = std::nullopt, .refusal = RemoteLinkRefusal::HomeId};
  const Json::Value& domain = json[std::string(kRemoteDomainField)];
  if (!domain.isString() || !isDomainName(domain.asString()))
    return {.state = std::nullopt, .refusal = RemoteLinkRefusal::Domain};
  const Json::Value& link = json[std::string(kRemoteLinkField)];
  const auto parsedLink = link.isString() ? remoteLinkFromString(link.asString()) : std::nullopt;
  if (!parsedLink)
    return {.state = std::nullopt, .refusal = RemoteLinkRefusal::Link};
  const Json::Value& since = json[std::string(kRemoteSinceField)];
  const bool integer = since.type() == Json::intValue || since.type() == Json::uintValue;
  if (!integer || !since.isInt64() || since.asInt64() <= 0)
    return {.state = std::nullopt, .refusal = RemoteLinkRefusal::Since};
  return {.state = RemoteLinkState{.homeId = homeId.asString(),
                                   .domain = domain.asString(),
                                   .link = *parsedLink,
                                   .since = since.asInt64()},
          .refusal = RemoteLinkRefusal::None};
}
}
