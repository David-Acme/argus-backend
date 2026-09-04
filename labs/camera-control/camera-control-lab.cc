#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <drogon/HttpController.h>
#include <drogon/WebSocketController.h>
#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>
#include <fcntl.h>
#include <fstream>
#include <json/reader.h>
#include <json/value.h>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <optional>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/face/face-service.hxx>
#include <shared/services/sqlite/vec-db.hxx>
#include <shared/services/stream/upstream-http.hxx>
#include <shared/services/tapo/tapo-api.hxx>
#include <shared/services/tapo/tapo-talk-client.hxx>
#include <shared/services/tts/onnx-utils.hxx>
#include <shared/services/tts/tts-service.hxx>
#include <shared/wrapper/cancellation/cancellation-token.hxx>
#include <shared/wrapper/sqlite-stmt/sqlite-stmt.hxx>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <toml++/toml.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{

constexpr const char* kConfigPath = "labs/camera-control/config.toml";

struct LabResult
{
  bool ok{false};
  int status{500};
  std::string error;
  Json::Value data;
};

LabResult failure(int status, std::string error)
{
  return {.ok = false,
          .status = status,
          .error = std::move(error),
          .data = Json::Value()};
}

LabResult success(Json::Value data)
{
  return {.ok = true, .status = 200, .error = {}, .data = std::move(data)};
}

int tapoInnerErrorCode(const Json::Value& data)
{
  const auto& responses = data["result"]["responses"];
  if (responses.isArray() && !responses.empty())
    return responses[responses.size() - 1]["error_code"].asInt();
  return data["error_code"].asInt();
}

std::string alarmVolumeLevel(int value)
{
  if (value <= 33)
    return "low";
  if (value <= 66)
    return "normal";
  return "high";
}

std::string escapeToml(const std::string& value)
{
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  for (const char c : value) {
    if (c == '\\' || c == '"')
      out.push_back('\\');
    if (c == '\n') {
      out += "\\n";
      continue;
    }
    if (c == '\r') {
      out += "\\r";
      continue;
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

std::string compactJson(const Json::Value& value)
{
  Json::StreamWriterBuilder builder;
  builder["indent"] = "";
  return Json::writeString(builder, value);
}

Json::Value normalizePresets(const Json::Value& response)
{
  Json::Value out(Json::objectValue);
  Json::Value presets(Json::arrayValue);

  const Json::Value* payload = &response;
  const auto& responses = response["result"]["responses"];
  if (responses.isArray() && responses.size() > 0)
    payload = &responses[0]["result"];

  const auto& preset = (*payload)["preset"]["preset"];
  const auto& ids = preset["id"];
  const auto& names = preset["name"];
  if (ids.isArray()) {
    for (Json::ArrayIndex index = 0; index < ids.size(); ++index) {
      Json::Value item(Json::objectValue);
      item["id"] = ids[index];
      item["name"] = names.isArray() && index < names.size() ? names[index] : ids[index];
      presets.append(std::move(item));
    }
  }

  out["presets"] = std::move(presets);
  out["raw"] = response;
  return out;
}

const toml::table* section(const toml::table& root, const char* name)
{
  const auto* node = root.get(name);
  return node ? node->as_table() : nullptr;
}

template <typename T>
T readValue(const toml::table* table, const char* key, T fallback)
{
  if (!table)
    return fallback;
  const auto* node = table->get(key);
  return node ? node->value_or(fallback) : fallback;
}

struct LabConfig
{
  std::string host{"127.0.0.1"};
  int port{7090};
  std::string webRoot{"labs/camera-control/web"};

  std::string go2rtcBin{"third_party/go2rtc/go2rtc"};
  std::string go2rtcHost{"127.0.0.1"};
  int go2rtcPort{1985};
  int go2rtcRtspPort{8555};

  std::string cameraHost;
  std::string username;
  std::string password;
  std::string cloudUsername{"admin"};
  std::string cloudPassword;
  int controlPort{443};
  int mediaPort{8800};
  int rtspPort{554};
  std::string transport{"auto"};
  int connectTimeoutMs{3000};
  int requestTimeoutMs{5000};
  int loginAttempts{2};
  std::string talkMode{"aec"};
  std::string talkFraming{"none"};
  int talkPacketMs{20};

  std::string provider{"native"};
  std::string fallback{"local"};
  std::string recordMode{"events"};
  int dwellSeconds{10};
  Json::Value zones{Json::arrayValue};
  int operatorLeaseSeconds{120};

  struct Faces
  {
    bool enabled{false};
    float minScore{0.6F};
    int captureCooldownMs{2000};
    int targetFps{5};
    bool identify{true};
    std::string source{"labcam"};
  };

  Faces faces;

  std::string go2rtcApi() const
  {
    return "http://" + go2rtcHost + ":" + std::to_string(go2rtcPort);
  }

  static LabConfig load(const std::string& path)
  {
    const auto parsed = toml::parse_file(path);
    const auto* lab = section(parsed, "lab");
    const auto* camera = section(parsed, "camera");
    const auto* detection = section(parsed, "detection");
    const auto* operatorSection = section(parsed, "operator");
    const auto* faces = section(parsed, "faces");

    LabConfig config;
    config.host = readValue(lab, "host", config.host);
    config.port = readValue<int64_t>(lab, "port", config.port);
    config.webRoot = readValue(lab, "web_root", config.webRoot);
    config.go2rtcBin = readValue(lab, "go2rtc_bin", config.go2rtcBin);
    config.go2rtcHost = readValue(lab, "go2rtc_host", config.go2rtcHost);
    config.go2rtcPort = readValue<int64_t>(lab, "go2rtc_port", config.go2rtcPort);
    config.go2rtcRtspPort = readValue<int64_t>(lab, "go2rtc_rtsp_port", config.go2rtcRtspPort);
    config.cameraHost = readValue(camera, "host", config.cameraHost);
    config.username = readValue(camera, "username", config.username);
    config.password = readValue(camera, "password", config.password);
    config.cloudUsername = readValue(camera, "cloud_username", config.cloudUsername);
    config.cloudPassword = readValue(camera, "cloud_password", config.cloudPassword);
    config.controlPort = readValue<int64_t>(camera, "control_port", config.controlPort);
    config.mediaPort = readValue<int64_t>(camera, "media_port", config.mediaPort);
    config.rtspPort = readValue<int64_t>(camera, "rtsp_port", config.rtspPort);
    config.transport = readValue(camera, "transport", config.transport);
    config.connectTimeoutMs =
        readValue<int64_t>(camera, "connect_timeout_ms", config.connectTimeoutMs);
    config.requestTimeoutMs =
        readValue<int64_t>(camera, "request_timeout_ms", config.requestTimeoutMs);
    config.loginAttempts = readValue<int64_t>(camera, "login_attempts", config.loginAttempts);
    config.talkMode = readValue(camera, "talk_mode", config.talkMode);
    config.talkFraming = readValue(camera, "talk_framing", config.talkFraming);
    config.talkPacketMs = readValue<int64_t>(camera, "talk_packet_ms", config.talkPacketMs);
    config.provider = readValue(detection, "provider", config.provider);
    config.fallback = readValue(detection, "fallback", config.fallback);
    config.recordMode = readValue(detection, "record_mode", config.recordMode);
    config.dwellSeconds = readValue<int64_t>(detection, "dwell_seconds", config.dwellSeconds);
    config.operatorLeaseSeconds =
        readValue<int64_t>(operatorSection, "lease_seconds", config.operatorLeaseSeconds);
    config.faces.enabled = readValue<bool>(faces, "enabled", config.faces.enabled);
    config.faces.minScore =
        static_cast<float>(readValue<double>(faces, "min_score", config.faces.minScore));
    config.faces.captureCooldownMs =
        readValue<int64_t>(faces, "capture_cooldown_ms", config.faces.captureCooldownMs);
    config.faces.targetFps = readValue<int64_t>(faces, "target_fps", config.faces.targetFps);
    config.faces.identify = readValue<bool>(faces, "identify", config.faces.identify);
    config.faces.source = readValue(faces, "source", config.faces.source);

    const std::string zonesJson = readValue(detection, "zones_json", "[]");
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream input(zonesJson);
    Json::Value zones;
    if (Json::parseFromStream(reader, input, &zones, &errors) && zones.isArray())
      config.zones = std::move(zones);
    return config;
  }

  Json::Value toJson() const
  {
    Json::Value out;
    out["lab"]["host"] = host;
    out["lab"]["port"] = port;
    out["lab"]["webRoot"] = webRoot;
    out["lab"]["go2rtcApi"] = go2rtcApi();
    out["lab"]["go2rtcBin"] = go2rtcBin;
    out["lab"]["go2rtcPort"] = go2rtcPort;
    out["lab"]["go2rtcRtspPort"] = go2rtcRtspPort;
    out["camera"]["host"] = cameraHost;
    out["camera"]["username"] = username;
    out["camera"]["password"] = password;
    out["camera"]["cloudUsername"] = cloudUsername;
    out["camera"]["cloudPassword"] = cloudPassword;
    out["camera"]["controlPort"] = controlPort;
    out["camera"]["mediaPort"] = mediaPort;
    out["camera"]["rtspPort"] = rtspPort;
    out["camera"]["transport"] = transport;
    out["camera"]["connectTimeoutMs"] = connectTimeoutMs;
    out["camera"]["requestTimeoutMs"] = requestTimeoutMs;
    out["camera"]["loginAttempts"] = loginAttempts;
    out["camera"]["talkMode"] = talkMode;
    out["camera"]["talkFraming"] = talkFraming;
    out["camera"]["talkPacketMs"] = talkPacketMs;
    out["detection"]["provider"] = provider;
    out["detection"]["fallback"] = fallback;
    out["detection"]["recordMode"] = recordMode;
    out["detection"]["dwellSeconds"] = dwellSeconds;
    out["detection"]["zones"] = zones;
    out["operator"]["leaseSeconds"] = operatorLeaseSeconds;
    out["faces"]["enabled"] = faces.enabled;
    out["faces"]["minScore"] = faces.minScore;
    out["faces"]["captureCooldownMs"] = faces.captureCooldownMs;
    out["faces"]["targetFps"] = faces.targetFps;
    out["faces"]["identify"] = faces.identify;
    out["faces"]["source"] = faces.source;
    return out;
  }

  bool update(const Json::Value& input)
  {
    if (input.isMember("lab") && input["lab"].isObject()) {
      const auto& value = input["lab"];
      if (value["host"].isString()) host = value["host"].asString();
      if (value["port"].isInt()) port = value["port"].asInt();
      if (value["webRoot"].isString()) webRoot = value["webRoot"].asString();
      if (value["go2rtcBin"].isString()) go2rtcBin = value["go2rtcBin"].asString();
      if (value["go2rtcHost"].isString()) go2rtcHost = value["go2rtcHost"].asString();
      if (value["go2rtcPort"].isInt()) go2rtcPort = value["go2rtcPort"].asInt();
      if (value["go2rtcRtspPort"].isInt()) go2rtcRtspPort = value["go2rtcRtspPort"].asInt();
    }
    if (input.isMember("camera") && input["camera"].isObject()) {
      const auto& value = input["camera"];
      if (value["host"].isString()) cameraHost = value["host"].asString();
      if (value["username"].isString()) username = value["username"].asString();
      if (value["password"].isString()) password = value["password"].asString();
      if (value["cloudUsername"].isString()) cloudUsername = value["cloudUsername"].asString();
      if (value["cloudPassword"].isString()) cloudPassword = value["cloudPassword"].asString();
      if (value["controlPort"].isInt()) controlPort = value["controlPort"].asInt();
      if (value["mediaPort"].isInt()) mediaPort = value["mediaPort"].asInt();
      if (value["rtspPort"].isInt()) rtspPort = value["rtspPort"].asInt();
      if (value["transport"].isString()) transport = value["transport"].asString();
      if (value["connectTimeoutMs"].isInt()) connectTimeoutMs = value["connectTimeoutMs"].asInt();
      if (value["requestTimeoutMs"].isInt()) requestTimeoutMs = value["requestTimeoutMs"].asInt();
      if (value["loginAttempts"].isInt()) loginAttempts = value["loginAttempts"].asInt();
      if (value["talkMode"].isString()) talkMode = value["talkMode"].asString();
      if (value["talkFraming"].isString()) talkFraming = value["talkFraming"].asString();
      if (value["talkPacketMs"].isInt()) talkPacketMs = value["talkPacketMs"].asInt();
    }
    if (input.isMember("detection") && input["detection"].isObject()) {
      const auto& value = input["detection"];
      if (value["provider"].isString()) provider = value["provider"].asString();
      if (value["fallback"].isString()) fallback = value["fallback"].asString();
      if (value["recordMode"].isString()) recordMode = value["recordMode"].asString();
      if (value["dwellSeconds"].isInt()) dwellSeconds = value["dwellSeconds"].asInt();
      if (value["zones"].isArray()) zones = value["zones"];
    }
    if (input.isMember("operator") && input["operator"].isObject() &&
        input["operator"]["leaseSeconds"].isInt())
      operatorLeaseSeconds = input["operator"]["leaseSeconds"].asInt();
    return valid();
  }

  bool valid() const
  {
    return !host.empty() && port > 0 && port <= 65535 && !webRoot.empty() &&
           !go2rtcHost.empty() && go2rtcPort > 0 && go2rtcPort <= 65535 &&
           go2rtcRtspPort > 0 && go2rtcRtspPort <= 65535 && rtspPort > 0 &&
           rtspPort <= 65535 && controlPort > 0 && controlPort <= 65535 &&
           mediaPort > 0 && mediaPort <= 65535 && dwellSeconds >= 0 &&
           operatorLeaseSeconds > 0 && zones.isArray() &&
           faces.targetFps > 0 && faces.targetFps <= 30 &&
           faces.minScore > 0.0F && faces.minScore < 1.0F &&
           faces.captureCooldownMs >= 0 && !faces.source.empty();
  }

  bool save(const std::string& path) const
  {
    const std::string temporary = path + ".tmp";
    std::ofstream out(temporary, std::ios::trunc);
    if (!out.is_open())
      return false;
    out << "# Local values for the standalone browser lab.\n\n"
        << "[lab]\n"
        << "host = " << escapeToml(host) << "\n"
        << "port = " << port << "\n"
        << "web_root = " << escapeToml(webRoot) << "\n"
        << "go2rtc_bin = " << escapeToml(go2rtcBin) << "\n"
        << "go2rtc_host = " << escapeToml(go2rtcHost) << "\n"
        << "go2rtc_port = " << go2rtcPort << "\n"
        << "go2rtc_rtsp_port = " << go2rtcRtspPort << "\n\n"
        << "[camera]\n"
        << "host = " << escapeToml(cameraHost) << "\n"
        << "username = " << escapeToml(username) << "\n"
        << "password = " << escapeToml(password) << "\n"
        << "cloud_username = " << escapeToml(cloudUsername) << "\n"
        << "cloud_password = " << escapeToml(cloudPassword) << "\n"
        << "control_port = " << controlPort << "\n"
        << "media_port = " << mediaPort << "\n"
        << "rtsp_port = " << rtspPort << "\n"
        << "transport = " << escapeToml(transport) << "\n"
        << "connect_timeout_ms = " << connectTimeoutMs << "\n"
        << "request_timeout_ms = " << requestTimeoutMs << "\n"
        << "login_attempts = " << loginAttempts << "\n"
        << "talk_mode = " << escapeToml(talkMode) << "\n"
        << "talk_framing = " << escapeToml(talkFraming) << "\n"
        << "talk_packet_ms = " << talkPacketMs << "\n\n"
        << "[detection]\n"
        << "provider = " << escapeToml(provider) << "\n"
        << "fallback = " << escapeToml(fallback) << "\n"
        << "record_mode = " << escapeToml(recordMode) << "\n"
        << "dwell_seconds = " << dwellSeconds << "\n"
        << "zones_json = " << escapeToml(compactJson(zones)) << "\n\n"
        << "[operator]\n"
        << "lease_seconds = " << operatorLeaseSeconds << "\n\n"
        << "[faces]\n"
        << "enabled = " << (faces.enabled ? "true" : "false") << "\n"
        << "min_score = " << faces.minScore << "\n"
        << "capture_cooldown_ms = " << faces.captureCooldownMs << "\n"
        << "target_fps = " << faces.targetFps << "\n"
        << "identify = " << (faces.identify ? "true" : "false") << "\n"
        << "source = " << escapeToml(faces.source) << "\n";
    out.close();
    if (!out.good())
      return false;
    std::remove(path.c_str());
    return std::rename(temporary.c_str(), path.c_str()) == 0;
  }
};

class TalkArbiter
{
public:
  struct Ticket
  {
    CancellationToken token;
    uint64_t generation{0};
    std::string source;
  };

  bool takeover()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    operatorActive_ = true;
    if (currentToken_)
      currentToken_->cancel();
    ++generation_;
    return true;
  }

  void release()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    operatorActive_ = false;
    ++generation_;
  }

  std::optional<Ticket> begin(const std::string& source)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (source == "llm" && operatorActive_)
      return std::nullopt;
    if (currentToken_)
      currentToken_->cancel();
    Ticket ticket;
    ticket.generation = ++generation_;
    ticket.source = source;
    currentToken_ = ticket.token;
    return ticket;
  }

  void finish(const Ticket& ticket)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ticket.generation == generation_)
      currentToken_.reset();
  }

  Json::Value status() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Json::Value out;
    out["operatorActive"] = operatorActive_;
    out["generation"] = static_cast<Json::UInt64>(generation_);
    out["talkActive"] = currentToken_.has_value();
    return out;
  }

private:
  mutable std::mutex mutex_;
  std::optional<CancellationToken> currentToken_;
  bool operatorActive_{false};
  uint64_t generation_{0};
};

TapoClientConfig makeClientConfig(const LabConfig& config)
{
  TapoClientConfig client;
  client.host = config.cameraHost;
  client.port = config.controlPort;
  client.transport = tapoTransportPreferenceFromString(config.transport);
  client.connectTimeoutMs = config.connectTimeoutMs;
  client.requestTimeoutMs = config.requestTimeoutMs;
  client.loginAttempts = config.loginAttempts;
  if (!config.username.empty() && !config.password.empty())
    client.candidates.push_back({.label = "camera_account",
                                 .username = config.username,
                                 .password = config.password});
  if (!config.cloudPassword.empty())
    client.candidates.push_back({.label = "cloud_account",
                                 .username = config.cloudUsername,
                                 .password = config.cloudPassword});
  return client;
}

std::string urlEncode(const std::string& value)
{
  static const char* digits = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size() * 2);
  for (const unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
      continue;
    }
    out.push_back('%');
    out.push_back(digits[c >> 4]);
    out.push_back(digits[c & 0x0F]);
  }
  return out;
}

bool portOpen(const std::string& host, int port)
{
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0 || !result)
    return false;
  bool open = false;
  for (addrinfo* info = result; info; info = info->ai_next) {
    const int fd = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol);
    if (fd < 0)
      continue;
    const timeval timeout{.tv_sec = 0, .tv_usec = 400000};
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (::connect(fd, info->ai_addr, info->ai_addrlen) == 0)
      open = true;
    ::close(fd);
    if (open)
      break;
  }
  ::freeaddrinfo(result);
  return open;
}

void writeGo2rtcConfig(const LabConfig& config)
{
  const std::string account = config.username.empty() ? config.cloudUsername : config.username;
  const std::string password = config.username.empty() ? config.cloudPassword : config.password;
  const std::string credentials = urlEncode(account) + ":" + urlEncode(password);

  std::ofstream out("labs/camera-control/go2rtc.yaml", std::ios::trunc);
  if (!out.is_open())
    return;
  out << "api:\n"
      << "  listen: \"" << config.go2rtcHost << ":" << config.go2rtcPort << "\"\n"
      << "  origin: \"*\"\n"
      << "rtsp:\n"
      << "  listen: \"127.0.0.1:" << config.go2rtcRtspPort << "\"\n"
      << "log:\n"
      << "  level: info\n"
      << "streams:\n"
      << "  labcam:\n"
      << "    - rtsp://" << credentials << "@" << config.cameraHost << ":"
      << config.rtspPort << "/stream2\n"
      << "  labcam_main:\n"
      << "    - rtsp://" << credentials << "@" << config.cameraHost << ":"
      << config.rtspPort << "/stream1\n";
}

std::string resolveGo2rtcBin(const LabConfig& config)
{
  const std::string candidates[] = {config.go2rtcBin,
                                    "../../../third_party/go2rtc/go2rtc",
                                    "../../../../third_party/go2rtc/go2rtc"};
  for (const std::string& candidate : candidates) {
    if (::access(candidate.c_str(), X_OK) == 0)
      return candidate;
  }
  return {};
}

bool ensurePreview(const LabConfig& config)
{
  if (portOpen(config.go2rtcHost, config.go2rtcPort))
    return true;
  writeGo2rtcConfig(config);
  const std::string bin = resolveGo2rtcBin(config);
  if (bin.empty())
    return false;
  const pid_t pid = ::fork();
  if (pid < 0)
    return false;
  if (pid == 0) {
    ::setsid();
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
    }
    const std::string yaml = "labs/camera-control/go2rtc.yaml";
    const char* argv[] = {bin.c_str(), "-config", yaml.c_str(), nullptr};
    ::execv(bin.c_str(), const_cast<char* const*>(argv));
    ::_exit(127);
  }
  for (int attempt = 0; attempt < 20; ++attempt) {
    ::usleep(150000);
    if (portOpen(config.go2rtcHost, config.go2rtcPort))
      return true;
  }
  return false;
}

int64_t epochMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

float faceIou(const FaceService::FaceBox& a, const FaceService::FaceBox& b)
{
  const float ix1 = std::max(a.x1, b.x1);
  const float iy1 = std::max(a.y1, b.y1);
  const float ix2 = std::min(a.x2, b.x2);
  const float iy2 = std::min(a.y2, b.y2);
  const float iw = std::max(0.0F, ix2 - ix1);
  const float ih = std::max(0.0F, iy2 - iy1);
  const float inter = iw * ih;
  const float areaA = std::max(0.0F, a.x2 - a.x1) * std::max(0.0F, a.y2 - a.y1);
  const float areaB = std::max(0.0F, b.x2 - b.x1) * std::max(0.0F, b.y2 - b.y1);
  return inter / (areaA + areaB - inter + 1e-6F);
}

class DetectionEngine
{
public:
  static DetectionEngine& instance()
  {
    static DetectionEngine engine;
    return engine;
  }

  void start(const LabConfig& config)
  {
    if (running_.exchange(true))
      return;
    stop_.store(false);
    worker_ = std::thread([this, config] { loop(config); });
  }

  void stop()
  {
    if (!running_.exchange(false))
      return;
    stop_.store(true);
    if (worker_.joinable())
      worker_.join();
  }

  bool running() const { return running_.load(); }
  bool loaded() const { return FaceService::instance().isLoaded(); }

  void add(const drogon::WebSocketConnectionPtr& connection)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    connections_.push_back(connection);
  }

  void remove(const drogon::WebSocketConnectionPtr& connection)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::erase(connections_, connection);
  }

  Json::Value snapshot() const
  {
    Json::Value out;
    out["running"] = running();
    out["loaded"] = loaded();
    std::lock_guard<std::mutex> lock(mutex_);
    out["ts"] = lastTick_["ts"];
    out["fps"] = lastTick_["fps"];
    out["frame"] = lastTick_["frame"];
    out["faces"] = lastTick_["faces"];
    out["capture"] = lastCapture_;
    out["history"] = history_;
    return out;
  }

  Json::Value enroll(const std::string& name)
  {
    std::vector<float> embedding;
    int64_t pendingTs = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      embedding = pendingEmbedding_;
      pendingTs = pendingTs_;
    }
    if (name.empty())
      return enrollmentError("Escribe un nombre para enrolar.");
    if (embedding.size() != 128 || epochMs() - pendingTs > 10000)
      return enrollmentError("No hay una captura reciente de rostro.");

    sqlite3* db = VecDb::instance().handle();
    if (!db)
      return enrollmentError("La base de datos no está disponible.");

    int64_t personId = 0;
    int64_t faceId = 0;
    {
      std::lock_guard<std::mutex> lock(VecDb::instance().mutex());
      if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK)
        return enrollmentError("No se pudo iniciar la transacción.");
      {
        SqliteStmt stmt;
        if (!stmt.prepare(db,
                "INSERT INTO person (user_id, name, alias, observation, "
                "first_seen_at, last_seen_at) VALUES (NULL, ?, '', '', "
                "strftime('%s','now'), strftime('%s','now'))") ||
            !stmt.bindText(1, name) || stmt.step() != SQLITE_DONE) {
          sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
          return enrollmentError("No se pudo crear la persona.");
        }
        personId = sqlite3_last_insert_rowid(db);
      }
      {
        const std::string blob(reinterpret_cast<const char*>(embedding.data()),
                               embedding.size() * sizeof(float));
        SqliteStmt stmt;
        if (!stmt.prepare(db,
                "INSERT INTO face_embedding (person_id, embedding, "
                "angle_label, quality) VALUES (?, ?, 'frontal', 1.0)") ||
            !stmt.bindInt64(1, personId) ||
            !stmt.bindBlob(2, blob.data(), blob.size()) ||
            stmt.step() != SQLITE_DONE) {
          sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
          return enrollmentError("No se pudo guardar el rostro.");
        }
        faceId = sqlite3_last_insert_rowid(db);
      }
      sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    }

    FaceService::instance().faceDb().insert(embedding.data(), personId, faceId);

    Json::Value out;
    out["personId"] = static_cast<Json::Int64>(personId);
    out["faceEmbeddingId"] = static_cast<Json::Int64>(faceId);
    out["name"] = name;
    out["known"] = true;
    return out;
  }

  std::string snapshotJpeg() const
  {
    cv::Mat frame;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      frame = lastFrame_;
    }
    if (frame.empty())
      return {};
    std::vector<uchar> jpeg;
    const std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, 80};
    if (!cv::imencode(".jpg", frame, jpeg, params))
      return {};
    return {jpeg.begin(), jpeg.end()};
  }

  Json::Value knownFaces() const
  {
    Json::Value list(Json::arrayValue);
    sqlite3* db = VecDb::instance().handle();
    if (!db)
      return list;
    std::lock_guard<std::mutex> lock(VecDb::instance().mutex());
    SqliteStmt stmt;
    if (!stmt.prepare(db,
            "SELECT p.id, p.name, COUNT(f.id) AS embeddings FROM person p "
            "LEFT JOIN face_embedding f ON f.person_id = p.id "
            "WHERE p.deleted_at IS NULL GROUP BY p.id "
            "ORDER BY p.id DESC LIMIT 20"))
      return list;
    while (stmt.step() == SQLITE_ROW) {
      Json::Value row;
      row["id"] = static_cast<Json::Int64>(stmt.columnInt64(0));
      row["name"] = stmt.columnText(1);
      row["embeddings"] = stmt.columnInt(2);
      list.append(std::move(row));
    }
    return list;
  }

private:
  DetectionEngine() = default;

  static Json::Value enrollmentError(const std::string& message)
  {
    Json::Value out;
    out["error"] = message;
    return out;
  }

  bool openFrameSource(const LabConfig& config)
  {
    const auto [host, port] =
        upstream_http::splitHostPort(config.go2rtcApi().substr(7));
    const std::string command =
        "ffmpeg -hide_banner -loglevel error -rtsp_transport tcp -i "
        "rtsp://" + host + ":8555/" + config.faces.source +
        " -vf fps=" + std::to_string(config.faces.targetFps) +
        ",scale=1280:720 -f rawvideo -pix_fmt bgr24 -";
    framePipe_ = ::popen(command.c_str(), "r");
    return framePipe_ != nullptr;
  }

  void closeFrameSource()
  {
    if (framePipe_) {
      ::pclose(framePipe_);
      framePipe_ = nullptr;
    }
  }

  cv::Mat readFrame()
  {
    if (!framePipe_)
      return {};
    frameBuffer_.resize(kFrameBytes);
    size_t filled = 0;
    while (filled < kFrameBytes) {
      const size_t got = ::fread(frameBuffer_.data() + filled, 1,
                                 kFrameBytes - filled, framePipe_);
      if (got == 0)
        return {};
      filled += got;
    }
    return cv::Mat(kFrameHeight, kFrameWidth, CV_8UC3, frameBuffer_.data());
  }

  std::optional<std::string> personName(int64_t personId) const
  {
    sqlite3* db = VecDb::instance().handle();
    if (!db)
      return std::nullopt;
    std::lock_guard<std::mutex> lock(VecDb::instance().mutex());
    SqliteStmt stmt;
    if (!stmt.prepare(db,
            "SELECT name FROM person WHERE id = ? AND deleted_at IS NULL"))
      return std::nullopt;
    stmt.bindInt64(1, personId);
    if (stmt.step() != SQLITE_ROW)
      return std::nullopt;
    return stmt.columnText(0);
  }

  Json::Value buildCapture(const cv::Mat& frame, const FaceService::FaceBox& box,
                           float score, int64_t personId,
                           const std::string& person, bool known, float match)
  {
    constexpr float margin = 0.30F;
    const float boxWidth = box.x2 - box.x1;
    const float boxHeight = box.y2 - box.y1;
    const int x = std::clamp(static_cast<int>(box.x1 - boxWidth * margin), 0,
                             frame.cols - 1);
    const int y = std::clamp(static_cast<int>(box.y1 - boxHeight * margin), 0,
                             frame.rows - 1);
    const int width = std::clamp(
        static_cast<int>(boxWidth * (1.0F + 2.0F * margin)), 1, frame.cols - x);
    const int height = std::clamp(
        static_cast<int>(boxHeight * (1.0F + 2.0F * margin)), 1, frame.rows - y);
    cv::Mat crop = frame(cv::Rect(x, y, width, height)).clone();
    constexpr int targetHeight = 220;
    if (crop.rows > targetHeight) {
      const double scale = static_cast<double>(targetHeight) / crop.rows;
      cv::resize(crop, crop, cv::Size(), scale, scale, cv::INTER_AREA);
    }
    std::vector<uchar> jpeg;
    const std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, 85};
    if (!cv::imencode(".jpg", crop, jpeg, params))
      return {};

    Json::Value capture;
    capture["type"] = "capture";
    capture["ts"] = static_cast<Json::Int64>(epochMs());
    capture["image"] = "data:image/jpeg;base64," +
                       drogon::utils::base64Encode(
                           reinterpret_cast<const unsigned char*>(jpeg.data()),
                           static_cast<size_t>(jpeg.size()));
    capture["score"] = score;
    capture["personId"] = static_cast<Json::Int64>(personId);
    capture["person"] = person;
    capture["known"] = known;
    capture["match"] = match;
    capture["box"]["x"] = std::clamp(box.x1 / frame.cols, 0.0F, 1.0F);
    capture["box"]["y"] = std::clamp(box.y1 / frame.rows, 0.0F, 1.0F);
    capture["box"]["w"] =
        std::clamp(boxWidth / frame.cols, 0.0F, 1.0F);
    capture["box"]["h"] =
        std::clamp(boxHeight / frame.rows, 0.0F, 1.0F);
    capture["size"] = static_cast<Json::Int64>(jpeg.size());
    return capture;
  }

  void publish(const Json::Value& tick, const Json::Value& capture)
  {
    std::string tickText;
    std::string captureText;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      lastTick_ = tick;
      if (!capture.isNull())
        lastCapture_ = capture;
      tickText = compactJson(tick);
      if (!capture.isNull())
        captureText = compactJson(capture);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& connection : connections_) {
      if (!connection || !connection->connected())
        continue;
      connection->send(tickText);
      if (!captureText.empty())
        connection->send(captureText);
    }
  }

  void loop(const LabConfig& config)
  {
    const auto period = std::chrono::milliseconds(1000 / config.faces.targetFps);
    auto lastCaptureTime = std::chrono::steady_clock::time_point::min();
    FaceService::FaceBox lastCapturedBox{};
    bool hasCapturedBox = false;
    auto lastTickTime = std::chrono::steady_clock::now() -
                        std::chrono::milliseconds(1000);

    if (!openFrameSource(config)) {
      publish(Json::Value(), Json::Value());
      return;
    }

    while (!stop_.load()) {
      const auto tickStart = std::chrono::steady_clock::now();
      cv::Mat frame = readFrame();
      if (frame.empty()) {
        closeFrameSource();
        if (stop_.load())
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!openFrameSource(config))
          break;
        continue;
      }
      cv::Mat rgb;
      cv::cvtColor(frame, rgb, cv::COLOR_BGR2RGB);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        lastFrame_ = frame.clone();
      }

      Json::Value capture;
      Json::Value faces(Json::arrayValue);
      int bestIndex = -1;
      float bestScore = 0.0F;
      int64_t bestPersonId = 0;
      std::string bestPerson;
      bool bestKnown = false;
      float bestMatch = 0.0F;
      std::vector<float> bestEmbedding;

      {
        std::lock_guard<std::mutex> lock(inferenceMutex_);
        auto& service = FaceService::instance();
        const std::vector<FaceService::FaceBox> boxes =
            service.detectAll(rgb.data, rgb.cols, rgb.rows);
        for (size_t index = 0; index < boxes.size(); ++index) {
          const auto& box = boxes[index];
          Json::Value face;
          face["x"] = std::clamp(box.x1 / rgb.cols, 0.0F, 1.0F);
          face["y"] = std::clamp(box.y1 / rgb.rows, 0.0F, 1.0F);
          face["w"] = std::clamp((box.x2 - box.x1) / rgb.cols, 0.0F, 1.0F);
          face["h"] = std::clamp((box.y2 - box.y1) / rgb.rows, 0.0F, 1.0F);
          face["score"] = box.score;
          face["personId"] = Json::Int64(0);
          face["person"] = "";
          face["known"] = false;
          face["match"] = 0.0F;
          faces.append(std::move(face));
          if (box.score > bestScore) {
            bestScore = box.score;
            bestIndex = static_cast<int>(index);
          }
        }

        if (bestIndex >= 0 && config.faces.identify) {
          const auto result =
              service.extractFace(rgb.data, rgb.cols, rgb.rows, boxes[bestIndex]);
          if (result) {
            bestEmbedding = result->embedding;
            const auto match = service.faceDb().search(result->embedding.data());
            if (match && match->second >= 0.80F) {
              bestPersonId = match->first;
              bestMatch = match->second;
              bestKnown = true;
              bestPerson = personName(bestPersonId).value_or("");
            }
          }
        }

        if (bestIndex >= 0 && bestScore >= config.faces.minScore) {
          const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     tickStart - lastCaptureTime)
                                     .count();
          const bool moved = !hasCapturedBox ||
                             faceIou(lastCapturedBox, boxes[bestIndex]) < 0.55F;
          if (elapsedMs >= config.faces.captureCooldownMs && moved) {
            capture = buildCapture(frame, boxes[bestIndex], bestScore,
                                   bestPersonId, bestPerson, bestKnown,
                                   bestMatch);
            lastCapturedBox = boxes[bestIndex];
            hasCapturedBox = true;
            lastCaptureTime = tickStart;
          }
        }
      }

      if (bestIndex >= 0) {
        auto& best = faces[static_cast<Json::ArrayIndex>(bestIndex)];
        best["personId"] = static_cast<Json::Int64>(bestPersonId);
        best["person"] = bestPerson;
        best["known"] = bestKnown;
        best["match"] = bestMatch;
      }

      const double elapsedTickMs =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - lastTickTime)
              .count();
      lastTickTime = tickStart;
      Json::Value tick;
      tick["type"] = "tick";
      tick["ts"] = static_cast<Json::Int64>(epochMs());
      tick["fps"] = std::max(1.0, 1000.0 / std::max(1.0, elapsedTickMs));
      tick["frame"]["width"] = frame.cols;
      tick["frame"]["height"] = frame.rows;
      tick["faces"] = faces;

      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!capture.isNull()) {
          pendingEmbedding_ = std::move(bestEmbedding);
          pendingTs_ = tick["ts"].asInt64();
          history_.append(capture);
          if (history_.size() > 6) {
            Json::Value trimmed(Json::arrayValue);
            for (Json::ArrayIndex index = history_.size() - 6;
                 index < history_.size(); ++index)
              trimmed.append(history_[index]);
            history_ = std::move(trimmed);
          }
        }
      }

      publish(tick, capture);
      std::this_thread::sleep_until(tickStart + period);
    }
    closeFrameSource();
  }

  static constexpr int kFrameWidth = 1280;
  static constexpr int kFrameHeight = 720;
  static constexpr size_t kFrameBytes =
      static_cast<size_t>(kFrameWidth) * kFrameHeight * 3;

  std::FILE* framePipe_{nullptr};
  std::vector<uint8_t> frameBuffer_;
  mutable std::mutex mutex_;
  std::vector<drogon::WebSocketConnectionPtr> connections_;
  cv::Mat lastFrame_;
  Json::Value lastTick_;
  Json::Value lastCapture_;
  Json::Value history_{Json::arrayValue};
  std::vector<float> pendingEmbedding_;
  int64_t pendingTs_{0};

  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::thread worker_;
  std::mutex inferenceMutex_;
};

class CameraLabService
{
public:
  CameraLabService() : config_(LabConfig::load(kConfigPath)) {}

  LabResult config() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return success(config_.toJson());
  }

  LabResult updateConfig(const Json::Value& input)
  {
    LabConfig updated;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      updated = config_;
      if (!updated.update(input))
        return failure(422, "Invalid lab configuration");
      if (!updated.save(kConfigPath))
        return failure(500, "Unable to persist labs/camera-control/config.toml");
      config_ = updated;
    }

    {
      std::lock_guard<std::mutex> deviceLock(deviceMutex_);
      api_.reset();
    }
    return success(updated.toJson());
  }

  LabResult status()
  {
    std::lock_guard<std::mutex> deviceLock(deviceMutex_);
    const auto ready = ensureConnected();
    if (!ready.ok)
      return failure(502, ready.error);
    const auto batch = api_->getStatus();
    if (!batch.ok)
      return failure(502, batch.error.empty() ? "Camera status request failed"
                                               : batch.error);
    Json::Value data = batch.toJson();
    data["connection"] = api_->state();
    return success(std::move(data));
  }

  std::string snapshotJpeg() const
  {
    std::string bytes = DetectionEngine::instance().snapshotJpeg();
    if (!bytes.empty())
      return bytes;
    LabConfig config;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      config = config_;
    }
    const auto [host, port] =
        upstream_http::splitHostPort(config.go2rtcApi().substr(7));
    const std::string command =
        "ffmpeg -hide_banner -loglevel error -rtsp_transport tcp -i rtsp://" +
        host + ":8555/" + config.faces.source +
        " -frames:v 1 -f image2pipe -vcodec mjpeg -";
    std::FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe)
      return {};
    std::string out;
    char buffer[65536];
    size_t got = 0;
    while ((got = ::fread(buffer, 1, sizeof(buffer), pipe)) > 0)
      out.append(buffer, got);
    ::pclose(pipe);
    return out;
  }

  LabResult preview()
  {
    LabConfig config;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      config = config_;
    }
    const bool ready = ensurePreview(config);
    Json::Value data;
    data["api"] = config.go2rtcApi();
    data["sub"] = "labcam";
    data["main"] = "labcam_main";
    data["ready"] = ready;
    if (!ready)
      data["error"] = "go2rtc binary not found or did not open its API port";
    return success(std::move(data));
  }

  LabResult detections() const
  {
    Json::Value data = DetectionEngine::instance().snapshot();
    if (!data["running"].asBool()) {
      std::lock_guard<std::mutex> lock(mutex_);
      data["faces"]["enabled"] = config_.faces.enabled;
      data["faces"]["targetFps"] = config_.faces.targetFps;
      data["faces"]["minScore"] = config_.faces.minScore;
    }
    return success(std::move(data));
  }

  LabResult toggleDetections()
  {
    LabConfig config;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      config = config_;
    }
    auto& engine = DetectionEngine::instance();
    if (engine.running())
      engine.stop();
    else if (!FaceService::instance().isLoaded())
      return failure(409, "FaceService: models missing in models/face");
    else
      engine.start(config);
    config.faces.enabled = engine.running();
    std::string toggleError;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (config.save(kConfigPath))
        config_ = config;
      else
        toggleError = "no se pudo guardar config.toml";
    }
    if (!toggleError.empty())
      return failure(500, toggleError);
    Json::Value data;
    data["running"] = engine.running();
    return success(std::move(data));
  }

  LabResult enrollFace(const Json::Value& body)
  {
    const Json::Value result = DetectionEngine::instance().enroll(
        body.isMember("name") && body["name"].isString() ? body["name"].asString()
                                                          : "");
    if (result.isMember("error"))
      return failure(409, result["error"].asString());
    return success(result);
  }

  LabResult faces() const
  {
    return success(DetectionEngine::instance().knownFaces());
  }

  LabResult capabilities()
  {
    std::lock_guard<std::mutex> deviceLock(deviceMutex_);
    const auto ready = ensureConnected();
    if (!ready.ok)
      return failure(502, ready.error);

    const auto motor = api_->getMotorCapability();

    std::lock_guard<std::mutex> lock(mutex_);
    Json::Value out;
    out["ptz"] = true;
    out["presets"] = true;
    out["talk"] = !config_.cloudPassword.empty();
    out["alarm"] = true;
    out["motorSupported"] = motor.ok;
    if (motor.ok)
      out["motor"] = motor.data;
    else {
      out["motorError"] = motor.error;
      out["motorErrorCode"] = motor.errorCode;
    }
    out["nativeDetection"] = true;
    out["localFallback"] = true;
    out["connection"] = api_->state();
    return success(out);
  }

  LabResult move(const Json::Value& body)
  {
    std::lock_guard<std::mutex> deviceLock(deviceMutex_);
    const auto ready = ensureConnected();
    if (!ready.ok)
      return failure(502, ready.error);
    if (body.isMember("direction") && body["direction"].isInt64())
      return toLab(api_->step({.direction = body["direction"].asInt64()}));
    if (body.isMember("angle") && body["angle"].isInt64())
      return toLab(api_->step({.direction = body["angle"].asInt64()}));
    if (body.isMember("x") && body["x"].isInt64() && body["y"].isInt64())
      return toLab(api_->move({.x = body["x"].asInt64(), .y = body["y"].asInt64()}));
    return failure(422, "Send direction or x and y");
  }

  LabResult calibrate()
  {
    std::lock_guard<std::mutex> deviceLock(deviceMutex_);
    const auto ready = ensureConnected();
    if (!ready.ok)
      return failure(502, ready.error);
    return toLab(api_->calibrateMotor());
  }

  LabResult stop()
  {
    std::lock_guard<std::mutex> deviceLock(deviceMutex_);
    const auto ready = ensureConnected();
    if (!ready.ok)
      return failure(502, ready.error);
    return toLab(api_->stopMotor());
  }

  LabResult presets()
  {
    std::lock_guard<std::mutex> deviceLock(deviceMutex_);
    const auto ready = ensureConnected();
    if (!ready.ok)
      return failure(502, ready.error);
    const auto result = api_->getPresets();
    return result.ok ? success(normalizePresets(result.data)) : failure(502, result.error);
  }

  LabResult preset(const Json::Value& body)
  {
    const std::string action = body.get("action", "goto").asString();
    const std::string id = body.get("id", "").asString();
    const std::string name = body.get("name", "").asString();
    if ((action != "goto" && action != "save" && action != "delete") ||
        (action != "save" && id.empty()) || (action == "save" && name.empty()))
      return failure(422, "Invalid preset action");

    std::lock_guard<std::mutex> deviceLock(deviceMutex_);
    const auto ready = ensureConnected();
    if (!ready.ok)
      return failure(502, ready.error);
    const TapoPresetInput input{.id = id, .name = name};
    if (action == "save")
      return toLab(api_->savePreset(input));
    if (action == "delete")
      return toLab(api_->deletePreset(input));
    return toLab(api_->gotoPreset(input));
  }

  LabResult alarm(const Json::Value& body)
  {
    if (!body.isMember("enabled") || !body["enabled"].isBool())
      return failure(422, "enabled must be boolean");
    std::string volume;
    if (body["volume"].isInt()) {
      const int value = body["volume"].asInt();
      if (value < 1 || value > 100)
        return failure(422, "volume must be between 1 and 100");
      volume = alarmVolumeLevel(value);
    }

    std::lock_guard<std::mutex> deviceLock(deviceMutex_);
    const auto ready = ensureConnected();
    if (!ready.ok)
      return failure(502, ready.error);
    if (!volume.empty()) {
      const auto configured = api_->setAlarmVolume(volume);
      if (!configured.ok)
        return failure(502, configured.error);
      if (const int code = tapoInnerErrorCode(configured.data); code != 0)
        return failure(502, "la cámara rechazó el volumen " + volume +
                                " (error " + std::to_string(code) + ")");
    }
    const auto alarmResult = api_->setAlarm({.enabled = body["enabled"].asBool()});
    if (!alarmResult.ok)
      return toLab(alarmResult);
    if (const int code = tapoInnerErrorCode(alarmResult.data); code != 0)
      return failure(502, "la cámara rechazó la alarma (error " +
                              std::to_string(code) + ")");
    return toLab(alarmResult);
  }

  LabResult alarmConfig()
  {
    std::lock_guard<std::mutex> deviceLock(deviceMutex_);
    const auto ready = ensureConnected();
    if (!ready.ok)
      return failure(502, ready.error);
    Json::Value data;
    const auto alert = api_->call("getAlertConfig", Json::Value());
    data["getAlertConfig"]["ok"] = alert.ok;
    data["getAlertConfig"]["errorCode"] = alert.errorCode;
    data["getAlertConfig"]["error"] = alert.error;
    data["getAlertConfig"]["data"] = alert.data;
    Json::Value names(Json::arrayValue);
    names.append("chn1_msg_alarm_info");
    Json::Value params;
    params["msg_alarm"]["name"] = names;
    const auto last = api_->call("getLastAlarmInfo", params);
    data["getLastAlarmInfo"]["ok"] = last.ok;
    data["getLastAlarmInfo"]["errorCode"] = last.errorCode;
    data["getLastAlarmInfo"]["error"] = last.error;
    data["getLastAlarmInfo"]["data"] = last.data;
    Json::Value capabilityParams;
    capabilityParams["msg_alarm"]["name"] = names;
    Json::Value tables(Json::arrayValue);
    tables.append("capability");
    capabilityParams["msg_alarm"]["table"] = tables;
    const auto capability = api_->call("getAlertConfig", capabilityParams);
    data["capability"]["ok"] = capability.ok;
    data["capability"]["errorCode"] = capability.errorCode;
    data["capability"]["error"] = capability.error;
    data["capability"]["data"] = capability.data;
    return success(std::move(data));
  }

  LabResult tapoProbe(const Json::Value& body)
  {
    const std::string method = body["method"].asString();
    if (method.empty() || method == "do")
      return failure(422, "solo se permiten métodos get* en la sonda");
    if (compactJson(body).find("\"start\"") != std::string::npos)
      return failure(422, "la sonda no dispara la sirena (action start)");
    std::lock_guard<std::mutex> deviceLock(deviceMutex_);
    const auto ready = ensureConnected();
    if (!ready.ok)
      return failure(502, ready.error);
    const auto result = api_->call(method, body.get("params", Json::Value()));
    Json::Value data;
    data["ok"] = result.ok;
    data["errorCode"] = result.errorCode;
    data["error"] = result.error;
    data["data"] = result.data;
    data["innerErrorCode"] = tapoInnerErrorCode(result.data);
    return success(std::move(data));
  }

  LabResult operatorStatus() const
  {
    std::lock_guard<std::mutex> lock(arbiterMutex_);
    return success(arbiter_.status());
  }

  LabResult takeover()
  {
    std::lock_guard<std::mutex> lock(arbiterMutex_);
    arbiter_.takeover();
    return success(arbiter_.status());
  }

  LabResult releaseOperator()
  {
    std::lock_guard<std::mutex> lock(arbiterMutex_);
    arbiter_.release();
    return success(arbiter_.status());
  }

  LabResult talk(const Json::Value& body)
  {
    const std::string text = body.get("text", "").asString();
    const std::string lang = body.get("lang", "es").asString();
    const std::string source = body.get("source", "operator").asString();
    if (text.empty() || text.size() > 300 || (lang != "es" && lang != "en"))
      return failure(422, "Invalid text or language");
    if (source != "operator" && source != "llm")
      return failure(422, "source must be operator or llm");

    std::optional<TalkArbiter::Ticket> ticket;
    {
      std::lock_guard<std::mutex> lock(arbiterMutex_);
      ticket = arbiter_.begin(source);
    }
    if (!ticket)
      return failure(409, "Operator has priority over the LLM");

    try {
      TtsService& tts = TtsService::instance();
      if (!tts.isLoaded())
        tts.init();
      const TtsRequest request{.text = text,
                               .lang = lang == "en" ? TtsLang::EN : TtsLang::ES,
                               .voiceId = "M3",
                               .quality = TtsQuality::Auto,
                               .speed = tts.defaultSpeed()};
      const auto floats = tts.synthesize(request);
      std::vector<int16_t> samples;
      samples.reserve(floats.size());
      for (const float sample : floats) {
        const float clamped = std::clamp(sample, -1.0F, 1.0F);
        samples.push_back(static_cast<int16_t>(clamped * 32767.0F));
      }

      LabConfig config;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        config = config_;
      }
      TapoTalkClient client({.host = config.cameraHost,
                             .port = config.mediaPort,
                             .username = "admin",
                             .cloudPassword = config.cloudPassword,
                             .mode = config.talkMode,
                             .connectTimeoutMs = config.connectTimeoutMs,
                             .ioTimeoutMs = config.requestTimeoutMs,
                             .packetMs = config.talkPacketMs,
                             .pace = true,
                             .framing = tapoTalkFramingFromString(config.talkFraming),
                             .ts = {}});
      const auto sent = client.sendChunk(
          {.samples = std::move(samples),
           .sampleRate = tts.sampleRate(),
           .reopenOnFailure = true},
          ticket->token);
      {
        std::lock_guard<std::mutex> lock(arbiterMutex_);
        arbiter_.finish(*ticket);
      }
      if (!sent.ok)
        return failure(502, sent.error);
      Json::Value data = sent.data;
      data["source"] = source;
      data["cancelled"] = ticket->token.cancelled();
      return success(std::move(data));
    }
    catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(arbiterMutex_);
      arbiter_.finish(*ticket);
      return failure(500, error.what());
    }
  }

private:
  TapoResult ensureConnected()
  {
    if (api_ && api_->isConnected())
      return TapoResult::success(Json::Value());
    LabConfig config;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      config = config_;
    }
    if (config.cameraHost.empty())
      return TapoResult::failure("Set camera.host in labs/camera-control/config.toml");
    api_ = std::make_unique<TapoApi>(makeClientConfig(config));
    const auto connected = api_->connect();
    if (!connected.ok)
      api_.reset();
    return connected;
  }

  static LabResult toLab(const TapoResult& result)
  {
    return result.ok ? success(result.data) : failure(502, result.error);
  }

  mutable std::mutex mutex_;
  LabConfig config_;
  std::mutex deviceMutex_;
  std::unique_ptr<TapoApi> api_;
  mutable std::mutex arbiterMutex_;
  TalkArbiter arbiter_;
};

drogon::HttpResponsePtr responseFor(const LabResult& result)
{
  Json::Value body;
  body["ok"] = result.ok;
  if (result.ok)
    body["data"] = result.data;
  else
    body["error"] = result.error;
  auto response = drogon::HttpResponse::newHttpJsonResponse(body);
  response->setStatusCode(static_cast<drogon::HttpStatusCode>(result.status));
  response->addHeader("Access-Control-Allow-Origin", "*");
  response->addHeader("Access-Control-Allow-Methods", "GET, POST, PATCH, PUT, OPTIONS");
  response->addHeader("Access-Control-Allow-Headers", "Content-Type");
  return response;
}

class DetectionSocket : public drogon::WebSocketController<DetectionSocket>
{
public:
  void handleNewMessage(const drogon::WebSocketConnectionPtr&,
                        std::string&&,
                        const drogon::WebSocketMessageType&) override
  {
  }

  void handleNewConnection(const drogon::HttpRequestPtr&,
                           const drogon::WebSocketConnectionPtr& connection) override
  {
    DetectionEngine::instance().add(connection);
  }

  void handleConnectionClosed(
      const drogon::WebSocketConnectionPtr& connection) override
  {
    DetectionEngine::instance().remove(connection);
  }

  WS_PATH_LIST_BEGIN
  WS_PATH_ADD("/api/detections/ws");
  WS_PATH_LIST_END
};

class CameraLabController : public drogon::HttpController<CameraLabController>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(CameraLabController::config, "/api/config", drogon::Get);
  ADD_METHOD_TO(CameraLabController::updateConfig, "/api/config", drogon::Put);
  ADD_METHOD_TO(CameraLabController::status, "/api/status", drogon::Get);
  ADD_METHOD_TO(CameraLabController::preview, "/api/preview", drogon::Get);
  ADD_METHOD_TO(CameraLabController::detections, "/api/detections", drogon::Get);
  ADD_METHOD_TO(CameraLabController::snapshot, "/api/snapshot", drogon::Get);
  ADD_METHOD_TO(CameraLabController::toggleDetections, "/api/detections/toggle", drogon::Post);
  ADD_METHOD_TO(CameraLabController::enrollFace, "/api/faces/enroll", drogon::Post);
  ADD_METHOD_TO(CameraLabController::faces, "/api/faces", drogon::Get);
  ADD_METHOD_TO(CameraLabController::capabilities, "/api/capabilities", drogon::Get);
  ADD_METHOD_TO(CameraLabController::move, "/api/ptz", drogon::Patch);
  ADD_METHOD_TO(CameraLabController::calibrate, "/api/ptz/calibrate", drogon::Post);
  ADD_METHOD_TO(CameraLabController::stop, "/api/ptz/stop", drogon::Post);
  ADD_METHOD_TO(CameraLabController::presets, "/api/presets", drogon::Get);
  ADD_METHOD_TO(CameraLabController::preset, "/api/preset", drogon::Post);
  ADD_METHOD_TO(CameraLabController::alarm, "/api/alarm", drogon::Post);
  ADD_METHOD_TO(CameraLabController::alarmConfig, "/api/alarm/config", drogon::Get);
  ADD_METHOD_TO(CameraLabController::tapoProbe, "/api/tapo/probe", drogon::Post);
  ADD_METHOD_TO(CameraLabController::operatorStatus, "/api/operator/status", drogon::Get);
  ADD_METHOD_TO(CameraLabController::takeover, "/api/operator/takeover", drogon::Post);
  ADD_METHOD_TO(CameraLabController::releaseOperator, "/api/operator/release", drogon::Post);
  ADD_METHOD_TO(CameraLabController::talk, "/api/talk", drogon::Post);
  METHOD_LIST_END

  void config(const drogon::HttpRequestPtr&,
              std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    callback(responseFor(service_.config()));
  }

  void updateConfig(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    const auto json = req->getJsonObject();
    callback(json ? responseFor(service_.updateConfig(*json))
                  : responseFor(failure(400, "JSON body required")));
  }

  void status(const drogon::HttpRequestPtr&,
              std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    runAsync([this] { return service_.status(); }, std::move(callback));
  }

  void preview(const drogon::HttpRequestPtr&,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    callback(responseFor(service_.preview()));
  }

  void detections(const drogon::HttpRequestPtr&,
                  std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    callback(responseFor(service_.detections()));
  }

  void snapshot(const drogon::HttpRequestPtr&,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    drogon::async_run(
        [work = [this] { return service_.snapshotJpeg(); },
         callback = std::move(callback)]() mutable -> drogon::Task<> {
          try {
            const std::string bytes = work();
            auto response = drogon::HttpResponse::newHttpResponse();
            if (bytes.empty()) {
              response->setStatusCode(drogon::k503ServiceUnavailable);
              response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
              response->setBody("Sin imagen: activa el preview o la detección.");
            }
            else {
              response->setContentTypeCode(drogon::CT_IMAGE_JPG);
              response->setBody(bytes);
            }
            callback(response);
          }
          catch (const std::exception& error) {
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setStatusCode(drogon::k500InternalServerError);
            response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            response->setBody(error.what());
            callback(response);
          }
          co_return;
        });
  }

  void toggleDetections(const drogon::HttpRequestPtr&,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    runAsync([this] { return service_.toggleDetections(); }, std::move(callback));
  }

  void enrollFace(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    const auto json = req->getJsonObject();
    if (!json) {
      callback(responseFor(failure(400, "JSON body required")));
      return;
    }
    runAsync([this, body = *json] { return service_.enrollFace(body); },
             std::move(callback));
  }

  void faces(const drogon::HttpRequestPtr&,
             std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    callback(responseFor(service_.faces()));
  }

  void capabilities(const drogon::HttpRequestPtr&,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    callback(responseFor(service_.capabilities()));
  }

  void move(const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    const auto json = req->getJsonObject();
    if (!json) {
      callback(responseFor(failure(400, "JSON body required")));
      return;
    }
    runAsync([this, body = *json] { return service_.move(body); }, std::move(callback));
  }

  void calibrate(const drogon::HttpRequestPtr&,
                 std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    runAsync([this] { return service_.calibrate(); }, std::move(callback));
  }

  void stop(const drogon::HttpRequestPtr&,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    runAsync([this] { return service_.stop(); }, std::move(callback));
  }

  void presets(const drogon::HttpRequestPtr&,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    runAsync([this] { return service_.presets(); }, std::move(callback));
  }

  void preset(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    const auto json = req->getJsonObject();
    if (!json) {
      callback(responseFor(failure(400, "JSON body required")));
      return;
    }
    runAsync([this, body = *json] { return service_.preset(body); }, std::move(callback));
  }

  void alarmConfig(const drogon::HttpRequestPtr&,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    runAsync([this] { return service_.alarmConfig(); }, std::move(callback));
  }

  void tapoProbe(const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    const auto json = req->getJsonObject();
    if (!json) {
      callback(responseFor(failure(400, "JSON body required")));
      return;
    }
    runAsync([this, body = *json] { return service_.tapoProbe(body); },
             std::move(callback));
  }

  void alarm(const drogon::HttpRequestPtr& req,
             std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    const auto json = req->getJsonObject();
    if (!json) {
      callback(responseFor(failure(400, "JSON body required")));
      return;
    }
    runAsync([this, body = *json] { return service_.alarm(body); }, std::move(callback));
  }

  void operatorStatus(const drogon::HttpRequestPtr&,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    callback(responseFor(service_.operatorStatus()));
  }

  void takeover(const drogon::HttpRequestPtr&,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    callback(responseFor(service_.takeover()));
  }

  void releaseOperator(const drogon::HttpRequestPtr&,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    callback(responseFor(service_.releaseOperator()));
  }

  void talk(const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    const auto json = req->getJsonObject();
    if (!json) {
      callback(responseFor(failure(400, "JSON body required")));
      return;
    }
    runAsync([this, body = *json] { return service_.talk(body); }, std::move(callback));
  }

private:
  template <typename Work>
  void runAsync(Work work,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback)
  {
    drogon::async_run([work = std::move(work), callback = std::move(callback)]() mutable
                      -> drogon::Task<> {
      try {
        callback(responseFor(work()));
      }
      catch (const std::exception& error) {
        callback(responseFor(failure(500, error.what())));
      }
      co_return;
    });
  }

  CameraLabService service_;
};

} // namespace

int main()
{
  ConfigService::load("config.toml");
  const LabConfig config = LabConfig::load(kConfigPath);
  ::signal(SIGCHLD, SIG_IGN);
  if (ensurePreview(config))
    LOG_INFO << "go2rtc preview: " << config.go2rtcApi() << "/api/stream.mjpeg?src=labcam";

  VecDb::instance().applySchema();
  FaceService::instance().init();
  if (!FaceService::instance().isLoaded())
    LOG_WARN << "Face detection disabled: models/face missing";
  else if (config.faces.enabled)
    DetectionEngine::instance().start(config);

  drogon::app().setThreadNum(2);
  drogon::app().setDocumentRoot("labs/camera-control/web");
  drogon::app().addListener("127.0.0.1", 7090);
  LOG_INFO << "Camera control lab: http://127.0.0.1:7090/";
  drogon::app().run();

  DetectionEngine::instance().stop();
  FaceService::instance().shutdown();
}
