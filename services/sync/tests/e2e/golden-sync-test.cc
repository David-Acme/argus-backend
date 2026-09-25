#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/WebSocketClient.h>
#include <drogon/WebSocketConnection.h>
#include <json/json.h>
#include <openssl/evp.h>
#include <trantor/net/EventLoopThread.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
constexpr int kConnectTimeoutSeconds = 10;
constexpr int kFrameTimeoutSeconds = 10;
constexpr int kClosedGraceSeconds = 3;
constexpr int kVoiceQuietSeconds = 5;
constexpr int kBinaryDrainMs = 1500;
constexpr size_t kBinaryKeepBytes = 64 * 1024;
constexpr size_t kHexPreviewBytes = 256;

const std::string kRecorderUserAgent = "argus-golden-recorder/1.0";

#ifndef ARGUS_TEST_SYNC_FIXTURES_DIR
#define ARGUS_TEST_SYNC_FIXTURES_DIR "src/test/fixtures/sync"
#endif

struct Frame
{
  bool binary{false};
  std::string text;
  std::vector<unsigned char> bytes;
  size_t byteLength{0};
  std::string sha256;
  std::string hex256;
};

struct Scenario
{
  std::string name;
  std::string request;
  std::vector<Frame> frames;
};

std::string envValue(const std::string& name, const std::string& fallback = {})
{
  const char* raw = std::getenv(name.c_str());
  return raw != nullptr && *raw != '\0' ? std::string(raw) : fallback;
}

std::string jsonToString(const Json::Value& json)
{
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "  ";
  return Json::writeString(builder, json);
}

Json::Value canonicalized(const Json::Value& node)
{
  if (node.isArray()) {
    Json::Value out(Json::arrayValue);
    for (const auto& item : node)
      out.append(canonicalized(item));
    return out;
  }
  if (node.isObject()) {
    Json::Value out(Json::objectValue);
    for (const auto& key : node.getMemberNames())
      out[key] = canonicalized(node[key]);
    return out;
  }
  return node;
}

std::string canonicalJson(const Json::Value& json)
{
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, canonicalized(json));
}

std::optional<Json::Value> parseJson(const std::string& text)
{
  Json::Value json;
  Json::Reader reader;
  if (!reader.parse(text, json) || !json.isObject())
    return std::nullopt;
  return json;
}

std::string sha256Hex(const unsigned char* data, size_t len)
{
  unsigned char digest[EVP_MAX_MD_SIZE]{};
  unsigned int digestLen = 0;
  if (!EVP_Digest(data, len, digest, &digestLen, EVP_sha256(), nullptr))
    return {};
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(digestLen * 2);
  for (unsigned int i = 0; i < digestLen; ++i) {
    out.push_back(hex[digest[i] >> 4]);
    out.push_back(hex[digest[i] & 0x0f]);
  }
  return out;
}

std::string sha256OfText(const std::string& text)
{
  return sha256Hex(reinterpret_cast<const unsigned char*>(text.data()),
                   text.size());
}

std::string hexPreview(const unsigned char* data, size_t len)
{
  static constexpr char hex[] = "0123456789abcdef";
  const size_t n = std::min(len, kHexPreviewBytes);
  std::string out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    out.push_back(hex[data[i] >> 4]);
    out.push_back(hex[data[i] & 0x0f]);
  }
  return out;
}

bool endsWith(const std::string& value, const std::string& suffix)
{
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
             0;
}

bool containsLower(const std::string& value, const std::string& needle)
{
  const std::string lower = [](std::string s) {
    for (char& c : s)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  }(value);
  return lower.find(needle) != std::string::npos;
}

bool isMaskedIdKey(const std::string& key)
{
  return key == "id" || key == "sub" || key == "subId" ||
         (key.size() > 2 && endsWith(key, "Id")) ||
         (key.size() > 2 && endsWith(key, "By"));
}

bool isMaskedTimeKey(const std::string& key)
{
  return endsWith(key, "At") || endsWith(key, "_at") ||
         endsWith(key, "Date") || containsLower(key, "timestamp") ||
         key == "created" || key == "deleted";
}

bool isMaskedSecretKey(const std::string& key)
{
  return containsLower(key, "token") || containsLower(key, "hash") ||
         containsLower(key, "secret") || containsLower(key, "nonce") ||
         containsLower(key, "password") || containsLower(key, "credential");
}

void normalizeInPlace(Json::Value& node)
{
  if (node.isArray()) {
    for (auto& item : node)
      normalizeInPlace(item);
    return;
  }
  if (!node.isObject())
    return;

  for (const auto& key : node.getMemberNames()) {
    if (isMaskedIdKey(key) && node[key].isIntegral())
      node[key] = 0;
    else if (isMaskedTimeKey(key) && node[key].isIntegral())
      node[key] = 0;
    else if (isMaskedSecretKey(key) && node[key].isString())
      node[key] = "<masked>";
    else
      normalizeInPlace(node[key]);
  }
}

Frame makeTextFrame(const std::string& text)
{
  Frame frame;
  frame.binary = false;
  frame.text = text;
  frame.byteLength = text.size();
  frame.sha256 = sha256Hex(
      reinterpret_cast<const unsigned char*>(text.data()), text.size());
  return frame;
}

Frame makeBinaryFrame(const unsigned char* data, size_t len)
{
  Frame frame;
  frame.binary = true;
  frame.byteLength = len;
  const size_t kept = std::min(len, kBinaryKeepBytes);
  frame.bytes.assign(data, data + kept);
  frame.sha256 = sha256Hex(data, len);
  frame.hex256 = hexPreview(data, len);
  return frame;
}

std::string messageTypeOf(const Frame& frame)
{
  if (frame.binary)
    return "binary";
  const auto json = parseJson(frame.text);
  if (!json)
    return "invalid";
  if (json->isMember("type"))
    return (*json)["type"].asString();
  if (!json->isMember("operation"))
    return "unknown";
  static const std::string kNames[] = {
      "initial_info", "sync", "sync_audit_log", "sync_user_audit_log",
      "add",          "delete", "log",          "auth_context_changed"};
  const int op = (*json)["operation"].asInt();
  return op >= 0 && op < 8 ? kNames[op] : "unknown";
}

std::optional<Json::Value> normalizedFrameJson(const Frame& frame)
{
  const auto json = parseJson(frame.text);
  if (!json)
    return std::nullopt;
  Json::Value copy = *json;
  normalizeInPlace(copy);
  return copy;
}

Json::Value frameSummary(const Frame& frame)
{
  Json::Value summary(Json::objectValue);
  summary["kind"] = frame.binary ? "binary" : "text";
  summary["messageType"] = messageTypeOf(frame);
  summary["byteLength"] = static_cast<Json::UInt64>(frame.byteLength);
  summary["sha256"] = frame.sha256;
  if (frame.binary) {
    summary["hex256"] = frame.hex256;
    summary["keptBytes"] = static_cast<Json::UInt64>(frame.bytes.size());
  }
  return summary;
}

Json::Value scenarioNormalized(const Scenario& scenario)
{
  Json::Value out(Json::objectValue);
  out["request"] = Json::Value(Json::objectValue);
  if (!scenario.request.empty()) {
    if (auto json = parseJson(scenario.request)) {
      normalizeInPlace(*json);
      out["request"] = *json;
    }
  }
  Json::Value frames(Json::arrayValue);
  for (const auto& frame : scenario.frames) {
    if (messageTypeOf(frame) == "camera:closed")
      continue;
    Json::Value entry(Json::objectValue);
    entry["kind"] = frame.binary ? "binary" : "text";
    if (!frame.binary) {
      const auto json = normalizedFrameJson(frame);
      entry["json"] = json ? *json : Json::Value();
    }
    frames.append(entry);
  }
  out["frames"] = frames;
  return out;
}

Json::Value scenarioRaw(const Scenario& scenario)
{
  Json::Value out(Json::objectValue);
  out["request"] = scenario.request;
  Json::Value frames(Json::arrayValue);
  for (const auto& frame : scenario.frames) {
    Json::Value entry = frameSummary(frame);
    if (!frame.binary)
      entry["raw"] = frame.text;
    frames.append(entry);
  }
  out["frames"] = frames;
  return out;
}

class WaitFlag
{
public:
  void set(bool value)
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      value_ = value;
      done_ = true;
    }
    cv_.notify_all();
  }

  std::optional<bool> waitFor(std::chrono::milliseconds timeout)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, timeout, [this] { return done_; }))
      return std::nullopt;
    return value_;
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool done_{false};
  bool value_{false};
};

class FrameCollector
{
public:
  void push(Frame frame)
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.push_back(std::move(frame));
    }
    cv_.notify_all();
  }

  std::optional<Frame> take(std::chrono::milliseconds timeout)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (queue_.empty()) {
      if (cv_.wait_until(lock, deadline) == std::cv_status::timeout)
        return std::nullopt;
    }
    Frame frame = std::move(queue_.front());
    queue_.erase(queue_.begin());
    return frame;
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<Frame> queue_;
};

struct UrlParts
{
  bool ssl{true};
  std::string host;
  uint16_t port{7025};
};

std::optional<UrlParts> parseUrl(const std::string& baseUrl)
{
  const auto schemeEnd = baseUrl.find("://");
  if (schemeEnd == std::string::npos)
    return std::nullopt;
  const std::string scheme = baseUrl.substr(0, schemeEnd);
  const std::string rest = baseUrl.substr(schemeEnd + 3);
  const auto colon = rest.rfind(':');
  if (colon == std::string::npos)
    return std::nullopt;
  UrlParts parts;
  parts.ssl = scheme == "https" || scheme == "wss";
  parts.host = rest.substr(0, colon);
  parts.port = static_cast<uint16_t>(std::stoi(rest.substr(colon + 1)));
  return parts;
}

Json::Value readJsonFile(const std::string& path)
{
  std::ifstream in(path);
  if (!in)
    return Json::Value();
  Json::Value json;
  Json::Reader reader;
  if (!reader.parse(in, json))
    return Json::Value();
  return json;
}

void writeTextFile(const std::string& path, const std::string& content)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
}

struct Difference
{
  std::string path;
  Json::Value expected;
  Json::Value actual;
  bool expectedAbsent{false};
  bool actualAbsent{false};
};

void diffJson(const Json::Value& expected, const Json::Value& actual,
              const std::string& path, std::vector<Difference>& out)
{
  if (expected.isObject() && actual.isObject()) {
    std::set<std::string> keys;
    for (const auto& key : expected.getMemberNames())
      keys.insert(key);
    for (const auto& key : actual.getMemberNames())
      keys.insert(key);
    for (const auto& key : keys) {
      const std::string child = path + "." + key;
      if (!expected.isMember(key)) {
        out.push_back({.path = child,
                       .expected = Json::Value(),
                       .actual = actual[key],
                       .expectedAbsent = true,
                       .actualAbsent = false});
      }
      else if (!actual.isMember(key)) {
        out.push_back({.path = child,
                       .expected = expected[key],
                       .actual = Json::Value(),
                       .expectedAbsent = false,
                       .actualAbsent = true});
      }
      else {
        diffJson(expected[key], actual[key], child, out);
      }
    }
    return;
  }
  if (expected.isArray() && actual.isArray()) {
    if (expected.size() != actual.size()) {
      out.push_back(
          {.path = path + "[length]",
           .expected = Json::Value(static_cast<Json::UInt64>(expected.size())),
           .actual = Json::Value(static_cast<Json::UInt64>(actual.size())),
           .expectedAbsent = false,
           .actualAbsent = false});
    }
    const auto shared = std::min(expected.size(), actual.size());
    for (Json::ArrayIndex index = 0; index < shared; ++index) {
      diffJson(expected[index], actual[index],
               path + "[" + std::to_string(index) + "]", out);
    }
    return;
  }
  if (expected != actual) {
    out.push_back({.path = path,
                   .expected = expected,
                   .actual = actual,
                   .expectedAbsent = false,
                   .actualAbsent = false});
  }
}

struct AcceptedAddition
{
  std::string path;
  Json::Value value;
};

std::vector<AcceptedAddition> acceptedAdditions(const Json::Value& manifest)
{
  std::vector<AcceptedAddition> additions;
  if (!manifest.isObject() || !manifest["acceptedAdditions"].isArray())
    return additions;
  for (const auto& entry : manifest["acceptedAdditions"]) {
    if (!entry.isObject() || !entry["path"].isString())
      continue;
    additions.push_back(
        {.path = entry["path"].asString(), .value = entry["value"]});
  }
  return additions;
}

bool isAcceptedAddition(const Difference& difference,
                        const std::vector<AcceptedAddition>& additions)
{
  if (!difference.expectedAbsent)
    return false;
  return std::any_of(additions.begin(), additions.end(),
                     [&difference](const AcceptedAddition& addition) {
                       return addition.path == difference.path &&
                              addition.value == difference.actual;
                     });
}

void reportDifferences(const Scenario& scenario,
                       const std::vector<Difference>& differences)
{
  constexpr size_t kMaxReported = 20;
  std::cout << "  MISMATCH " << scenario.name << ": " << differences.size()
            << " difference(s) against the committed fixture\n";
  const size_t reported = std::min(differences.size(), kMaxReported);
  for (size_t index = 0; index < reported; ++index) {
    const auto& difference = differences[index];
    std::cout << "    " << difference.path << "\n";
    std::cout << "      expected "
              << (difference.expectedAbsent
                      ? "<absent>"
                      : canonicalJson(difference.expected))
              << "\n";
    std::cout << "      actual   "
              << (difference.actualAbsent ? "<absent>"
                                          : canonicalJson(difference.actual))
              << "\n";
  }
  if (differences.size() > reported)
    std::cout << "    ... " << differences.size() - reported << " more\n";
}

struct VerifyInput
{
  const std::string& fixturesDir;
  const Scenario& scenario;
  const Json::Value& manifest;
};

std::vector<const Json::Value*> outgoingPins(const Json::Value& manifest,
                                             const std::string& fixture)
{
  std::vector<const Json::Value*> pins;
  if (!manifest.isObject() || !manifest["frames"].isArray())
    return pins;
  for (const auto& entry : manifest["frames"]) {
    if (!entry.isObject())
      continue;
    if (entry.get("direction", "").asString() != "out")
      continue;
    if (entry.get("fixture", "").asString() != fixture)
      continue;
    pins.push_back(&entry);
  }
  return pins;
}

bool verifyRequestBytes(const VerifyInput& input)
{
  const auto pins = outgoingPins(input.manifest, input.scenario.name + ".json");
  if (pins.empty())
    return input.scenario.request.empty();
  if (input.scenario.request.empty()) {
    std::cout << "  MISMATCH " << input.scenario.name
              << ": the committed session sent a request this replay does not\n";
    return false;
  }
  const Frame outgoing = makeTextFrame(input.scenario.request);
  const Json::Value& pin = *pins.front();
  const std::string pinnedSha = pin.get("sha256", "").asString();
  const auto pinnedLength = pin.get("byteLength", 0).asUInt64();
  if (pinnedSha != outgoing.sha256 ||
      pinnedLength != static_cast<Json::UInt64>(outgoing.byteLength)) {
    std::cout << "  MISMATCH " << input.scenario.name
              << ": request bytes drifted from the committed frame\n";
    std::cout << "    expected: " << pinnedLength << " bytes sha256 "
              << pinnedSha << "\n";
    std::cout << "    actual:   " << outgoing.byteLength << " bytes sha256 "
              << outgoing.sha256 << "\n";
    return false;
  }
  return true;
}

std::string acceptedSuffix(size_t additions)
{
  if (additions == 0)
    return {};
  return ", " + std::to_string(additions) + " accepted addition(s)";
}

bool verifyScenario(const VerifyInput& input)
{
  const Json::Value expected =
      readJsonFile(input.fixturesDir + "/" + input.scenario.name + ".json");
  if (expected.isNull() || !expected.isObject()) {
    std::cout << "  MISMATCH " << input.scenario.name
              << ": cannot read the committed fixture\n";
    return false;
  }
  const Json::Value actual = scenarioNormalized(input.scenario);

  std::vector<Difference> differences;
  diffJson(expected, actual, input.scenario.name, differences);
  const auto additions = acceptedAdditions(input.manifest);
  std::vector<Difference> unexplained;
  size_t explained = 0;
  for (const auto& difference : differences) {
    if (isAcceptedAddition(difference, additions))
      ++explained;
    else
      unexplained.push_back(difference);
  }
  if (!unexplained.empty()) {
    reportDifferences(input.scenario, unexplained);
    return false;
  }

  const std::string expectedBytes = canonicalJson(expected);
  const std::string actualBytes = canonicalJson(actual);
  if (explained == 0 && expectedBytes != actualBytes) {
    std::cout << "  MISMATCH " << input.scenario.name
              << ": canonical bytes differ without a reported field\n";
    return false;
  }
  if (!verifyRequestBytes(input))
    return false;

  std::cout << "  OK " << input.scenario.name << " ("
            << input.scenario.frames.size() << " frame(s), "
            << actualBytes.size() << " normalized bytes, sha256 "
            << sha256OfText(actualBytes) << acceptedSuffix(explained) << ")\n";
  return true;
}

struct SocketSession
{
  std::string label;
  drogon::WebSocketClientPtr client;
  drogon::WebSocketConnectionPtr connection;
  FrameCollector collector;
  std::mutex closedMutex;
  std::condition_variable closedCv;
  bool closed{false};
};

struct OpenSocketInput
{
  const UrlParts& parts;
  const std::string& path;
  const std::string& token;
};

bool openSocket(SocketSession& session, const OpenSocketInput& input,
                trantor::EventLoopThread& loop)
{
  session.client = drogon::WebSocketClient::newWebSocketClient(
      std::string(input.parts.ssl ? "wss://" : "ws://") + input.parts.host +
          ":" + std::to_string(input.parts.port),
      loop.getLoop(), false, false);
  session.client->setMessageHandler(
      [&session](std::string&& message, const drogon::WebSocketClientPtr&,
                 const drogon::WebSocketMessageType& type) {
        const auto* data =
            reinterpret_cast<const unsigned char*>(message.data());
        if (type == drogon::WebSocketMessageType::Binary)
          session.collector.push(makeBinaryFrame(data, message.size()));
        else if (type == drogon::WebSocketMessageType::Text)
          session.collector.push(makeTextFrame(message));
      });
  session.client->setConnectionClosedHandler(
      [&session](const drogon::WebSocketClientPtr&) {
        {
          std::lock_guard<std::mutex> lock(session.closedMutex);
          session.closed = true;
        }
        session.closedCv.notify_all();
      });

  auto request = drogon::HttpRequest::newHttpRequest();
  request->setPath(input.path);
  request->setParameter("token", input.token);
  request->addHeader("User-Agent", kRecorderUserAgent);

  WaitFlag connected;
  session.client->connectToServer(
      request,
      [&connected](drogon::ReqResult result, const drogon::HttpResponsePtr&,
                   const drogon::WebSocketClientPtr& client) {
        connected.set(result == drogon::ReqResult::Ok &&
                      client->getConnection() != nullptr);
      });
  const auto outcome =
      connected.waitFor(std::chrono::seconds(kConnectTimeoutSeconds + 2));
  if (!outcome || !*outcome)
    return false;
  session.connection = session.client->getConnection();
  return true;
}

void closeSocket(SocketSession& session)
{
  if (session.connection)
    session.connection->shutdown();
  std::unique_lock<std::mutex> lock(session.closedMutex);
  session.closedCv.wait_for(lock, std::chrono::seconds(3),
                            [&session] { return session.closed; });
}

}

int main(int argc, char* argv[])
{
  std::string mode = argc > 1 ? argv[1] : std::string();

  const std::string baseUrl = envValue("ARGUS_TEST_BASE_URL",
                                       "https://127.0.0.1:7025");
  const std::string fixturesDir =
      envValue("ARGUS_TEST_FIXTURES_DIR", ARGUS_TEST_SYNC_FIXTURES_DIR);
  const std::string refreshToken = envValue("ARGUS_TEST_REFRESH_TOKEN");

  if (refreshToken.empty()) {
    std::cout << "SKIP: no ARGUS_TEST_REFRESH_TOKEN provided; golden /sync "
                 "recording needs a locally running backend and a test "
                 "session\n";
    return 0;
  }

  const std::string authBaseUrl =
      envValue("ARGUS_TEST_AUTH_BASE_URL", baseUrl);
  const std::string mediaBaseUrl =
      envValue("ARGUS_TEST_MEDIA_BASE_URL", "https://127.0.0.1:7026");

  const auto url = parseUrl(baseUrl);
  if (!url) {
    std::cout << "SKIP: invalid ARGUS_TEST_BASE_URL '" << baseUrl << "'\n";
    return 0;
  }
  const auto authUrl = parseUrl(authBaseUrl);
  if (!authUrl) {
    std::cout << "SKIP: invalid ARGUS_TEST_AUTH_BASE_URL '" << authBaseUrl
              << "'\n";
    return 0;
  }
  const auto mediaUrl = parseUrl(mediaBaseUrl);
  if (!mediaUrl) {
    std::cout << "SKIP: invalid ARGUS_TEST_MEDIA_BASE_URL '" << mediaBaseUrl
              << "'\n";
    return 0;
  }

  trantor::EventLoopThread loopThread;
  loopThread.run();

  const std::string httpHost =
      std::string(authUrl->ssl ? "https://" : "http://") + authUrl->host + ":" +
      std::to_string(authUrl->port);
  auto httpClient =
      drogon::HttpClient::newHttpClient(httpHost, loopThread.getLoop(), false,
                                        false);
  httpClient->setUserAgent(kRecorderUserAgent);

  Json::Value body;
  body["refreshToken"] = refreshToken;
  auto authReq = drogon::HttpRequest::newHttpJsonRequest(body);
  authReq->setMethod(drogon::Patch);
  authReq->setPath("/auth/refresh-token");
  authReq->addHeader("User-Agent", kRecorderUserAgent);

  Json::Value session(Json::objectValue);
  std::string authFailure;
  {
    WaitFlag done;
    httpClient->sendRequest(
        authReq,
        [&done, &session, &authFailure](drogon::ReqResult result,
                                        const drogon::HttpResponsePtr& response) {
          if (result != drogon::ReqResult::Ok) {
            authFailure = "transport result " + std::to_string(
                            static_cast<int>(result));
            done.set(false);
            return;
          }
          if (!response) {
            authFailure = "empty response";
            done.set(false);
            return;
          }
          if (response->getStatusCode() != drogon::k200OK) {
            authFailure = "status " +
                          std::to_string(response->getStatusCode()) + " body " +
                          std::string(response->getBody());
            done.set(false);
            return;
          }
          const auto json = parseJson(std::string(response->getBody()));
          if (!json) {
            authFailure = "non-JSON body";
            done.set(false);
            return;
          }
          session = (*json)["info"].isObject() ? (*json)["info"] : *json;
          done.set(true);
        },
        static_cast<double>(kConnectTimeoutSeconds));

    const auto outcome =
        done.waitFor(std::chrono::seconds(kConnectTimeoutSeconds + 2));
    if (!outcome) {
      std::cout << "SKIP: backend not reachable at " << authBaseUrl << "\n";
      return 0;
    }
    if (!*outcome) {
      std::cout << "SKIP: /auth/refresh-token did not accept the test session"
                << " (unreachable server, expired token or wrong UA)\n";
      if (!authFailure.empty())
        std::cout << "      detail: " << authFailure << "\n";
      return 0;
    }
  }

  if (!session.isMember("accessToken") || !session.isMember("refreshToken")) {
    std::cout << "SKIP: unexpected /auth/refresh-token response shape\n";
    return 0;
  }
  const std::string accessToken = session["accessToken"].asString();
  std::cout << "auth ok; the rotated ARGUS_TEST_REFRESH_TOKEN must be reused "
               "by the next recording run\n";

  trantor::EventLoopThread wsLoopThread;
  wsLoopThread.run();

  SocketSession syncSocket;
  syncSocket.label = "sync";
  if (!openSocket(syncSocket,
                  {.parts = *url, .path = "/sync", .token = accessToken},
                  wsLoopThread)) {
    std::cout << "SKIP: could not open /sync WebSocket at " << baseUrl << "\n";
    return 0;
  }
  std::cout << "ws connected: " << baseUrl << "/sync\n";

  SocketSession mediaSocket;
  mediaSocket.label = "media";
  if (!openSocket(mediaSocket,
                  {.parts = *mediaUrl,
                   .path = "/media",
                   .token = accessToken},
                  wsLoopThread)) {
    std::cout << "SKIP: could not open /media WebSocket at " << mediaBaseUrl
              << "\n";
    return 0;
  }
  std::cout << "ws connected: " << mediaBaseUrl << "/media\n";

  std::vector<Scenario> scenarios;
  const auto runScenario = [&](SocketSession& session, Scenario scenario,
                               const std::function<bool(const Frame&)>& more) {
    if (!scenario.request.empty() && session.connection) {
      session.connection->send(scenario.request);
      std::cout << "  > " << messageTypeOf(makeTextFrame(scenario.request))
                << "\n";
    }
    bool accepted = false;
    while (true) {
      auto frame =
          session.collector.take(std::chrono::seconds(kFrameTimeoutSeconds));
      if (!frame) {
        std::cout << "  TIMEOUT waiting for a frame in " << scenario.name
                  << "\n";
        break;
      }
      scenario.frames.push_back(*frame);
      accepted = true;
      std::cout << "  < " << messageTypeOf(*frame) << "\n";
      if (!more || !more(*frame))
        break;
    }
    if (accepted)
      scenarios.push_back(std::move(scenario));
  };

  const auto stopOnFirst = [](const Frame&) { return false; };
  const auto drainBinary = [](const Frame& frame) { return frame.binary; };

  Scenario initialInfo;
  initialInfo.name = "initial-info";
  runScenario(syncSocket, std::move(initialInfo), stopOnFirst);

  const std::string fullBody = R"({"requiredCreate":true,"findLastCreated":true,)"
                               R"("requiredDeleted":true,"findLastDeleted":true})";
  std::string syncPayload = "{";
  static const char* kTables[] = {
      "user", "user_invitation", "camera", "camera_stream", "zone", "reminder",
      "reminder_detail", "calendar_event", "calendar_event_share", "project",
      "project_member", "project_task", "event", "person"};
  for (const char* table : kTables)
    syncPayload += std::string("\"") + table + "\":" + fullBody + ",";
  syncPayload +=
      "\"notification\":{\"requiredCreate\":true,\"findLastCreated\":true}}";

  {
    Scenario bootstrap;
    bootstrap.name = "sync-bootstrap";
    bootstrap.request = "{\"type\":\"sync\",\"payload\":" + syncPayload + "}";
    runScenario(syncSocket, std::move(bootstrap), stopOnFirst);
  }

  const auto watermarkOf = [&scenarios]() -> std::optional<int64_t> {
    if (scenarios.empty() || scenarios.back().frames.empty())
      return std::nullopt;
    const auto json = parseJson(scenarios.back().frames.front().text);
    if (!json || !(*json)["info"].isObject() ||
        !(*json)["info"].isMember("watermarkId") ||
        !(*json)["info"]["watermarkId"].isIntegral())
      return std::nullopt;
    const int64_t value = (*json)["info"]["watermarkId"].asInt64();
    return value > 0 ? std::optional<int64_t>(value) : std::nullopt;
  };

  {
    Scenario watermark;
    watermark.name = "sync-audit-log-watermark";
    watermark.request =
        "{\"type\":\"sync_audit_log\",\"payload\":{\"findLast\":true}}";
    runScenario(syncSocket, std::move(watermark), stopOnFirst);
    if (const auto wm = watermarkOf()) {
      Scenario page;
      page.name = "sync-audit-log-page";
      page.request = "{\"type\":\"sync_audit_log\",\"payload\":{\"afterId\":0,"
                     "\"endId\":" + std::to_string(*wm) + "}}";
      runScenario(syncSocket, std::move(page), stopOnFirst);
    }
  }

  {
    Scenario watermark;
    watermark.name = "sync-user-audit-log-watermark";
    watermark.request =
        "{\"type\":\"sync_user_audit_log\",\"payload\":{\"findLast\":true}}";
    runScenario(syncSocket, std::move(watermark), stopOnFirst);
    if (const auto wm = watermarkOf()) {
      Scenario page;
      page.name = "sync-user-audit-log-page";
      page.request = "{\"type\":\"sync_user_audit_log\",\"payload\":"
                     "{\"afterId\":0,\"endId\":" +
                     std::to_string(*wm) + "}}";
      runScenario(syncSocket, std::move(page), stopOnFirst);
    }
  }

  {
    Scenario subscribe;
    subscribe.name = "camera-subscribe";
    subscribe.request = "{\"type\":\"camera:subscribe\",\"payload\":"
                        "{\"cameraId\":1,\"quality\":\"main\"}}";
    runScenario(mediaSocket, std::move(subscribe), drainBinary);
    if (!scenarios.empty() && !scenarios.back().frames.empty()) {
      const auto json = parseJson(scenarios.back().frames.front().text);
      if (json && (*json)["type"] == "camera:ready" &&
          mediaSocket.connection) {
        const int subId = (*json)["payload"].get("subId", 0).asInt();
        mediaSocket.connection->send(
            "{\"type\":\"camera:unsubscribe\",\"payload\":"
            "{\"subId\":" + std::to_string(subId) + "}}");
        while (auto frame = mediaSocket.collector.take(
                   std::chrono::seconds(kClosedGraceSeconds))) {
          scenarios.back().frames.push_back(*frame);
          std::cout << "  < " << messageTypeOf(*frame) << "\n";
          if (messageTypeOf(*frame) == "camera:closed")
            break;
        }
      }
    }
  }

  {
    Scenario unknown;
    unknown.name = "unknown-type-error";
    unknown.request = "{\"type\":\"__golden_probe__\",\"payload\":{}}";
    runScenario(syncSocket, std::move(unknown), stopOnFirst);
  }

  {
    Scenario voice;
    voice.name = "voice-start-stop";
    if (syncSocket.connection) {
      syncSocket.connection->send("{\"type\":\"voice:start\",\"payload\":{}}");
      std::cout << "  > voice:start\n";
    }
    bool greeted = false;
    while (auto frame = syncSocket.collector.take(std::chrono::seconds(
               greeted ? kVoiceQuietSeconds : kFrameTimeoutSeconds))) {
      voice.frames.push_back(*frame);
      greeted = true;
      std::cout << "  < " << messageTypeOf(*frame) << "\n";
    }
    if (!voice.frames.empty() && syncSocket.connection) {
      syncSocket.connection->send("{\"type\":\"voice:stop\",\"payload\":{}}");
      std::cout << "  > voice:stop\n";
    }
    while (auto frame = syncSocket.collector.take(
               std::chrono::seconds(kFrameTimeoutSeconds))) {
      voice.frames.push_back(*frame);
      std::cout << "  < " << messageTypeOf(*frame) << "\n";
      if (messageTypeOf(*frame) == "voice:done")
        break;
    }
    if (!voice.frames.empty())
      scenarios.push_back(std::move(voice));
    else
      std::cout << "  voice-start-stop captured no frames\n";
  }

  closeSocket(syncSocket);
  closeSocket(mediaSocket);

  std::filesystem::create_directories(fixturesDir);
  const std::string manifestPath = fixturesDir + "/manifest.json";
  const bool verifyMode =
      mode == "verify" ||
      (mode.empty() && std::filesystem::exists(manifestPath));

  const auto committedFixtures = [](const Json::Value& manifest) {
    std::set<std::string> fixtures;
    if (manifest.isObject() && manifest["frames"].isArray()) {
      for (const auto& entry : manifest["frames"]) {
        if (entry.isObject() && entry.isMember("fixture") &&
            entry["fixture"].isString())
          fixtures.insert(entry["fixture"].asString());
      }
    }
    return fixtures;
  };

  if (mode != "record" && verifyMode) {
    std::cout << "verify against " << fixturesDir << "\n";
    bool ok = true;

    const Json::Value manifest = readJsonFile(manifestPath);
    if (!manifest.isObject()) {
      std::cout << "  MISMATCH manifest: cannot read " << manifestPath << "\n";
      ok = false;
    }
    const std::set<std::string> committed = committedFixtures(manifest);
    for (const auto& scenario : scenarios) {
      if (!committed.count(scenario.name + ".json"))
        continue;
      ok = verifyScenario({.fixturesDir = fixturesDir,
                           .scenario = scenario,
                           .manifest = manifest}) &&
           ok;
    }
    for (const auto& fixture : committed) {
      if (!endsWith(fixture, ".json"))
        continue;
      const std::string name = fixture.substr(0, fixture.size() - 5);
      const auto captured = std::find_if(
          scenarios.begin(), scenarios.end(),
          [&name](const Scenario& scenario) { return scenario.name == name; });
      if (captured == scenarios.end() || captured->frames.empty()) {
        std::cout << "  MISMATCH " << name
                  << ": committed scenario captured no frames this session\n";
        ok = false;
      }
    }

    if (!ok) {
      std::cout << "FAIL: golden /sync contract drifted from fixtures\n";
      return 1;
    }
    std::cout << "PASS: golden /sync contract matches fixtures\n";
    return 0;
  }

  const Json::Value previous = readJsonFile(manifestPath);
  const std::set<std::string> committed = committedFixtures(previous);
  const bool firstRecord = !previous.isObject();

  Json::Value manifest(Json::objectValue);
  manifest["generator"] = "golden-sync-test";
  {
    std::array<char, 32> now{};
    const std::time_t t = std::time(nullptr);
    std::strftime(now.data(), now.size(), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    manifest["recordedAtUtc"] = now.data();
  }
  manifest["baseUrl"] = baseUrl;
  manifest["authBaseUrl"] = authBaseUrl;
  manifest["mediaBaseUrl"] = mediaBaseUrl;
  manifest["userAgent"] = kRecorderUserAgent;

  Json::Value rules(Json::objectValue);
  rules["description"] =
      "Fixtures compare structurally: per-boot-varying values are masked in "
      "the normalized .json files; the .raw.json files keep the session as "
      "received.";
  rules["maskedIds"] =
      "key == id/sub/subId, key ending in 'Id' or 'By' (integer) -> 0";
  rules["maskedTimestamps"] =
      "key ending in 'At'/'_at'/'Date', containing 'timestamp', or exactly "
      "'created'/'deleted' (integer) -> 0";
  rules["maskedSecrets"] =
      "key containing token/hash/secret/nonce/password/credential (string) -> "
      "'<masked>'";
  rules["binaryFrames"] =
      "normalized files record kind only; raw files record byteLength, sha256 "
      "and the first 256 bytes hex; the first 64 KiB of a binary frame is also "
      "stored as .bin";
  rules["cameraClosedFrames"] =
      "camera:closed is emitted by the go2rtc relay on its own retry "
      "schedule, so its arrival is timing-dependent; it is kept in the raw "
      "fixtures but excluded from the normalized comparison";
  manifest["normalization"] = rules;

  Json::Value frames(Json::arrayValue);
  int index = 0;
  int recorded = 0;
  for (const auto& scenario : scenarios) {
    if (!firstRecord && !committed.count(scenario.name + ".json"))
      continue;
    ++recorded;
    if (!scenario.request.empty()) {
      const Frame outgoing = makeTextFrame(scenario.request);
      Json::Value requestEntry = frameSummary(outgoing);
      requestEntry["index"] = index++;
      requestEntry["direction"] = "out";
      requestEntry["fixture"] = scenario.name + ".json";
      frames.append(requestEntry);
    }

    for (const auto& frame : scenario.frames) {
      Json::Value entry = frameSummary(frame);
      entry["index"] = index++;
      entry["direction"] = "in";
      entry["scenario"] = scenario.name;
      entry["fixture"] = scenario.name + ".json";
      frames.append(entry);
    }

    writeTextFile(fixturesDir + "/" + scenario.name + ".json",
                  jsonToString(scenarioNormalized(scenario)) + "\n");
    writeTextFile(fixturesDir + "/" + scenario.name + ".raw.json",
                  jsonToString(scenarioRaw(scenario)) + "\n");
    if (!scenario.frames.empty() && scenario.frames.front().binary) {
      std::ofstream bin(fixturesDir + "/" + scenario.name + ".bin",
                        std::ios::binary | std::ios::trunc);
      const auto& bytes = scenario.frames.front().bytes;
      bin.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
  }
  manifest["frames"] = frames;
  writeTextFile(manifestPath, jsonToString(manifest) + "\n");

  std::cout << "recorded " << recorded << " scenario(s), " << index
            << " frame(s) into " << fixturesDir << "\n";
  return 0;
}
