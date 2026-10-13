#include <shared/services/onvif/onvif-soap.hxx>

#include <shared/services/tapo/tapo-crypto.hxx>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <sstream>

namespace
{
constexpr const char* kSecurityNamespace =
    "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd";
constexpr const char* kUtilityNamespace =
    "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd";
constexpr const char* kNonceEncoding =
    "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary";

constexpr std::array<std::string_view, 6> kUnauthorizedCodes{
    "NotAuthorized",        "FailedAuthentication",    "InvalidSecurity",
    "InvalidSecurityToken", "SecurityTokenUnavailable", "FailedCheck"};

constexpr std::array<std::string_view, 5> kUnsupportedCodes{
    "ActionNotSupported", "NoPTZProfile", "NoProfile", "NotSupported", "NoSuchService"};

void appendText(pugi::xml_node& parent, const std::string& name, const std::string& value)
{
  parent.append_child(name.c_str()).text().set(value.c_str());
}

void appendSecurity(pugi::xml_node& header, const OnvifSecurity& security)
{
  auto wsse = header.append_child("wsse:Security");
  wsse.append_attribute("s:mustUnderstand") = "1";
  wsse.append_attribute("xmlns:wsse") = kSecurityNamespace;
  wsse.append_attribute("xmlns:wsu") = kUtilityNamespace;
  auto token = wsse.append_child("wsse:UsernameToken");
  appendText(token, "wsse:Username", security.username);
  auto password = token.append_child("wsse:Password");
  password.append_attribute("Type") = onvif_soap::kPasswordDigestType;
  password.text().set(onvif_soap::passwordDigest(security).c_str());
  auto nonce = token.append_child("wsse:Nonce");
  nonce.append_attribute("EncodingType") = kNonceEncoding;
  nonce.text().set(tapo_crypto::base64Encode(security.nonce).c_str());
  appendText(token, "wsu:Created", security.created);
}

std::string_view afterColon(std::string_view value)
{
  const auto colon = value.rfind(':');
  return colon == std::string_view::npos ? value : value.substr(colon + 1);
}

pugi::xml_node firstElement(const pugi::xml_node& parent)
{
  for (const auto node : parent.children()) {
    if (node.type() == pugi::node_element)
      return node;
  }
  return {};
}
}

std::string_view onvif_soap::prefixOf(OnvifService service)
{
  switch (service) {
    case OnvifService::Device:
      return "tds";
    case OnvifService::Media:
      return "trt";
    case OnvifService::Ptz:
      return "tptz";
  }
  return "tds";
}

std::string_view onvif_soap::namespaceOf(OnvifService service)
{
  switch (service) {
    case OnvifService::Device:
      return "http://www.onvif.org/ver10/device/wsdl";
    case OnvifService::Media:
      return "http://www.onvif.org/ver10/media/wsdl";
    case OnvifService::Ptz:
      return "http://www.onvif.org/ver20/ptz/wsdl";
  }
  return "http://www.onvif.org/ver10/device/wsdl";
}

std::string onvif_soap::actionOf(OnvifService service, std::string_view operation)
{
  return std::string(namespaceOf(service)) + "/" + std::string(operation);
}

std::string onvif_soap::passwordDigest(const OnvifSecurity& security)
{
  std::string material(security.nonce.begin(), security.nonce.end());
  material += security.created;
  material += security.password;
  return tapo_crypto::base64Encode(tapo_crypto::sha1Raw(material));
}

std::string onvif_soap::createdAt(int64_t epochSeconds)
{
  const std::chrono::sys_seconds at{std::chrono::seconds{epochSeconds}};
  return std::format("{:%Y-%m-%dT%H:%M:%SZ}", at);
}

std::string onvif_soap::envelope(const OnvifEnvelopeInput& input)
{
  pugi::xml_document document;
  auto declaration = document.append_child(pugi::node_declaration);
  declaration.append_attribute("version") = "1.0";
  declaration.append_attribute("encoding") = "UTF-8";
  auto root = document.append_child("s:Envelope");
  root.append_attribute("xmlns:s") = kSoapNamespace;
  root.append_attribute("xmlns:tt") = kSchemaNamespace;
  if (input.security) {
    auto header = root.append_child("s:Header");
    appendSecurity(header, *input.security);
  }
  auto body = root.append_child("s:Body");
  const std::string prefix(prefixOf(input.service));
  const std::string space(namespaceOf(input.service));
  auto operation = body.append_child((prefix + ":" + input.operation).c_str());
  operation.append_attribute(("xmlns:" + prefix).c_str()) = space.c_str();
  if (input.body)
    input.body(operation);
  std::ostringstream out;
  document.save(out, "", pugi::format_raw);
  return out.str();
}

std::optional<OnvifReply> onvif_soap::reply(std::string_view xml)
{
  auto document = std::make_shared<pugi::xml_document>();
  if (xml.empty() || !document->load_buffer(xml.data(), xml.size()))
    return std::nullopt;
  const pugi::xml_node first = firstElement(path(*document, {"Envelope", "Body"}));
  if (!first)
    return std::nullopt;
  OnvifReply out{.document = document,
                 .response = first,
                 .fault = localName(first) == "Fault",
                 .faultCodes = {},
                 .faultReason = {}};
  if (!out.fault)
    return out;
  for (pugi::xml_node level = child(first, "Code"); level; level = child(level, "Subcode")) {
    if (const std::string value = textOf(child(level, "Value")); !value.empty())
      out.faultCodes.emplace_back(afterColon(value));
  }
  if (const std::string legacy = textOf(child(first, "faultcode")); !legacy.empty())
    out.faultCodes.emplace_back(afterColon(legacy));
  out.faultReason = textOf(path(first, {"Reason", "Text"}));
  if (out.faultReason.empty())
    out.faultReason = textOf(child(first, "faultstring"));
  return out;
}

OnvifFailure onvif_soap::faultKind(const OnvifReply& reply)
{
  const auto names = [&reply](const auto& codes) {
    return std::ranges::any_of(reply.faultCodes, [&codes](const std::string& code) {
      return std::ranges::find(codes, code) != codes.end();
    });
  };
  if (names(kUnauthorizedCodes))
    return OnvifFailure::Unauthorized;
  if (names(kUnsupportedCodes))
    return OnvifFailure::Unsupported;
  return OnvifFailure::Fault;
}

std::string_view onvif_soap::localName(const pugi::xml_node& node)
{
  const std::string_view name(node.name());
  const auto colon = name.find(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

pugi::xml_node onvif_soap::child(const pugi::xml_node& parent, std::string_view local)
{
  for (const auto node : parent.children()) {
    if (node.type() == pugi::node_element && localName(node) == local)
      return node;
  }
  return {};
}

pugi::xml_node onvif_soap::path(const pugi::xml_node& from, std::initializer_list<std::string_view> locals)
{
  pugi::xml_node node = from;
  for (const auto local : locals) {
    node = child(node, local);
    if (!node)
      break;
  }
  return node;
}

std::string onvif_soap::textOf(const pugi::xml_node& node)
{
  const std::string_view text(node.child_value());
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos)
    return {};
  const auto last = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(first, last - first + 1));
}
