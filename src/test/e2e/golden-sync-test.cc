// Golden /sync frame recorder. A scripted WebSocket client that plays the
// real client flow (refresh-token auth, bootstrap, audit syncs, camera
// subscribe) against a locally running backend and either records the
// resulting frames as fixtures or verifies a new session against them.
// Read-only: it never touches camera control or alarm endpoints.

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
#include <sstream>
#include <string>
#include <vector>

namespace
{
constexpr int kConnectTimeoutSeconds = 10;
constexpr int kFrameTimeoutSeconds = 10;
constexpr int kClosedGraceSeconds = 3;
constexpr int kBinaryDrainMs = 1500;
constexpr size_t kBinaryKeepBytes = 64 * 1024;
constexpr size_t kHexPreviewBytes = 256;

const std::string kRecorderUserAgent = "argus-golden-recorder/1.0";

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

// Per-boot-varying values (ids, timestamps, tokens, nonces) are masked so the
// fixtures compare structurally across migrations instead of byte-wise.
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
    // go2rtc emits camera:closed on its own retry schedule; compare without it.
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
  uint16_t port{7024};
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

void reportMismatch(const Scenario& scenario,
                    const std::string& expected,
                    const std::string& actual)
{
  std::cout << "  MISMATCH " << scenario.name << ": normalized session "
            << "differs from the committed fixture\n";
  std::cout << "    expected: " << expected << "\n";
  std::cout << "    actual:   " << actual << "\n";
}

bool verifyScenario(const std::string& fixturesDir, const Scenario& scenario)
{
  const Json::Value expected =
      readJsonFile(fixturesDir + "/" + scenario.name + ".json");
  if (expected.isNull() || !expected.isObject()) {
    reportMismatch(scenario, "<fixture file>", "<missing>");
    return false;
  }
  const Json::Value actual = scenarioNormalized(scenario);
  if (expected != actual) {
    reportMismatch(scenario, jsonToString(expected), jsonToString(actual));
    return false;
  }
  std::cout << "  OK " << scenario.name << " (" << scenario.frames.size()
            << " frame(s))\n";
  return true;
}

} // namespace

int main(int argc, char* argv[])
{
  std::string mode = argc > 1 ? argv[1] : std::string();

  const std::string baseUrl = envValue("ARGUS_TEST_BASE_URL",
                                       "https://127.0.0.1:7024");
  const std::string fixturesDir =
      envValue("ARGUS_TEST_FIXTURES_DIR", "src/test/fixtures/sync");
  const std::string refreshToken = envValue("ARGUS_TEST_REFRESH_TOKEN");

  if (refreshToken.empty()) {
    std::cout << "SKIP: no ARGUS_TEST_REFRESH_TOKEN provided; golden /sync "
                 "recording needs a locally running backend and a test "
                 "session\n";
    return 0;
  }

  const auto url = parseUrl(baseUrl);
  if (!url) {
    std::cout << "SKIP: invalid ARGUS_TEST_BASE_URL '" << baseUrl << "'\n";
    return 0;
  }

  trantor::EventLoopThread loopThread;
  loopThread.run();

  const std::string httpHost =
      std::string(url->ssl ? "https://" : "http://") + url->host + ":" +
      std::to_string(url->port);
  auto httpClient =
      drogon::HttpClient::newHttpClient(httpHost, loopThread.getLoop(), false,
                                        false);
  // Drogon stamps its own default user agent over the request header.
  httpClient->setUserAgent(kRecorderUserAgent);

  // Step 1: rotate the test refresh token over the real HTTP auth endpoint.
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
      std::cout << "SKIP: backend not reachable at " << baseUrl << "\n";
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

  // Step 2: open /sync with the rotated access token.
  trantor::EventLoopThread wsLoopThread;
  wsLoopThread.run();

  const std::string wsHost =
      std::string(url->ssl ? "wss://" : "ws://") + url->host + ":" +
      std::to_string(url->port);
  auto wsClient = drogon::WebSocketClient::newWebSocketClient(
      wsHost, wsLoopThread.getLoop(), false, false);

  FrameCollector collector;
  std::mutex closedMutex;
  std::condition_variable closedCv;
  bool serverClosed = false;

  wsClient->setMessageHandler(
      [&collector](std::string&& message,
                   const drogon::WebSocketClientPtr&,
                   const drogon::WebSocketMessageType& type) {
        const auto* data = reinterpret_cast<const unsigned char*>(message.data());
        if (type == drogon::WebSocketMessageType::Binary)
          collector.push(makeBinaryFrame(data, message.size()));
        else if (type == drogon::WebSocketMessageType::Text)
          collector.push(makeTextFrame(message));
      });

  wsClient->setConnectionClosedHandler(
      [&serverClosed, &closedMutex, &closedCv](
          const drogon::WebSocketClientPtr&) {
        std::lock_guard<std::mutex> lock(closedMutex);
        serverClosed = true;
        closedCv.notify_all();
      });

  auto connectReq = drogon::HttpRequest::newHttpRequest();
  connectReq->setPath("/sync");
  connectReq->setParameter("token", accessToken);
  connectReq->addHeader("User-Agent", kRecorderUserAgent);

  {
    WaitFlag connected;
    wsClient->connectToServer(
        connectReq,
        [&connected](drogon::ReqResult result,
                     const drogon::HttpResponsePtr&,
                     const drogon::WebSocketClientPtr& client) {
          connected.set(result == drogon::ReqResult::Ok &&
                        client->getConnection() != nullptr);
        });

    const auto outcome =
        connected.waitFor(std::chrono::seconds(kConnectTimeoutSeconds + 2));
    if (!outcome || !*outcome) {
      std::cout << "SKIP: could not open /sync WebSocket at " << baseUrl
                << "\n";
      return 0;
    }
  }
  const drogon::WebSocketConnectionPtr connection = wsClient->getConnection();
  std::cout << "ws connected: " << baseUrl << "/sync\n";

  // Step 3: play the client scenarios and capture every incoming frame.
  std::vector<Scenario> scenarios;
  const auto runScenario = [&](Scenario scenario,
                               const std::function<bool(const Frame&)>& more) {
    if (!scenario.request.empty() && connection) {
      connection->send(scenario.request);
      std::cout << "  > " << messageTypeOf(makeTextFrame(scenario.request))
                << "\n";
    }
    bool accepted = false;
    while (true) {
      auto frame = collector.take(std::chrono::seconds(kFrameTimeoutSeconds));
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
  runScenario(std::move(initialInfo), stopOnFirst);

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
    runScenario(std::move(bootstrap), stopOnFirst);
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
    runScenario(std::move(watermark), stopOnFirst);
    if (const auto wm = watermarkOf()) {
      Scenario page;
      page.name = "sync-audit-log-page";
      page.request = "{\"type\":\"sync_audit_log\",\"payload\":{\"afterId\":0,"
                     "\"endId\":" + std::to_string(*wm) + "}}";
      runScenario(std::move(page), stopOnFirst);
    }
  }

  {
    Scenario watermark;
    watermark.name = "sync-user-audit-log-watermark";
    watermark.request =
        "{\"type\":\"sync_user_audit_log\",\"payload\":{\"findLast\":true}}";
    runScenario(std::move(watermark), stopOnFirst);
    if (const auto wm = watermarkOf()) {
      Scenario page;
      page.name = "sync-user-audit-log-page";
      page.request = "{\"type\":\"sync_user_audit_log\",\"payload\":"
                     "{\"afterId\":0,\"endId\":" +
                     std::to_string(*wm) + "}}";
      runScenario(std::move(page), stopOnFirst);
    }
  }

  {
    Scenario subscribe;
    subscribe.name = "camera-subscribe";
    subscribe.request = "{\"type\":\"camera:subscribe\",\"payload\":"
                        "{\"cameraId\":1,\"quality\":\"main\"}}";
    runScenario(std::move(subscribe), drainBinary);
    if (!scenarios.empty() && !scenarios.back().frames.empty()) {
      const auto json = parseJson(scenarios.back().frames.front().text);
      if (json && (*json)["type"] == "camera:ready" && connection) {
        const int subId = (*json)["payload"].get("subId", 0).asInt();
        connection->send("{\"type\":\"camera:unsubscribe\",\"payload\":"
                         "{\"subId\":" + std::to_string(subId) + "}}");
        // The closed event arrives on go2rtc's own schedule: collect it when
        // it shows up inside a short grace window, never wait for it.
        while (auto frame =
                   collector.take(std::chrono::seconds(kClosedGraceSeconds))) {
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
    runScenario(std::move(unknown), stopOnFirst);
  }

  if (connection)
    connection->shutdown();
  {
    std::unique_lock<std::mutex> closedLock(closedMutex);
    closedCv.wait_for(closedLock, std::chrono::seconds(3));
  }

  // Step 4: write the fixtures or verify the new session against them.
  std::filesystem::create_directories(fixturesDir);
  const std::string manifestPath = fixturesDir + "/manifest.json";
  const bool verifyMode =
      mode == "verify" ||
      (mode.empty() && std::filesystem::exists(manifestPath));

  if (mode != "record" && verifyMode) {
    std::cout << "verify against " << fixturesDir << "\n";
    bool ok = true;
    for (const auto& scenario : scenarios)
      ok = verifyScenario(fixturesDir, scenario) && ok;
    if (!ok) {
      std::cout << "FAIL: golden /sync contract drifted from fixtures\n";
      return 1;
    }
    std::cout << "PASS: golden /sync contract matches fixtures\n";
    return 0;
  }

  Json::Value manifest(Json::objectValue);
  manifest["generator"] = "golden-sync-test";
  {
    std::array<char, 32> now{};
    const std::time_t t = std::time(nullptr);
    std::strftime(now.data(), now.size(), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    manifest["recordedAtUtc"] = now.data();
  }
  manifest["baseUrl"] = baseUrl;
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
  for (const auto& scenario : scenarios) {
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

  std::cout << "recorded " << scenarios.size() << " scenario(s), " << index
            << " frame(s) into " << fixturesDir << "\n";
  return 0;
}