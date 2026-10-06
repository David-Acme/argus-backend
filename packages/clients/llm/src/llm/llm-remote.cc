#include "llm-remote.hxx"

#include <config/config-service.hxx>

#include <json/json.h>
#include <net/loopback-socket.hxx>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <stop_token>

namespace
{

constexpr const char* kChatPath = "/llm/v1/chat";
constexpr const char* kChatStreamPath = "/llm/v1/chat-stream";

std::string trim(const std::string& value)
{
  size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start])))
    ++start;
  size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1])))
    --end;
  return value.substr(start, end - start);
}

void throwEnvelopeError(int status, const std::string& body)
{
  Json::Value json;
  Json::Reader reader;
  std::string detail = "HTTP " + std::to_string(status);
  if (reader.parse(body, json) && json.isObject() &&
      json["errors"].isObject()) {
    detail = json["errors"].get("code", "").asString() + ": " +
             json["errors"].get("message", "").asString();
  }
  throw std::runtime_error("argus-llm " + detail);
}

struct HttpRequestHead
{
  const char* method{nullptr};
  const std::string* path{nullptr};
  const std::string* body{nullptr};
  const argus::net::Endpoint* address{nullptr};
  const std::string* credential{nullptr};
  bool closeConnection{true};
};

std::string serialize(const HttpRequestHead& head)
{
  std::string wire =
      std::string(head.method) + " " + *head.path + " HTTP/1.1\r\n";
  wire += "Host: " + head.address->host + "\r\n";
  wire += "Content-Type: application/json\r\n";
  wire += "Content-Length: " + std::to_string(head.body->size()) + "\r\n";
  if (head.credential && !head.credential->empty() &&
      head.credential->find_first_of("\r\n") == std::string::npos)
    wire += std::string(kCallerCredentialHeader) + ": " + *head.credential + "\r\n";
  wire += head.closeConnection ? "Connection: close\r\n\r\n" : "\r\n";
  return wire;
}

struct Head
{
  int status{0};
  bool chunked{false};
  size_t contentLength{0};
  std::string::size_type bodyStart{0};
};

Head parseHead(const std::string& wire)
{
  const auto split = wire.find("\r\n\r\n");
  if (split == std::string::npos)
    throw std::runtime_error("argus-llm response has no header block");

  const auto lineEnd = wire.find("\r\n");
  const std::string statusLine = wire.substr(0, lineEnd);
  const auto space1 = statusLine.find(' ');
  const auto space2 = statusLine.find(' ', space1 + 1);
  if (space1 == std::string::npos || space2 == std::string::npos)
    throw std::runtime_error("argus-llm malformed status line");

  Head head;
  head.status = std::stoi(statusLine.substr(space1 + 1, space2 - space1 - 1));
  head.bodyStart = split + 4;

  std::string::size_type cursor = lineEnd + 2;
  while (cursor < split) {
    const auto eol = wire.find("\r\n", cursor);
    if (eol == std::string::npos || eol > split)
      break;
    const auto colon = wire.find(':', cursor);
    if (colon != std::string::npos && colon < eol) {
      std::string name = trim(wire.substr(cursor, colon - cursor));
      std::string value = trim(wire.substr(colon + 1, eol - colon - 1));
      for (auto& c : name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      for (auto& c : value)
        c = static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
      if (name == "transfer-encoding" &&
          value.find("chunked") != std::string::npos)
        head.chunked = true;
      if (name == "content-length")
        head.contentLength =
            static_cast<size_t>(std::stoul(value));
    }
    cursor = eol + 2;
  }
  return head;
}

bool parseSentinel(const std::string& line, LlmPrefillStats* stats)
{
  Json::Value json;
  Json::Reader reader;
  if (!reader.parse(line, json, false) || !json.isObject())
    return false;
  if (!json.isMember("done") || !json["done"].asBool())
    return false;
  if (stats) {
    stats->promptTokens = json.get("prompt_tokens", 0).asInt();
    stats->reusedTokens = json.get("reused_tokens", 0).asInt();
    stats->decodedTokens = json.get("decoded_tokens", 0).asInt();
  }
  return true;
}

void throwIfCancelled(const std::stop_token& cancellation)
{
  if (cancellation.stop_requested())
    throw std::runtime_error("argus-llm stream cancelled");
}

struct SendRequestInput
{
  int fd;
  const std::string& outgoing;
  std::stop_token cancellation;
};

void sendRequest(const SendRequestInput& input)
{
  const auto status = argus::net::sendAll(
      {.fd = input.fd, .data = input.outgoing, .cancellation = input.cancellation});
  throwIfCancelled(input.cancellation);
  if (status != argus::net::NetStatus::Ok)
    throw std::runtime_error("argus-llm request send failed");
}

struct ConnectInput
{
  const std::string& baseUrl;
  const argus::net::Endpoint& address;
  int timeoutMs;
  std::stop_token cancellation;
};

argus::net::Connection connectTo(const ConnectInput& input)
{
  auto connection = argus::net::connectLoopback(
      {.host = input.address.host,
       .port = input.address.port,
       .timeout = std::chrono::milliseconds(input.timeoutMs),
       .cancellation = input.cancellation});
  throwIfCancelled(input.cancellation);
  if (!connection.connected())
    throw std::runtime_error("argus-llm unreachable at " + input.baseUrl);
  return connection;
}

std::chrono::milliseconds rpcTimeout(int timeoutMs)
{
  const auto ceiling = std::chrono::duration_cast<std::chrono::milliseconds>(
      argus::llm::kMaxTimeout);
  if (timeoutMs <= 0)
    return ceiling;
  return std::min(std::chrono::milliseconds(timeoutMs), ceiling);
}

}

LlmRemoteConfig LlmRemoteConfig::resolve()
{
  LlmRemoteConfig config;
  config.url = ConfigService::getString("llm.remote_url");
  if (const int ms = ConfigService::getInt("llm.remote_timeout_ms"); ms > 0)
    config.timeoutMs = ms;
  return config;
}

LlmHttpClient::LlmHttpClient(std::string baseUrl, int timeoutMs)
    : baseUrl_(std::move(baseUrl)), timeoutMs_(timeoutMs)
{
  if (argus::net::parseEndpoint(baseUrl_).host.empty())
    throw std::runtime_error("argus-llm remote_url has no host");
}

LlmHttpClient& LlmHttpClient::withCredential(std::string credential)
{
  credential_ = std::move(credential);
  return *this;
}

std::string LlmHttpClient::chatBody(const ChatRequest& request) const
{
  Json::Value body(Json::objectValue);
  Json::Value messages(Json::arrayValue);
  for (const auto& message : request.messages) {
    Json::Value entry(Json::objectValue);
    entry["role"] = message.role;
    entry["content"] = message.content;
    messages.append(entry);
  }
  body["messages"] = std::move(messages);
  if (request.maxTokens > 0)
    body["max_tokens"] = request.maxTokens;
  if (request.temperature >= 0.0F)
    body["temperature"] = request.temperature;
  if (request.resetContext)
    body["reset_context"] = true;
  if (!request.toolsEnabled)
    body["tools"] = false;
  if (!request.grammar.empty()) {
    body["grammar"] = request.grammar;
    body["grammar_required"] = request.grammarRequired;
  }
  if (request.userId > 0)
    body["user_id"] = Json::Int64{request.userId};
  if (userRoleKnown(request.role))
    body["role"] = userRoleToString(request.role);
  if (!request.lang.empty())
    body["lang"] = request.lang;
  if (!request.sessionId.empty())
    body["session_id"] = request.sessionId;
  if (request.prefillOnly)
    body["prefill_only"] = true;

  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, body);
}

std::string LlmHttpClient::chat(const ChatRequest& request) const
{
  const auto address = argus::net::parseEndpoint(baseUrl_);
  const auto connection = connectTo(
      {.baseUrl = baseUrl_, .address = address, .timeoutMs = timeoutMs_, .cancellation = {}});

  const std::string body = chatBody(request);
  const std::string path = kChatPath;
  const HttpRequestHead head{.method = "POST",
                             .path = &path,
                             .body = &body,
                             .address = &address,
                             .credential = &credential_,
                             .closeConnection = true};
  const std::string outgoing = serialize(head) + body;
  sendRequest({.fd = connection.socket.get(), .outgoing = outgoing, .cancellation = {}});

  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeoutMs_);
  const std::string wire =
      argus::net::readUntilClosed(
          {.fd = connection.socket.get(), .deadline = deadline, .cancellation = {}})
          .data;
  if (wire.empty())
    throw std::runtime_error("argus-llm closed the connection before answering");

  const Head parsed = parseHead(wire);
  if (parsed.status != 200)
    throwEnvelopeError(parsed.status, wire.substr(parsed.bodyStart));
  const std::string bodyOut = wire.substr(parsed.bodyStart, parsed.contentLength);
  Json::Value json;
  Json::Reader reader;
  if (!reader.parse(bodyOut, json) || !json["info"].isObject() ||
      !json["info"].isMember("text"))
    throw std::runtime_error("argus-llm chat response unreadable");
  return json["info"]["text"].asString();
}

void LlmHttpClient::chatStream(const LlmStreamInput& input) const
{
  throwIfCancelled(input.cancellation);
  const auto address = argus::net::parseEndpoint(baseUrl_);
  const auto connection = connectTo({.baseUrl = baseUrl_,
                                     .address = address,
                                     .timeoutMs = timeoutMs_,
                                     .cancellation = input.cancellation});
  const int fd = connection.socket.get();
  const std::stop_callback cancel(input.cancellation,
                                  [&connection] { connection.socket.shutdown(); });
  throwIfCancelled(input.cancellation);

  const std::string body = chatBody(input.request);
  const std::string path = kChatStreamPath;
  const HttpRequestHead head{.method = "POST",
                             .path = &path,
                             .body = &body,
                             .address = &address,
                             .credential = &credential_,
                             .closeConnection = false};
  const std::string outgoing = serialize(head) + body;
  sendRequest({.fd = fd, .outgoing = outgoing, .cancellation = input.cancellation});

  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeoutMs_);

  std::string wire;
  auto recvMore = [&]() -> bool {
    throwIfCancelled(input.cancellation);
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    const auto status = argus::net::receiveSome(
        {.fd = fd, .into = wire, .cancellation = input.cancellation});
    throwIfCancelled(input.cancellation);
    return status == argus::net::NetStatus::Ok;
  };

  while (wire.find("\r\n\r\n") == std::string::npos) {
    if (!recvMore())
      throw std::runtime_error("argus-llm response has no header block");
  }
  const Head parsed = parseHead(wire);
  if (parsed.status != 200) {
    while (wire.size() < parsed.bodyStart + parsed.contentLength) {
      if (!recvMore())
        break;
    }
    throwEnvelopeError(parsed.status, wire.substr(parsed.bodyStart));
  }

  std::string::size_type cursor = parsed.bodyStart;
  bool doneSent = false;
  for (;;) {
    std::string::size_type lineEnd = wire.find("\r\n", cursor);
    while (lineEnd == std::string::npos) {
      if (!recvMore())
        throw std::runtime_error("argus-llm chunked body truncated");
      lineEnd = wire.find("\r\n", cursor);
    }
    const std::string sizeLine = wire.substr(cursor, lineEnd - cursor);
    const auto semicolon = sizeLine.find(';');
    const std::string sizeHex =
        trim(semicolon == std::string::npos
                 ? sizeLine
                 : sizeLine.substr(0, semicolon));
    if (sizeHex.empty())
      throw std::runtime_error("argus-llm malformed chunk size");
    const size_t size =
        static_cast<size_t>(std::stoul(sizeHex, nullptr, 16));
    if (size == 0) {
      if (!doneSent)
        throw std::runtime_error("argus-llm stream ended without a sentinel");
      return;
    }
    const auto dataStart = lineEnd + 2;
    while (wire.size() < dataStart + size + 2) {
      if (!recvMore())
        throw std::runtime_error("argus-llm chunked body truncated");
    }

    const std::string chunk = wire.substr(dataStart, size);
    const auto mark = chunk.find(kStreamSentinelMark);
    if (mark == std::string::npos) {
      if (!chunk.empty())
        input.onToken(chunk, false);
    }
    else {
      std::string candidate = chunk.substr(mark + 1);
      if (!candidate.empty() && candidate.back() == '\n')
        candidate.pop_back();
      if (!parseSentinel(candidate, input.stats))
        throw std::runtime_error("argus-llm malformed stream sentinel");
      if (mark > 0)
        input.onToken(chunk.substr(0, mark), false);
      input.onToken("", true);
      doneSent = true;
    }
    cursor = dataStart + size + 2;
  }
}

LlmClient::LlmClient(std::string baseUrl, int timeoutMs)
    : baseUrl_(std::move(baseUrl)), timeoutMs_(timeoutMs)
{
}

std::shared_ptr<argus::llm::Client> LlmClient::rpcClient() const
{
  const auto target = ConfigService::getString("llm.grpc_target");
  if (target.empty())
    return {};
  const auto credential = ConfigService::getString("llm.grpc_credential");
  auto cached = rpcCache_.load();
  while (!cached || cached->target != target ||
         cached->credential != credential) {
    auto built = std::make_shared<RpcCache>(
        RpcCache{.target = target,
                 .credential = credential,
                 .client = std::make_shared<argus::llm::Client>(
                     argus::llm::ClientConfig{
                         .target = target,
                         .credential = credential,
                         .timeout = rpcTimeout(timeoutMs_)})});
    if (rpcCache_.compare_exchange_weak(cached, built))
      return built->client;
  }
  return cached->client;
}

std::string LlmClient::chat(const ChatRequest& request) const
{
  if (const auto client = rpcClient())
    return client->chat(request);
  return LlmHttpClient(baseUrl_, timeoutMs_)
      .withCredential(ConfigService::getString("llm.grpc_credential"))
      .chat(request);
}

void LlmClient::chatStream(const LlmStreamInput& input) const
{
  if (const auto client = rpcClient()) {
    client->chatStream(input);
    return;
  }
  LlmHttpClient(baseUrl_, timeoutMs_)
      .withCredential(ConfigService::getString("llm.grpc_credential"))
      .chatStream(input);
}

bool LlmClient::remote() const
{
  return !ConfigService::getString("llm.grpc_target").empty() ||
         !baseUrl_.empty();
}
