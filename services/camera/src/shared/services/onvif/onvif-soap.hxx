#pragma once

#include <shared/services/onvif/onvif-types.hxx>

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <pugixml.hpp>
#include <string>
#include <string_view>
#include <vector>

enum class OnvifService : uint8_t
{
  Device = 0,
  Media,
  Ptz,
  Events,
  Notification
};

inline std::string onvifServiceToString(OnvifService service)
{
  switch (service) {
    case OnvifService::Device:
      return "device";
    case OnvifService::Media:
      return "media";
    case OnvifService::Ptz:
      return "ptz";
    case OnvifService::Events:
      return "events";
    case OnvifService::Notification:
      return "notification";
  }
  return "device";
}

inline OnvifService onvifServiceFromString(std::string_view text)
{
  if (text == "media")
    return OnvifService::Media;
  if (text == "ptz")
    return OnvifService::Ptz;
  if (text == "events")
    return OnvifService::Events;
  if (text == "notification")
    return OnvifService::Notification;
  return OnvifService::Device;
}

struct OnvifSecurity
{
  std::string username;
  std::string password;
  std::vector<uint8_t> nonce;
  std::string created;
};

using OnvifBodyWriter = std::function<void(pugi::xml_node&)>;

struct OnvifEnvelopeInput
{
  OnvifService service{OnvifService::Device};
  std::string operation;
  OnvifBodyWriter body;
  std::optional<OnvifSecurity> security;
  OnvifBodyWriter header;
};

struct OnvifReply
{
  std::shared_ptr<pugi::xml_document> document;
  pugi::xml_node response;
  bool fault{false};
  std::vector<std::string> faultCodes;
  std::string faultReason;
};

namespace onvif_soap
{
inline constexpr const char* kSoapNamespace = "http://www.w3.org/2003/05/soap-envelope";
inline constexpr const char* kSchemaNamespace = "http://www.onvif.org/ver10/schema";
inline constexpr const char* kAddressingNamespace = "http://www.w3.org/2005/08/addressing";
inline constexpr const char* kPasswordDigestType =
    "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest";

std::string_view prefixOf(OnvifService service);
std::string_view namespaceOf(OnvifService service);
std::string actionOf(OnvifService service, std::string_view operation);

std::string passwordDigest(const OnvifSecurity& security);
std::string createdAt(int64_t epochSeconds);
std::string envelope(const OnvifEnvelopeInput& input);
std::optional<OnvifReply> reply(std::string_view xml);
OnvifFailure faultKind(const OnvifReply& reply);

std::string_view localName(const pugi::xml_node& node);
pugi::xml_node child(const pugi::xml_node& parent, std::string_view local);
pugi::xml_node path(const pugi::xml_node& from, std::initializer_list<std::string_view> locals);
std::string textOf(const pugi::xml_node& node);
}
