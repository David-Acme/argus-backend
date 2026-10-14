#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/camera-driver/camera-capabilities.hxx>
#include <shared/services/camera-driver/onvif-driver.hxx>
#include <shared/services/onvif/onvif-client.hxx>
#include <shared/services/onvif/onvif-parse.hxx>
#include <shared/services/onvif/onvif-soap.hxx>
#include <shared/services/tapo/tapo-crypto.hxx>

#include <scripted-onvif.hxx>

#include <drogon/drogon.h>
#include <trantor/net/EventLoopThread.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
constexpr int64_t kNow = 1791910800;
constexpr int64_t kDeviceTime = 1791911070;
constexpr const char* kUser = "admin";
constexpr const char* kPassword = "Secret!1";

using scripted_onvif::answer;
using scripted_onvif::fixedCamera;
using scripted_onvif::fixture;
using scripted_onvif::operationOf;
using scripted_onvif::ptzCamera;
using scripted_onvif::ScriptedOnvif;

OnvifEndpoint endpoint()
{
  return {.host = "192.168.1.64", .port = 80, .username = kUser, .password = kPassword, .timeoutSeconds = 1.0};
}

OnvifClient clientOver(const std::shared_ptr<IOnvifTransport>& transport)
{
  return OnvifClient(
      {.endpoint = endpoint(), .transport = transport, .asyncTransport = {}, .clock = [] { return kNow; }});
}

std::shared_ptr<pugi::xml_document> parsed(const std::string& xml)
{
  auto document = std::make_shared<pugi::xml_document>();
  REQUIRE(document->load_buffer(xml.data(), xml.size()));
  return document;
}

pugi::xml_node operationNode(const pugi::xml_document& document)
{
  for (const auto node : onvif_soap::path(document, {"Envelope", "Body"}).children()) {
    if (node.type() == pugi::node_element)
      return node;
  }
  return {};
}

pugi::xml_node tokenOf(const pugi::xml_document& document)
{
  return onvif_soap::path(document, {"Envelope", "Header", "Security", "UsernameToken"});
}

void checkSigned(const OnvifHttpRequest& request, const std::string& created)
{
  const auto document = parsed(request.body);
  const pugi::xml_node token = tokenOf(*document);
  REQUIRE(token);
  CHECK(onvif_soap::textOf(onvif_soap::child(token, "Username")) == kUser);
  const pugi::xml_node password = onvif_soap::child(token, "Password");
  CHECK(std::string(password.attribute("Type").value()) == onvif_soap::kPasswordDigestType);
  CHECK(onvif_soap::textOf(onvif_soap::child(token, "Created")) == created);
  const OnvifSecurity security{
      .username = kUser,
      .password = kPassword,
      .nonce = tapo_crypto::base64Decode(onvif_soap::textOf(onvif_soap::child(token, "Nonce"))),
      .created = created};
  CHECK(security.nonce.size() == 16);
  CHECK(onvif_soap::textOf(password) == onvif_soap::passwordDigest(security));
}

std::map<std::string, std::string> digestFields(const std::string& header)
{
  std::map<std::string, std::string> fields;
  const std::regex pair(R"re((\w+)=(?:"([^"]*)"|([^,\s]+)))re");
  for (auto it = std::sregex_iterator(header.begin(), header.end(), pair); it != std::sregex_iterator(); ++it)
    fields[(*it)[1].str()] = (*it)[2].matched ? (*it)[2].str() : (*it)[3].str();
  return fields;
}

std::string md5(const std::string& text)
{
  return tapo_crypto::toHex(tapo_crypto::md5Raw(text), false);
}

std::string expectedDigest(const std::map<std::string, std::string>& fields, const std::string& password)
{
  const std::string ha1 = md5(fields.at("username") + ":" + fields.at("realm") + ":" + password);
  const std::string ha2 = md5("POST:" + fields.at("uri"));
  return md5(ha1 + ":" + fields.at("nonce") + ":" + fields.at("nc") + ":" + fields.at("cnonce") + ":" +
             fields.at("qop") + ":" + ha2);
}

CameraSchema onvifCamera(const std::string& config, int64_t id = 0)
{
  CameraSchema camera;
  camera.id = id;
  camera.name = "Garage";
  camera.ip = "192.168.1.64";
  camera.port = 554;
  camera.username = kUser;
  camera.password = kPassword;
  camera.driver = CameraDriver::Onvif;
  camera.model = "PT-4MP-W";
  camera.config = config;
  return camera;
}
}

TEST_CASE("a PasswordDigest is the base64 SHA-1 of nonce, created and password, as in the ONVIF guide")
{
  const OnvifSecurity security{.username = "user",
                               .password = "userpassword",
                               .nonce = tapo_crypto::base64Decode("LKqI6G/AikKCQrN0zqZFlg=="),
                               .created = "2010-09-16T07:50:45Z"};
  CHECK(onvif_soap::passwordDigest(security) == "tuOSpGlFlIXsozq4HFNeeGeFLEI=");
  CHECK(onvif_soap::createdAt(kDeviceTime) == "2026-10-13T17:04:30Z");
  CHECK(onvif_soap::createdAt(0) == "1970-01-01T00:00:00Z");
}

TEST_CASE("an envelope names its operation in its service's namespace and signs only when asked")
{
  const OnvifSecurity security{.username = "a<d&m>\"in",
                               .password = kPassword,
                               .nonce = std::vector<uint8_t>(16, 7),
                               .created = "2026-10-13T17:04:30Z"};
  const std::string signedXml = onvif_soap::envelope(
      {.service = OnvifService::Ptz,
       .operation = "GetPresets",
       .body = [](pugi::xml_node& operation) {
         operation.append_child("tptz:ProfileToken").text().set("Profile_1");
       },
       .security = security,
       .header = {}});
  CHECK(signedXml.starts_with("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"));
  const auto document = parsed(signedXml);
  const pugi::xml_node envelope = onvif_soap::child(*document, "Envelope");
  CHECK(std::string(envelope.name()) == "s:Envelope");
  CHECK(std::string(envelope.attribute("xmlns:s").value()) == onvif_soap::kSoapNamespace);
  const pugi::xml_node operation = operationNode(*document);
  CHECK(std::string(operation.name()) == "tptz:GetPresets");
  CHECK(std::string(operation.attribute("xmlns:tptz").value()) == "http://www.onvif.org/ver20/ptz/wsdl");
  CHECK(onvif_soap::textOf(onvif_soap::child(operation, "ProfileToken")) == "Profile_1");
  const pugi::xml_node token = tokenOf(*document);
  CHECK(onvif_soap::textOf(onvif_soap::child(token, "Username")) == "a<d&m>\"in");
  CHECK(onvif_soap::textOf(onvif_soap::child(token, "Password")) == onvif_soap::passwordDigest(security));
  CHECK(tapo_crypto::base64Decode(onvif_soap::textOf(onvif_soap::child(token, "Nonce"))) == security.nonce);
  CHECK(onvif_soap::actionOf(OnvifService::Ptz, "GetPresets") == "http://www.onvif.org/ver20/ptz/wsdl/GetPresets");

  const auto plain = parsed(onvif_soap::envelope({.service = OnvifService::Device,
                                                  .operation = "GetSystemDateAndTime",
                                                  .body = {},
                                                  .security = std::nullopt,
                                                  .header = {}}));
  CHECK_FALSE(onvif_soap::path(*plain, {"Envelope", "Header"}));
  CHECK(std::string(operationNode(*plain).name()) == "tds:GetSystemDateAndTime");
}

TEST_CASE("an XAddr gives its path and never its host")
{
  CHECK(onvif_parse::pathOf("http://10.9.9.9:8899/onvif/ptz_service") == "/onvif/ptz_service");
  CHECK(onvif_parse::pathOf("http://192.168.1.64/onvif/media_service?profile=all") == "/onvif/media_service");
  CHECK(onvif_parse::pathOf("http://[fd00::5]:80/onvif/device_service") == "/onvif/device_service");
  CHECK(onvif_parse::pathOf("http://192.168.1.64") == "/");
  CHECK(onvif_parse::pathOf("/onvif/service") == "/onvif/service");
  CHECK(onvif_parse::pathOf("onvif/service").empty());
  CHECK(onvif_parse::pathOf("http://192.168.1.64/a b").empty());
  CHECK(onvif_parse::pathOf("").empty());
}

TEST_CASE("a subscription address keeps its query and its colons, never its host or its fragment")
{
  CHECK(onvif_parse::targetOf("http://192.168.1.64/onvif/Events/PullSubManager_2026-10-13T17:04:30Z_0") ==
        "/onvif/Events/PullSubManager_2026-10-13T17:04:30Z_0");
  CHECK(onvif_parse::targetOf("http://10.9.9.9:8000/onvif/Subscription?Idx=3#pull") == "/onvif/Subscription?Idx=3");
  CHECK(onvif_parse::targetOf("http://192.168.1.64") == "/");
  CHECK(onvif_parse::targetOf("http://192.168.1.64/a b").empty());
  CHECK(onvif_parse::targetOf("subscription").empty());
  CHECK(onvif_parse::epochOf("2026-10-13T17:04:35.500Z") == kDeviceTime + 5);
  CHECK(onvif_parse::epochOf("2026-10-13T12:04:36-05:00") == kDeviceTime + 6);
  CHECK_FALSE(onvif_parse::epochOf("PT60S").has_value());
  CHECK_FALSE(onvif_parse::epochOf("").has_value());
}

TEST_CASE("the device clock comes from UTCDateTime, and a broken date is no time")
{
  const auto clock = onvif_soap::reply(fixture("get-system-date-and-time.xml"));
  REQUIRE(clock.has_value());
  CHECK(onvif_parse::deviceEpoch(clock.value_or(OnvifReply{}).response) == kDeviceTime);
  const auto broken = onvif_soap::reply(
      R"(<s:Envelope xmlns:s="x" xmlns:t="y"><s:Body><t:GetSystemDateAndTimeResponse><t:SystemDateAndTime>)"
      R"(<t:UTCDateTime><t:Time><t:Hour>17</t:Hour><t:Minute>4</t:Minute><t:Second>30</t:Second></t:Time>)"
      R"(<t:Date><t:Year>2026</t:Year><t:Month>2</t:Month><t:Day>30</t:Day></t:Date></t:UTCDateTime>)"
      R"(</t:SystemDateAndTime></t:GetSystemDateAndTimeResponse></s:Body></s:Envelope>)");
  REQUIRE(broken.has_value());
  CHECK_FALSE(onvif_parse::deviceEpoch(broken.value_or(OnvifReply{}).response).has_value());
  CHECK_FALSE(onvif_soap::reply("<html><body>404</body></html>").has_value());
  CHECK_FALSE(onvif_soap::reply("").has_value());
}

TEST_CASE("connect reads the clock unsigned, then signs every call with the device's own time")
{
  const auto camera = ptzCamera();
  OnvifClient client = clientOver(camera);
  const auto connected = client.connect();
  REQUIRE(connected.ok());
  const OnvifDevice& device = connected.value;
  CHECK(device.info.manufacturer == "Example Optics");
  CHECK(device.info.model == "PT-4MP-W");
  CHECK(device.info.firmware == "V5.7.1 build 260517");
  CHECK(device.info.serial == "EX4MP20260517AAWR1234");
  CHECK(device.info.hardwareId == "88");
  CHECK(device.services.device == "/onvif/device_service");
  CHECK(device.services.media == "/onvif/media_service");
  CHECK(device.services.ptz == "/onvif/ptz_service");
  CHECK(device.services.imaging == "/onvif/imaging_service");
  CHECK(device.profileToken == "Profile_1");
  CHECK(device.ptz);
  CHECK(device.presets);
  CHECK(device.clockOffsetSeconds == kDeviceTime - kNow);

  CHECK(camera->operations() ==
        std::vector<std::string>{"GetSystemDateAndTime", "GetCapabilities", "GetDeviceInformation", "GetProfiles",
                                 "GetPresets"});
  REQUIRE(camera->sent.size() == 5);
  CHECK(camera->sent[0].path == "/onvif/device_service");
  CHECK_FALSE(tokenOf(*parsed(camera->sent[0].body)));
  CHECK(camera->sent[1].action == "http://www.onvif.org/ver10/device/wsdl/GetCapabilities");
  CHECK(onvif_soap::textOf(onvif_soap::child(operationNode(*parsed(camera->sent[1].body)), "Category")) == "All");
  CHECK(camera->sent[3].path == "/onvif/media_service");
  CHECK(camera->sent[3].action == "http://www.onvif.org/ver10/media/wsdl/GetProfiles");
  CHECK(camera->sent[4].path == "/onvif/ptz_service");
  CHECK(onvif_soap::textOf(onvif_soap::child(operationNode(*parsed(camera->sent[4].body)), "ProfileToken")) ==
        "Profile_1");
  for (size_t index = 1; index < camera->sent.size(); ++index) {
    CAPTURE(index);
    checkSigned(camera->sent[index], "2026-10-13T17:04:30Z");
    CHECK(camera->sent[index].authorization.empty());
  }
  CHECK(onvif_soap::textOf(onvif_soap::child(tokenOf(*parsed(camera->sent[1].body)), "Nonce")) !=
        onvif_soap::textOf(onvif_soap::child(tokenOf(*parsed(camera->sent[2].body)), "Nonce")));
}

TEST_CASE("the PTZ calls name the profile, the preset and the generic coordinates")
{
  const auto camera = ptzCamera();
  OnvifClient client = clientOver(camera);
  REQUIRE(client.connect().ok());
  camera->sent.clear();

  REQUIRE(client.continuousMove({.x = 0.5, .y = 0.0}).ok());
  REQUIRE(client.relativeMove({.x = 10.0 / 180.0, .y = -2.0}).ok());
  REQUIRE(client.stop().ok());
  REQUIRE(client.gotoPreset("2").ok());
  const auto saved = client.savePreset("Entrada & patio");
  REQUIRE(saved.ok());
  CHECK(saved.value == "3");
  REQUIRE(client.removePreset("2").ok());
  const auto stored = client.presets();
  REQUIRE(stored.ok());
  REQUIRE(stored.value.size() == 2);
  CHECK(stored.value[0].token == "1");
  CHECK(stored.value[0].name == "Puerta");
  CHECK(stored.value[1].token == "2");
  CHECK(stored.value[1].name == "Jardín & patio");

  REQUIRE(camera->sent.size() == 7);
  for (const auto& request : camera->sent) {
    CHECK(request.path == "/onvif/ptz_service");
    CHECK(onvif_soap::textOf(onvif_soap::child(operationNode(*parsed(request.body)), "ProfileToken")) ==
          "Profile_1");
  }
  const auto velocity = parsed(camera->sent[0].body);
  const pugi::xml_node moving = onvif_soap::path(operationNode(*velocity), {"Velocity", "PanTilt"});
  CHECK(std::string(moving.attribute("x").value()) == "0.5000");
  CHECK(std::string(moving.attribute("y").value()) == "0.0000");
  const pugi::xml_node lease = onvif_soap::child(operationNode(*velocity), "Timeout");
  REQUIRE(lease);
  CHECK(std::string(lease.name()) == "tptz:Timeout");
  CHECK(onvif_soap::textOf(lease) == "PT2S");
  CHECK(std::string(lease.previous_sibling().name()) == "tptz:Velocity");
  CHECK_FALSE(onvif_soap::child(operationNode(*parsed(camera->sent[1].body)), "Timeout"));
  const auto step = parsed(camera->sent[1].body);
  const pugi::xml_node stepping = onvif_soap::path(operationNode(*step), {"Translation", "PanTilt"});
  CHECK(std::string(stepping.attribute("x").value()) == "0.0556");
  CHECK(std::string(stepping.attribute("y").value()) == "-1.0000");
  CHECK(onvif_soap::textOf(onvif_soap::child(operationNode(*parsed(camera->sent[2].body)), "PanTilt")) == "true");
  CHECK(onvif_soap::textOf(onvif_soap::child(operationNode(*parsed(camera->sent[3].body)), "PresetToken")) == "2");
  CHECK(onvif_soap::textOf(onvif_soap::child(operationNode(*parsed(camera->sent[4].body)), "PresetName")) ==
        "Entrada & patio");
  CHECK(onvif_soap::textOf(onvif_soap::child(operationNode(*parsed(camera->sent[5].body)), "PresetToken")) == "2");
}

TEST_CASE("an HTTP digest challenge is answered once, then sent ahead with the next count")
{
  const auto camera = ptzCamera();
  camera->once("GetCapabilities",
               {.status = 401,
                .body = "Unauthorized",
                .challenge = R"re(Digest realm="IP Camera(C2290)", nonce="4e5a4c6c", qop="auth,auth-int", )re"
                             R"re(opaque="7a2b", algorithm=MD5)re",
                .error = {}});
  OnvifClient client = clientOver(camera);
  REQUIRE(client.connect().ok());
  CHECK(camera->operations() ==
        std::vector<std::string>{"GetSystemDateAndTime", "GetCapabilities", "GetCapabilities",
                                 "GetDeviceInformation", "GetProfiles", "GetPresets"});
  REQUIRE(camera->sent.size() == 6);
  CHECK(camera->sent[1].authorization.empty());
  const auto first = digestFields(camera->sent[2].authorization);
  CHECK(camera->sent[2].authorization.starts_with("Digest "));
  CHECK(first.at("username") == kUser);
  CHECK(first.at("realm") == "IP Camera(C2290)");
  CHECK(first.at("nonce") == "4e5a4c6c");
  CHECK(first.at("uri") == "/onvif/device_service");
  CHECK(first.at("qop") == "auth");
  CHECK(first.at("nc") == "00000001");
  CHECK(first.at("opaque") == "7a2b");
  CHECK(first.at("response") == expectedDigest(first, kPassword));
  const auto next = digestFields(camera->sent[3].authorization);
  CHECK(next.at("nc") == "00000002");
  CHECK(next.at("response") == expectedDigest(next, kPassword));
  const auto media = digestFields(camera->sent[4].authorization);
  CHECK(media.at("uri") == "/onvif/media_service");
  CHECK(media.at("response") == expectedDigest(media, kPassword));
  CHECK(onvif_soap::textOf(onvif_soap::child(tokenOf(*parsed(camera->sent[1].body)), "Nonce")) !=
        onvif_soap::textOf(onvif_soap::child(tokenOf(*parsed(camera->sent[2].body)), "Nonce")));
}

TEST_CASE("a refused account, a missing feature and an answer that is not SOAP are told apart")
{
  {
    const auto camera = ptzCamera();
    camera->once("GetCapabilities", answer("fault-not-authorized.xml", 400));
    OnvifClient client = clientOver(camera);
    const auto refused = client.connect();
    CHECK(refused.failure == OnvifFailure::Unauthorized);
    CHECK(refused.error.find("Sender not Authorized") != std::string::npos);
    CHECK_FALSE(client.device().has_value());
  }
  {
    const auto camera = ptzCamera();
    camera->once("GetCapabilities", {.status = 401, .body = {}, .challenge = {}, .error = {}});
    OnvifClient client = clientOver(camera);
    CHECK(client.connect().failure == OnvifFailure::Unauthorized);
  }
  {
    const auto camera = ptzCamera();
    const OnvifHttpResponse challenge{
        .status = 401, .body = {}, .challenge = R"(Digest realm="r", nonce="n")", .error = {}};
    camera->once("GetCapabilities", challenge);
    camera->once("GetCapabilities", challenge);
    OnvifClient client = clientOver(camera);
    CHECK(client.connect().failure == OnvifFailure::Unauthorized);
    CHECK(camera->sent.size() == 3);
  }
  {
    const auto camera = ptzCamera();
    camera->always("GetPresets", answer("fault-action-not-supported.xml", 500));
    OnvifClient client = clientOver(camera);
    const auto connected = client.connect();
    REQUIRE(connected.ok());
    CHECK(connected.value.ptz);
    CHECK_FALSE(connected.value.presets);
    const auto stored = client.presets();
    CHECK(stored.failure == OnvifFailure::Unsupported);
    CHECK(stored.error.find("PresetsNotSupported") != std::string::npos);
  }
  {
    const auto camera = ptzCamera();
    camera->once("GetDeviceInformation", {.status = 500, .body = "<html>busy</html>", .challenge = {}, .error = {}});
    OnvifClient client = clientOver(camera);
    CHECK(client.connect().failure == OnvifFailure::Malformed);
  }
  {
    const auto silent = std::make_shared<ScriptedOnvif>();
    OnvifClient client = clientOver(silent);
    const auto unreachable = client.connect();
    CHECK(unreachable.failure == OnvifFailure::Transport);
    CHECK(silent->sent.size() == 1);
  }
}

TEST_CASE("a lost camera or a refused account is reconnected from its clock on the next call")
{
  const auto camera = ptzCamera();
  OnvifDriver driver(onvifCamera("{}"), camera);
  REQUIRE(driver.presets().ok);
  camera->once("GotoPreset",
               {.status = 0, .body = {}, .challenge = {}, .error = "no ONVIF answer from 192.168.1.64:80 (Timeout)"});
  const DriverResult lost = driver.preset({.action = "goto", .id = "1", .name = {}});
  CHECK_FALSE(lost.ok);
  CHECK(lost.error.find("no ONVIF answer") != std::string::npos);
  camera->sent.clear();
  REQUIRE(driver.preset({.action = "goto", .id = "1", .name = {}}).ok);
  CHECK(camera->operations() == std::vector<std::string>{"GetSystemDateAndTime", "GetCapabilities",
                                                         "GetDeviceInformation", "GetProfiles", "GetPresets",
                                                         "GotoPreset"});

  camera->once("Stop", {.status = 401, .body = {}, .challenge = {}, .error = {}});
  const DriverResult refused =
      driver.move({.x = std::nullopt, .y = std::nullopt, .angle = std::nullopt, .stop = true});
  CHECK_FALSE(refused.ok);
  CHECK(refused.failure == "credential_refused");
  camera->sent.clear();
  REQUIRE(driver.move({.x = std::nullopt, .y = std::nullopt, .angle = std::nullopt, .stop = true}).ok);
  REQUIRE_FALSE(camera->sent.empty());
  CHECK(camera->operations().front() == "GetSystemDateAndTime");
  CHECK(camera->operations().back() == "Stop");

  camera->once("RelativeMove", answer("fault-action-not-supported.xml", 500));
  CHECK_FALSE(driver.move({.x = 10, .y = 0, .angle = std::nullopt, .stop = false}).ok);
  camera->sent.clear();
  REQUIRE(driver.move({.x = std::nullopt, .y = std::nullopt, .angle = std::nullopt, .stop = true}).ok);
  CHECK(camera->operations() == std::vector<std::string>{"Stop"});
}

TEST_CASE("a forgotten session forgets the camera's digest challenge too")
{
  const auto camera = ptzCamera();
  camera->once("GetCapabilities",
               {.status = 401, .body = {}, .challenge = R"(Digest realm="r", nonce="n1", qop="auth")", .error = {}});
  OnvifClient client = clientOver(camera);
  REQUIRE(client.connect().ok());
  CHECK_FALSE(camera->sent.back().authorization.empty());
  camera->once("Stop", {.status = 0, .body = {}, .challenge = {}, .error = "no answer"});
  CHECK(client.stop().failure == OnvifFailure::Transport);
  CHECK_FALSE(client.device().has_value());
  CHECK(client.stop().failure == OnvifFailure::Unsupported);
  camera->sent.clear();
  REQUIRE(client.connect().ok());
  CHECK(camera->operations().front() == "GetSystemDateAndTime");
  for (const auto& request : camera->sent)
    CHECK(request.authorization.empty());
}

TEST_CASE("a camera without pan and tilt still connects, and every PTZ call says so without asking it")
{
  const auto camera = fixedCamera();
  OnvifClient client = clientOver(camera);
  const auto connected = client.connect();
  REQUIRE(connected.ok());
  CHECK_FALSE(connected.value.ptz);
  CHECK_FALSE(connected.value.presets);
  CHECK(connected.value.profileToken == "profile_1");
  CHECK(connected.value.services.media == "/onvif/service");
  CHECK(connected.value.services.ptz.empty());
  const size_t before = camera->sent.size();
  CHECK(client.presets().failure == OnvifFailure::Unsupported);
  CHECK(client.stop().failure == OnvifFailure::Unsupported);
  CHECK(client.presets().error == "This camera has no pan and tilt over ONVIF");
  CHECK(camera->sent.size() == before);

  OnvifDriver driver(onvifCamera(R"({"catalogId":"onvif"})"), fixedCamera());
  const Json::Value discovered = driver.discoverCapabilities();
  CHECK_FALSE(discovered["ptz"].asBool());
  CHECK_FALSE(discovered["presets"].asBool());
  CHECK(discovered["streamOnly"].asBool());
  CHECK(discovered["catalogId"].asString() == "onvif");
  CHECK_FALSE(driver.move({.x = std::nullopt, .y = std::nullopt, .angle = std::nullopt, .stop = true}).ok);
}

TEST_CASE("a camera's own ONVIF port wins over its model's, and 80 is the last default")
{
  CHECK(OnvifDriver::endpointOf(onvifCamera(R"({"catalogId":"vigi","onvifPort":8899})")).port == 8899);
  CHECK(OnvifDriver::endpointOf(onvifCamera(R"({"onvifPort":8899})")).port == 8899);
  CHECK(OnvifDriver::endpointOf(onvifCamera(R"({"catalogId":"vigi","onvifPort":0})")).port == 2020);
  CHECK(OnvifDriver::endpointOf(onvifCamera(R"({"catalogId":"vigi","onvifPort":"8899"})")).port == 2020);
  CHECK(OnvifDriver::endpointOf(onvifCamera(R"({"onvifPort":70000})")).port == 80);
  CHECK(OnvifDriver::portOf(onvifCamera(R"({"catalogId":"reolink","onvifPort":8000})")) == 8000);
}

TEST_CASE("the ONVIF driver answers the app in the shapes it already reads")
{
  CHECK(OnvifDriver::endpointOf(onvifCamera(R"({"catalogId":"vigi"})")).port == 2020);
  CHECK(OnvifDriver::endpointOf(onvifCamera(R"({"catalogId":"reolink"})")).port == 8000);
  CHECK(OnvifDriver::endpointOf(onvifCamera(R"({"catalogId":"tapo-c225"})")).port == 2020);
  CHECK(OnvifDriver::endpointOf(onvifCamera("{}")).port == 80);
  const OnvifEndpoint target = OnvifDriver::endpointOf(onvifCamera("{}"));
  CHECK(target.host == "192.168.1.64");
  CHECK(target.username == kUser);
  CHECK(target.password == kPassword);

  const auto camera = ptzCamera();
  OnvifDriver driver(onvifCamera(R"({"catalogId":"onvif"})"), camera);
  const Json::Value before = driver.capabilities();
  CHECK_FALSE(before["ptz"].asBool());
  CHECK(before["streamOnly"].asBool());
  CHECK(before["holdRenewMs"].asInt() == 1000);
  CHECK(camera->sent.empty());

  const Json::Value discovered = driver.discoverCapabilities();
  CHECK(discovered["ptz"].asBool());
  CHECK(discovered["presets"].asBool());
  CHECK_FALSE(discovered["streamOnly"].asBool());
  CHECK(discovered["holdRenewMs"].asInt() == 1000);
  CHECK(camera_capabilities::listOf(discovered) == R"(["ptz","presets"])");
  CHECK_FALSE(discovered["talk"].asBool());
  CHECK_FALSE(discovered["privacy"].asBool());
  CHECK(driver.capabilities() == discovered);
  CHECK(camera->sent.size() == 5);

  const DriverResult status = driver.status();
  REQUIRE(status.ok);
  CHECK(status.data["model"].asString() == "PT-4MP-W");
  CHECK(status.data["manufacturer"].asString() == "Example Optics");
  CHECK(status.data["firmware"].asString() == "V5.7.1 build 260517");
  CHECK(status.data["hardwareVersion"].asString() == "88");
  CHECK(status.data["serial"].asString() == "EX4MP20260517AAWR1234");
  const int64_t serverNow = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
  CHECK(std::abs(status.data["clockOffsetSeconds"].asInt64() - (kDeviceTime - serverNow)) <= 5);
  CHECK_FALSE(status.data["streamOnly"].asBool());
  CHECK(camera->sent.size() == 5);

  const DriverResult presets = driver.presets();
  REQUIRE(presets.ok);
  CHECK(presets.data["preset"]["id"].size() == 2);
  CHECK(presets.data["preset"]["id"][0].asString() == "1");
  CHECK(presets.data["preset"]["name"][1].asString() == "Jardín & patio");

  const auto lastPanTilt = [&camera](const char* holder) {
    const auto document = parsed(camera->sent.back().body);
    const pugi::xml_node pan = onvif_soap::path(operationNode(*document), {holder, "PanTilt"});
    return std::string(pan.attribute("x").value()) + "," + pan.attribute("y").value();
  };
  const DriverResult held = driver.move({.x = std::nullopt, .y = std::nullopt, .angle = 90, .stop = false});
  REQUIRE(held.ok);
  CHECK(held.data["moved"].asBool());
  CHECK_FALSE(held.data["limit"].asBool());
  CHECK(lastPanTilt("Velocity") == "0.0000,0.5000");
  CHECK(onvif_soap::textOf(onvif_soap::child(operationNode(*parsed(camera->sent.back().body)), "Timeout")) ==
        "PT2S");
  REQUIRE(driver.move({.x = std::nullopt, .y = std::nullopt, .angle = 180, .stop = false}).ok);
  CHECK(lastPanTilt("Velocity") == "-0.5000,0.0000");
  REQUIRE(driver.move({.x = std::nullopt, .y = std::nullopt, .angle = 270, .stop = false}).ok);
  CHECK(lastPanTilt("Velocity") == "0.0000,-0.5000");
  REQUIRE(driver.move({.x = 10, .y = 0, .angle = std::nullopt, .stop = false}).ok);
  CHECK(lastPanTilt("Translation") == "0.0556,0.0000");
  REQUIRE(driver.move({.x = 0, .y = -45, .angle = std::nullopt, .stop = false}).ok);
  CHECK(lastPanTilt("Translation") == "0.0000,-0.5000");
  REQUIRE(driver.move({.x = std::nullopt, .y = std::nullopt, .angle = std::nullopt, .stop = true}).ok);
  CHECK(operationOf(camera->sent.back().action) == "Stop");

  const DriverResult saved = driver.preset({.action = "save", .id = {}, .name = "Entrada"});
  REQUIRE(saved.ok);
  CHECK(saved.data["id"].asString() == "3");
  REQUIRE(driver.preset({.action = "goto", .id = "1", .name = {}}).ok);
  CHECK(operationOf(camera->sent.back().action) == "GotoPreset");
  REQUIRE(driver.preset({.action = "delete", .id = "1", .name = {}}).ok);
  CHECK(operationOf(camera->sent.back().action) == "RemovePreset");

  const size_t sent = camera->sent.size();
  CHECK_FALSE(driver.settings({.privacy = true,
                               .led = std::nullopt,
                               .dayNight = std::nullopt,
                               .motion = std::nullopt,
                               .motionSensitivity = std::nullopt,
                               .autoTrack = std::nullopt,
                               .alarm = std::nullopt,
                               .alarmVolume = std::nullopt,
                               .sounding = std::nullopt})
                  .ok);
  CHECK_FALSE(driver.speak({.samples = {}, .sampleRate = 16000}).ok);
  CHECK(camera->sent.size() == sent);

  const auto refusing = ptzCamera();
  refusing->once("GetCapabilities", answer("fault-not-authorized.xml", 400));
  OnvifDriver refused(onvifCamera("{}"), refusing);
  const DriverResult denied = refused.status();
  CHECK_FALSE(denied.ok);
  CHECK(denied.failure == "credential_refused");
}

namespace
{
constexpr const char* kEventProperties =
    "http://www.onvif.org/ver10/events/wsdl/EventPortType/GetEventPropertiesRequest";
constexpr const char* kCreatePullPoint =
    "http://www.onvif.org/ver10/events/wsdl/EventPortType/CreatePullPointSubscriptionRequest";
constexpr const char* kPullMessages = "http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/PullMessagesRequest";
constexpr const char* kRenew = "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/RenewRequest";
constexpr const char* kUnsubscribe = "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/UnsubscribeRequest";
constexpr const char* kAxisEvents = "http://www.axis.com/2009/event";

OnvifClient eventsClientOver(const std::shared_ptr<ScriptedOnvif>& camera)
{
  return OnvifClient(
      {.endpoint = endpoint(), .transport = {}, .asyncTransport = camera, .clock = [] { return kNow; }});
}

std::shared_ptr<ScriptedOnvif> eventsCamera()
{
  auto camera = ptzCamera();
  camera->always("GetEventPropertiesRequest", answer("get-event-properties.xml"));
  camera->always("CreatePullPointSubscriptionRequest", answer("create-pull-point-subscription.xml"));
  camera->always("PullMessagesRequest", answer("pull-messages-motion.xml"));
  camera->always("RenewRequest", answer("renew.xml"));
  camera->always("UnsubscribeRequest", answer("unsubscribe.xml"));
  return camera;
}

pugi::xml_node headerOf(const pugi::xml_document& document)
{
  return onvif_soap::path(document, {"Envelope", "Header"});
}

void checkAddressed(const OnvifHttpRequest& request, const OnvifSubscription& subscription)
{
  const auto document = parsed(request.body);
  const pugi::xml_node action = onvif_soap::child(headerOf(*document), "Action");
  CHECK(onvif_soap::textOf(action) == request.action);
  CHECK(std::string(action.attribute("xmlns:wsa").value()) == onvif_soap::kAddressingNamespace);
  const pugi::xml_node to = onvif_soap::child(headerOf(*document), "To");
  CHECK(onvif_soap::textOf(to) == subscription.address);
  CHECK(std::string(to.attribute("xmlns:wsa").value()) == onvif_soap::kAddressingNamespace);
  CHECK(request.path == subscription.path);
}
}

TEST_CASE("events open from the device clock and the events path, and a camera without one says so")
{
  const auto camera = eventsCamera();
  OnvifClient client = eventsClientOver(camera);
  REQUIRE(drogon::sync_wait(client.openEvents()).ok());
  CHECK(camera->operations() ==
        std::vector<std::string>{"GetSystemDateAndTime", "GetCapabilities", "GetEventPropertiesRequest"});
  CHECK_FALSE(tokenOf(*parsed(camera->sent[0].body)));
  checkSigned(camera->sent[1], "2026-10-13T17:04:30Z");
  CHECK(camera->sent[2].path == "/onvif/event_service");
  CHECK(camera->sent[2].action == kEventProperties);
  checkSigned(camera->sent[2], "2026-10-13T17:04:30Z");
  CHECK(std::string(operationNode(*parsed(camera->sent[2].body)).name()) == "tev:GetEventProperties");

  const auto made = drogon::sync_wait(client.subscribe());
  REQUIRE(made.ok());
  const OnvifHttpRequest& request = camera->sent.back();
  CHECK(request.path == "/onvif/event_service");
  CHECK(request.action == kCreatePullPoint);
  CHECK(request.timeoutSeconds == 0.0);
  checkSigned(request, "2026-10-13T17:04:30Z");
  const auto document = parsed(request.body);
  CHECK_FALSE(onvif_soap::child(headerOf(*document), "Action"));
  const pugi::xml_node operation = operationNode(*document);
  CHECK(std::string(operation.name()) == "tev:CreatePullPointSubscription");
  CHECK(std::string(operation.attribute("xmlns:tev").value()) == "http://www.onvif.org/ver10/events/wsdl");
  CHECK(onvif_soap::textOf(onvif_soap::child(operation, "InitialTerminationTime")) == "PT60S");
  CHECK_FALSE(onvif_soap::child(operation, "Filter"));
  CHECK(made.value.address == "http://192.168.1.64/onvif/Events/PullSubManager_2026-10-13T17:04:30Z_0");
  CHECK(made.value.path == "/onvif/Events/PullSubManager_2026-10-13T17:04:30Z_0");
  CHECK(made.value.references.empty());
  CHECK(made.value.lifetimeSeconds == 60);

  const auto fixed = fixedCamera();
  OnvifClient bare = eventsClientOver(fixed);
  const auto refused = drogon::sync_wait(bare.openEvents());
  CHECK(refused.failure == OnvifFailure::Unsupported);
  CHECK(drogon::sync_wait(bare.subscribe()).failure == OnvifFailure::Unsupported);
  CHECK(fixed->operations() == std::vector<std::string>{"GetSystemDateAndTime", "GetCapabilities"});

  const auto unnamed = eventsCamera();
  unnamed->always("CreatePullPointSubscriptionRequest", answer("stop.xml"));
  OnvifClient nameless = eventsClientOver(unnamed);
  REQUIRE(drogon::sync_wait(nameless.openEvents()).ok());
  CHECK(drogon::sync_wait(nameless.subscribe()).failure == OnvifFailure::Malformed);
}

TEST_CASE("a camera that names no motion topic is not subscribed, and one that cannot say still is")
{
  const auto still = eventsCamera();
  still->always("GetEventPropertiesRequest", answer("get-event-properties-no-motion.xml"));
  OnvifClient client = eventsClientOver(still);
  const auto refused = drogon::sync_wait(client.openEvents());
  CHECK(refused.failure == OnvifFailure::Unsupported);
  CHECK(refused.error == "The camera publishes no ONVIF motion topic");
  CHECK(still->operations() ==
        std::vector<std::string>{"GetSystemDateAndTime", "GetCapabilities", "GetEventPropertiesRequest"});

  const auto unsure = eventsCamera();
  unsure->always("GetEventPropertiesRequest", answer("fault-action-not-supported.xml", 500));
  OnvifClient generous = eventsClientOver(unsure);
  CHECK(drogon::sync_wait(generous.openEvents()).ok());
  CHECK(drogon::sync_wait(generous.subscribe()).ok());

  const auto lost = eventsCamera();
  lost->once("GetEventPropertiesRequest", {.status = 0, .body = {}, .challenge = {}, .error = "connection refused"});
  OnvifClient dropped = eventsClientOver(lost);
  CHECK(drogon::sync_wait(dropped.openEvents()).failure == OnvifFailure::Transport);
  const size_t asked = lost->sent.size();
  CHECK(drogon::sync_wait(dropped.subscribe()).failure == OnvifFailure::Unsupported);
  CHECK(lost->sent.size() == asked);
}

TEST_CASE("a pull goes to the subscription itself, addressed and signed, and reads only motion")
{
  const auto camera = eventsCamera();
  camera->always("CreatePullPointSubscriptionRequest", answer("create-pull-point-subscription-referenced.xml"));
  OnvifClient client = eventsClientOver(camera);
  REQUIRE(drogon::sync_wait(client.openEvents()).ok());
  const auto made = drogon::sync_wait(client.subscribe());
  REQUIRE(made.ok());
  CHECK(made.value.path == "/onvif/Subscription?Idx=3");
  CHECK(made.value.lifetimeSeconds == 60);

  const auto pulled = drogon::sync_wait(client.pull(made.value));
  REQUIRE(pulled.ok());
  const OnvifHttpRequest& request = camera->sent.back();
  CHECK(request.action == kPullMessages);
  CHECK(request.timeoutSeconds == 15.0);
  checkSigned(request, "2026-10-13T17:04:30Z");
  checkAddressed(request, made.value);
  const auto document = parsed(request.body);
  const pugi::xml_node reference = onvif_soap::child(headerOf(*document), "SubscriptionId");
  CHECK(onvif_soap::textOf(reference) == "3");
  CHECK(std::string(reference.attribute("xmlns:dom0").value()) == kAxisEvents);
  CHECK_FALSE(reference.attribute("xmlns:SOAP-ENV"));
  const pugi::xml_node operation = operationNode(*document);
  CHECK(std::string(operation.name()) == "tev:PullMessages");
  CHECK(onvif_soap::textOf(onvif_soap::child(operation, "Timeout")) == "PT10S");
  CHECK(onvif_soap::textOf(onvif_soap::child(operation, "MessageLimit")) == "10");

  REQUIRE(pulled.value.motions.size() == 4);
  CHECK_FALSE(pulled.value.motions[0].moving);
  CHECK(pulled.value.motions[1].moving);
  CHECK(pulled.value.motions[2].moving);
  CHECK_FALSE(pulled.value.motions[3].moving);
  CHECK(pulled.value.lifetimeSeconds == 60);

  camera->always("PullMessagesRequest", answer("pull-messages-empty.xml"));
  const auto quiet = drogon::sync_wait(client.pull(made.value));
  REQUIRE(quiet.ok());
  CHECK(quiet.value.motions.empty());
  CHECK(quiet.value.lifetimeSeconds == 60);
}

TEST_CASE("renew and unsubscribe speak WS-BaseNotification to the subscription manager")
{
  const auto camera = eventsCamera();
  OnvifClient client = eventsClientOver(camera);
  REQUIRE(drogon::sync_wait(client.openEvents()).ok());
  const auto made = drogon::sync_wait(client.subscribe());
  REQUIRE(made.ok());

  const auto renewed = drogon::sync_wait(client.renew(made.value));
  REQUIRE(renewed.ok());
  CHECK(renewed.value == 60);
  const OnvifHttpRequest renewal = camera->sent.back();
  CHECK(renewal.action == kRenew);
  CHECK(renewal.timeoutSeconds == 0.0);
  checkSigned(renewal, "2026-10-13T17:04:30Z");
  checkAddressed(renewal, made.value);
  const auto renewDocument = parsed(renewal.body);
  const pugi::xml_node renew = operationNode(*renewDocument);
  CHECK(std::string(renew.name()) == "wsnt:Renew");
  CHECK(std::string(renew.attribute("xmlns:wsnt").value()) == "http://docs.oasis-open.org/wsn/b-2");
  CHECK(onvif_soap::textOf(onvif_soap::child(renew, "TerminationTime")) == "PT60S");

  REQUIRE(drogon::sync_wait(client.unsubscribe(made.value)).ok());
  const OnvifHttpRequest goodbye = camera->sent.back();
  CHECK(goodbye.action == kUnsubscribe);
  CHECK(goodbye.timeoutSeconds == 2.0);
  checkSigned(goodbye, "2026-10-13T17:04:30Z");
  checkAddressed(goodbye, made.value);
  const auto goodbyeDocument = parsed(goodbye.body);
  CHECK(std::string(operationNode(*goodbyeDocument).name()) == "wsnt:Unsubscribe");
  CHECK(camera->operations() == std::vector<std::string>{"GetSystemDateAndTime", "GetCapabilities",
                                                         "GetEventPropertiesRequest",
                                                         "CreatePullPointSubscriptionRequest", "RenewRequest",
                                                         "UnsubscribeRequest"});
}

TEST_CASE("the events calls answer the digest challenge like every call, and a lost camera is opened again")
{
  const auto camera = eventsCamera();
  camera->once("CreatePullPointSubscriptionRequest",
               {.status = 401, .body = {}, .challenge = R"(Digest realm="r", nonce="n1", qop="auth")", .error = {}});
  OnvifClient client = eventsClientOver(camera);
  REQUIRE(drogon::sync_wait(client.openEvents()).ok());
  const auto made = drogon::sync_wait(client.subscribe());
  REQUIRE(made.ok());
  REQUIRE(camera->sent.size() == 5);
  CHECK(camera->sent[3].authorization.empty());
  const auto first = digestFields(camera->sent[4].authorization);
  CHECK(first.at("uri") == "/onvif/event_service");
  CHECK(first.at("nc") == "00000001");
  CHECK(first.at("response") == expectedDigest(first, kPassword));

  REQUIRE(drogon::sync_wait(client.pull(made.value)).ok());
  const auto pulled = digestFields(camera->sent.back().authorization);
  CHECK(pulled.at("uri") == made.value.path);
  CHECK(pulled.at("nc") == "00000002");
  CHECK(pulled.at("response") == expectedDigest(pulled, kPassword));

  camera->once("PullMessagesRequest", {.status = 0, .body = {}, .challenge = {}, .error = "connection reset"});
  const auto lost = drogon::sync_wait(client.pull(made.value));
  CHECK(lost.failure == OnvifFailure::Transport);
  CHECK(lost.error == "connection reset");
  const size_t asked = camera->sent.size();
  CHECK(drogon::sync_wait(client.subscribe()).failure == OnvifFailure::Unsupported);
  CHECK(camera->sent.size() == asked);
  REQUIRE(drogon::sync_wait(client.openEvents()).ok());
  REQUIRE(drogon::sync_wait(client.subscribe()).ok());
  CHECK(camera->sent.back().authorization.empty());

  camera->once("RenewRequest", answer("fault-not-authorized.xml", 400));
  CHECK(drogon::sync_wait(client.renew(made.value)).failure == OnvifFailure::Unauthorized);
  CHECK(drogon::sync_wait(client.subscribe()).failure == OnvifFailure::Unsupported);

  camera->once("UnsubscribeRequest", answer("fault-action-not-supported.xml", 500));
  CHECK(drogon::sync_wait(client.unsubscribe(made.value)).failure == OnvifFailure::Unsupported);
}

TEST_CASE("a client sends only over the transports it was given")
{
  const auto camera = eventsCamera();
  OnvifClient client = clientOver(camera);
  CHECK(drogon::sync_wait(client.openEvents()).failure == OnvifFailure::Unsupported);
  CHECK(drogon::sync_wait(client.subscribe()).failure == OnvifFailure::Unsupported);
  const OnvifSubscription subscription{
      .address = "http://192.168.1.64/onvif/Subscription", .path = "/onvif/Subscription", .references = {},
      .lifetimeSeconds = 60};
  CHECK(drogon::sync_wait(client.pull(subscription)).failure == OnvifFailure::Unsupported);
  CHECK(drogon::sync_wait(client.renew(subscription)).failure == OnvifFailure::Unsupported);
  CHECK(drogon::sync_wait(client.unsubscribe(subscription)).failure == OnvifFailure::Unsupported);
  CHECK(camera->sent.empty());
  REQUIRE(client.connect().ok());

  const auto listener = eventsCamera();
  OnvifClient eventsOnly = eventsClientOver(listener);
  const auto refused = eventsOnly.connect();
  CHECK(refused.failure == OnvifFailure::Unsupported);
  CHECK(refused.error == "This ONVIF client has no transport for that call");
  CHECK(listener->sent.empty());
}

namespace
{
struct SeenOnvif
{
  std::vector<std::string> contentTypes;
  std::vector<std::string> paths;
  std::vector<std::string> targets;
  int challenged{0};
  int answered{0};
};

constexpr const char* kManagerPath = "/onvif/Events/PullSubManager_2026-10-13T17:04:30Z_0";

class FakeOnvifCamera
{
public:
  void install()
  {
    for (const char* path :
         {"/onvif/device_service", "/onvif/media_service", "/onvif/ptz_service", "/onvif/event_service", kManagerPath}) {
      drogon::app().registerHandler(
          path,
          [this](const drogon::HttpRequestPtr& request,
                 std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(respond(request));
          },
          {drogon::Post});
    }
  }

  [[nodiscard]] SeenOnvif seen() const
  {
    const std::scoped_lock lock(mutex_);
    return seen_;
  }

private:
  drogon::HttpResponsePtr respond(const drogon::HttpRequestPtr& request)
  {
    const std::string contentType = request->getHeader("content-type");
    const std::string authorization = request->getHeader("authorization");
    const std::smatch found = matchAction(contentType);
    const std::string operation = found.size() > 1 ? operationOf(found[1].str()) : std::string();
    const std::string target = request->getOriginalPath() + (request->query().empty() ? "" : "?" + request->query());
    const std::scoped_lock lock(mutex_);
    seen_.contentTypes.push_back(contentType);
    seen_.paths.emplace_back(request->path());
    seen_.targets.push_back(target);
    const bool open = operation == "GetSystemDateAndTime";
    if (!open && !authorized(authorization, target)) {
      ++seen_.challenged;
      auto refusal = drogon::HttpResponse::newHttpResponse();
      refusal->setStatusCode(drogon::k401Unauthorized);
      refusal->addHeader("WWW-Authenticate", R"(Digest realm="argus-fake", nonce="0a1b2c3d", qop="auth")");
      return refusal;
    }
    ++seen_.answered;
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setContentTypeString("application/soap+xml; charset=utf-8");
    response->setBody(fixtureFor(operation));
    return response;
  }

  static std::smatch matchAction(const std::string& contentType)
  {
    static const std::regex action(R"re(action="([^"]+)")re");
    std::smatch found;
    std::regex_search(contentType, found, action);
    return found;
  }

  static bool authorized(const std::string& header, std::string_view path)
  {
    if (!header.starts_with("Digest "))
      return false;
    const auto fields = digestFields(header);
    return fields.contains("response") && fields.contains("uri") && fields.at("uri") == path &&
           fields.contains("nonce") && fields.at("nonce") == "0a1b2c3d" && fields.contains("nc") &&
           fields.contains("cnonce") && fields.contains("qop") &&
           fields.at("response") == expectedDigest(fields, kPassword);
  }

  static std::string fixtureFor(const std::string& operation)
  {
    static const std::map<std::string, std::string> kFiles{
        {"GetSystemDateAndTime", "get-system-date-and-time.xml"},
        {"GetCapabilities", "get-capabilities.xml"},
        {"GetDeviceInformation", "get-device-information.xml"},
        {"GetProfiles", "get-profiles.xml"},
        {"GetPresets", "get-presets.xml"},
        {"Stop", "stop.xml"},
        {"CreatePullPointSubscriptionRequest", "create-pull-point-subscription.xml"},
        {"GetEventPropertiesRequest", "get-event-properties.xml"},
        {"PullMessagesRequest", "pull-messages-motion.xml"}};
    const auto file = kFiles.find(operation);
    return file == kFiles.end() ? fixture("fault-action-not-supported.xml") : fixture(file->second);
  }

  mutable std::mutex mutex_;
  SeenOnvif seen_;
};

bool waitForBoot(std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (drogon::app().isRunning())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return drogon::app().isRunning();
}
}

TEST_CASE("over HTTP the transport posts SOAP 1.2 with its action and answers the camera's digest")
{
  FakeOnvifCamera fake;
  fake.install();
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addListener("127.0.0.1", 0);
  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  const auto listeners = drogon::app().getListeners();
  REQUIRE_FALSE(listeners.empty());

  OnvifClient client({.endpoint = {.host = "127.0.0.1",
                                   .port = listeners.front().toPort(),
                                   .username = kUser,
                                   .password = kPassword,
                                   .timeoutSeconds = 5.0},
                      .transport = std::make_shared<OnvifHttpTransport>(OnvifEndpoint{
                          .host = "127.0.0.1",
                          .port = listeners.front().toPort(),
                          .username = kUser,
                          .password = kPassword,
                          .timeoutSeconds = 5.0}),
                      .asyncTransport = {},
                      .clock = {}});
  const auto connected = client.connect();
  CHECK(connected.ok());
  CHECK(connected.value.ptz);
  CHECK(connected.value.presets);
  CHECK(connected.value.info.model == "PT-4MP-W");
  CHECK(client.stop().ok());

  const SeenOnvif seen = fake.seen();
  CHECK(seen.challenged == 1);
  CHECK(seen.answered == 6);
  REQUIRE_FALSE(seen.contentTypes.empty());
  CHECK(seen.contentTypes.front() ==
        R"(application/soap+xml; charset=utf-8; action="http://www.onvif.org/ver10/device/wsdl/GetSystemDateAndTime")");
  CHECK(seen.paths ==
        std::vector<std::string>{"/onvif/device_service", "/onvif/device_service", "/onvif/device_service",
                                 "/onvif/device_service", "/onvif/media_service", "/onvif/ptz_service",
                                 "/onvif/ptz_service"});

  const auto control = CameraDriverRegistry::instance().controlDriverFor(onvifCamera("{}", 77));
  CHECK(std::dynamic_pointer_cast<OnvifDriver>(control) != nullptr);
  CHECK(CameraDriverRegistry::instance().driverFor(onvifCamera("{}", 77)) == nullptr);
  CameraDriverRegistry::instance().forget(77);
  const auto onOwnPort = CameraDriverRegistry::instance().controlDriverFor(onvifCamera(R"({"onvifPort":8000})", 78));
  CHECK(CameraDriverRegistry::instance().controlDriverFor(onvifCamera(R"({"onvifPort":8000})", 78)) == onOwnPort);
  CHECK(CameraDriverRegistry::instance().controlDriverFor(onvifCamera(R"({"onvifPort":8001})", 78)) != onOwnPort);
  CameraDriverRegistry::instance().forget(78);

  trantor::EventLoopThread eventsLoop("onvif-events-test");
  eventsLoop.run();
  const OnvifEndpoint local{.host = "127.0.0.1",
                            .port = listeners.front().toPort(),
                            .username = kUser,
                            .password = kPassword,
                            .timeoutSeconds = 5.0};
  OnvifClient events({.endpoint = local,
                      .transport = {},
                      .asyncTransport = std::make_shared<OnvifAsyncHttpTransport>(local, eventsLoop.getLoop()),
                      .clock = {}});
  REQUIRE(drogon::sync_wait(events.openEvents()).ok());
  const auto made = drogon::sync_wait(events.subscribe());
  REQUIRE(made.ok());
  CHECK(made.value.path == kManagerPath);
  const auto pulled = drogon::sync_wait(events.pull(made.value));
  REQUIRE(pulled.ok());
  CHECK(pulled.value.motions.size() == 4);
  const SeenOnvif after = fake.seen();
  CHECK(after.challenged == 2);
  REQUIRE(after.targets.size() >= 3);
  CHECK(after.targets.back() == kManagerPath);
  CHECK(after.targets[after.targets.size() - 2] == "/onvif/event_service");
  CHECK(after.contentTypes.back() ==
        R"(application/soap+xml; charset=utf-8; action="http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/PullMessagesRequest")");

  drogon::app().quit();
  runner.join();
}
