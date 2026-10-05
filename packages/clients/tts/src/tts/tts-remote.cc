#include "tts-remote.hxx"

#include <errors/response-exception.hxx>
#include <tts/tts-client.hxx>
#include <atomic>
#include <algorithm>
#include <tts/tts-errors.hxx>
#include <utility>

#include <config/config-service.hxx>

#include <json/json.h>
#include <net/loopback-socket.hxx>

#include <cctype>
#include <chrono>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>

namespace
{

std::string toLower(std::string value)
{
  for (auto& c : value)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

std::string trim(const std::string& value)
{
  const auto start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos)
    return "";
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
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
  throw std::runtime_error("argus-tts " + detail);
}

std::vector<float> pcmFromBytes(const std::string& bytes)
{
  if (bytes.size() % sizeof(float) != 0)
    throw std::runtime_error("argus-tts PCM body is not float32-aligned");
  std::vector<float> pcm(bytes.size() / sizeof(float));
  std::memcpy(pcm.data(), bytes.data(), bytes.size());
  return pcm;
}

std::string jsonBody(const TtsRequest& req)
{
  Json::Value json(Json::objectValue);
  json["text"] = req.text;
  json["style_id"] = req.voiceId;
  json["lang"] = langCode(req.lang);
  if (req.speed > 0)
    json["speed"] = static_cast<double>(req.speed);
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, json);
}

void checkCancellation(std::stop_token cancellation)
{
  if (cancellation.stop_requested())
    throw ResponseException(499, TtsErrors::Cancelled);
}

std::string requestHead(const WireRequest& request, const argus::net::Endpoint& address)
{
  std::string head = request.method + " " + request.path + " HTTP/1.1\r\n";
  head += "Host: " + address.host + "\r\n";
  if (!request.body.empty()) {
    head += "Content-Type: application/json\r\n";
    head += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
  }
  if (request.closeConnection)
    head += "Connection: close\r\n";
  head += "\r\n";
  return head;
}

struct ExchangeIo
{
  int fd;
  const std::string& outgoing;
  std::stop_token cancellation;
};

void sendRequest(const ExchangeIo& io)
{
  checkCancellation(io.cancellation);
  const auto status = argus::net::sendAll(
      {.fd = io.fd, .data = io.outgoing, .cancellation = io.cancellation});
  checkCancellation(io.cancellation);
  if (status != argus::net::NetStatus::Ok)
    throw std::runtime_error("argus-tts request send failed");
}

struct ConnectToInput
{
  const std::string& baseUrl;
  const argus::net::Endpoint& address;
  int timeoutMs;
  std::stop_token cancellation;
};

argus::net::Connection connectTo(const ConnectToInput& input)
{
  checkCancellation(input.cancellation);
  auto connection = argus::net::connectLoopback(
      {.host = input.address.host,
       .port = input.address.port,
       .timeout = std::chrono::milliseconds(input.timeoutMs),
       .cancellation = input.cancellation});
  checkCancellation(input.cancellation);
  if (!connection.connected())
    throw std::runtime_error("argus-tts unreachable at " + input.baseUrl);
  return connection;
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
  Head head;
  const auto split = wire.find("\r\n\r\n");
  if (split == std::string::npos)
    throw std::runtime_error("argus-tts response has no header block");
  head.bodyStart = split + 4;

  const auto lineEnd = wire.find("\r\n");
  const std::string statusLine = wire.substr(0, lineEnd);
  const auto space1 = statusLine.find(' ');
  const auto space2 = statusLine.find(' ', space1 + 1);
  if (space1 == std::string::npos || space2 == std::string::npos)
    throw std::runtime_error("argus-tts malformed status line");
  head.status = std::stoi(statusLine.substr(space1 + 1, space2 - space1 - 1));

  std::string::size_type cursor = lineEnd + 2;
  while (cursor < split) {
    const auto eol = wire.find("\r\n", cursor);
    if (eol == std::string::npos || eol > split)
      break;
    const auto colon = wire.find(':', cursor);
    if (colon != std::string::npos && colon < eol) {
      const std::string name = toLower(wire.substr(cursor, colon - cursor));
      const std::string value = trim(wire.substr(colon + 1, eol - colon - 1));
      if (name == "transfer-encoding" &&
          toLower(value).find("chunked") != std::string::npos)
        head.chunked = true;
      if (name == "content-length")
        head.contentLength = static_cast<size_t>(std::stoul(value));
    }
    cursor = eol + 2;
  }
  return head;
}

struct ForEachHttpChunkInput
{
  const std::string& wire;
  const Head& head;
  const std::function<void(const char*, size_t)>& onChunk;
};

void forEachHttpChunk(const ForEachHttpChunkInput& input)
{
  const std::string& wire = input.wire;
  const Head& head = input.head;
  const std::function<void(const char*, size_t)>& onChunk = input.onChunk;
  std::string::size_type cursor = head.bodyStart;
  for (;;) {
    const auto lineEnd = wire.find("\r\n", cursor);
    if (lineEnd == std::string::npos)
      throw std::runtime_error("argus-tts chunked body truncated");
    const std::string sizeLine = wire.substr(cursor, lineEnd - cursor);
    const auto semicolon = sizeLine.find(';');
    const std::string sizeHex =
        trim(semicolon == std::string::npos
                 ? sizeLine
                 : sizeLine.substr(0, semicolon));
    if (sizeHex.empty())
      throw std::runtime_error("argus-tts malformed chunk size");
    const size_t size =
        static_cast<size_t>(std::stoul(sizeHex, nullptr, 16));
    if (size == 0)
      return;
    const auto dataStart = lineEnd + 2;
    if (wire.size() < dataStart + size + 2)
      throw std::runtime_error("argus-tts chunked body truncated");
    onChunk(wire.data() + dataStart, size);
    cursor = dataStart + size + 2;
  }
}

}

TtsRemoteConfig TtsRemoteConfig::resolve()
{
  TtsRemoteConfig config;
  config.url = ConfigService::getString("tts.remote_url");
  if (const int ms = ConfigService::getInt("tts.remote_timeout_ms"); ms > 0)
    config.timeoutMs = ms;
  return config;
}

TtsHttpClient::TtsHttpClient(std::string baseUrl, int timeoutMs)
    : baseUrl_(std::move(baseUrl)), timeoutMs_(timeoutMs)
{
  if (argus::net::parseEndpoint(baseUrl_).host.empty())
    throw std::runtime_error("argus-tts remote_url has no host");
}

TtsHttpClient::RawResponse TtsHttpClient::exchange(
    const WireRequest& request, std::stop_token cancellation) const
{
  const auto address = argus::net::parseEndpoint(baseUrl_);
  const auto connection = connectTo({.baseUrl = baseUrl_,
                                     .address = address,
                                     .timeoutMs = timeoutMs_,
                                     .cancellation = cancellation});
  const int fd = connection.socket.get();
  const std::stop_callback cancel(cancellation, [&connection] { connection.socket.shutdown(); });

  const std::string outgoing = requestHead(request, address) + request.body;
  sendRequest({.fd = fd, .outgoing = outgoing, .cancellation = cancellation});

  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeoutMs_);
  const std::string wire =
      argus::net::readUntilClosed({.fd = fd, .deadline = deadline, .cancellation = cancellation})
          .data;
  checkCancellation(cancellation);
  if (wire.empty())
    throw std::runtime_error("argus-tts closed the connection before answering");

  const Head parsed = parseHead(wire);
  if (parsed.status != 200)
    throwEnvelopeError(parsed.status, wire.substr(parsed.bodyStart));
  if (parsed.chunked) {
    std::string body;
    forEachHttpChunk({.wire = wire,
                      .head = parsed,
                      .onChunk = [&body](const char* data, size_t size) {
                        body.append(data, size);
                      }});
    return {.status = parsed.status, .body = std::move(body)};
  }
  return {.status = parsed.status,
          .body = wire.substr(parsed.bodyStart, parsed.contentLength)};
}

void TtsHttpClient::stream(
    const WireRequest& request, const TtsHttpStreamInput& input) const
{
  const auto& cancellation = input.cancellation;
  const auto& onChunk = input.onChunk;
  checkCancellation(cancellation);
  const auto address = argus::net::parseEndpoint(baseUrl_);
  const auto connection = connectTo({.baseUrl = baseUrl_,
                                     .address = address,
                                     .timeoutMs = timeoutMs_,
                                     .cancellation = cancellation});
  const int fd = connection.socket.get();
  const std::stop_callback cancel(cancellation, [&connection] { connection.socket.shutdown(); });

  const std::string outgoing = requestHead(request, address) + request.body;
  sendRequest({.fd = fd, .outgoing = outgoing, .cancellation = cancellation});

  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeoutMs_);

  std::string wire;
  auto recvMore = [&]() -> bool {
    checkCancellation(cancellation);
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    const auto status = argus::net::receiveSome(
        {.fd = fd, .into = wire, .cancellation = cancellation});
    checkCancellation(cancellation);
    return status == argus::net::NetStatus::Ok;
  };

  while (wire.find("\r\n\r\n") == std::string::npos) {
    if (!recvMore())
      throw std::runtime_error("argus-tts response has no header block");
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
  for (;;) {
    checkCancellation(cancellation);
    std::string::size_type lineEnd = wire.find("\r\n", cursor);
    while (lineEnd == std::string::npos) {
      if (!recvMore())
        throw std::runtime_error("argus-tts chunked body truncated");
      lineEnd = wire.find("\r\n", cursor);
    }
    const std::string sizeLine = wire.substr(cursor, lineEnd - cursor);
    const auto semicolon = sizeLine.find(';');
    const std::string sizeHex =
        trim(semicolon == std::string::npos
                 ? sizeLine
                 : sizeLine.substr(0, semicolon));
    if (sizeHex.empty())
      throw std::runtime_error("argus-tts malformed chunk size");
    const size_t size =
        static_cast<size_t>(std::stoul(sizeHex, nullptr, 16));
    if (size == 0)
      return;
    const auto dataStart = lineEnd + 2;
    while (wire.size() < dataStart + size + 2) {
      if (!recvMore())
        throw std::runtime_error("argus-tts chunked body truncated");
    }
    checkCancellation(cancellation);
    onChunk(wire.data() + dataStart, size);
    checkCancellation(cancellation);
    cursor = dataStart + size + 2;
  }
}

float TtsHttpClient::defaultSpeed(std::stop_token cancellation) const
{
  const RawResponse response = exchange(
      {.method = "GET", .path = "/tts/v1/config", .body = "",
       .closeConnection = true},
      cancellation);
  Json::Value json;
  Json::Reader reader;
  if (!reader.parse(response.body, json) || !json["info"].isObject() ||
      !json["info"].isMember("defaultSpeed"))
    throw std::runtime_error("argus-tts config response unreadable");
  return json["info"]["defaultSpeed"].asFloat();
}

int TtsHttpClient::sampleRate(std::stop_token cancellation) const
{
  const RawResponse response = exchange(
      {.method = "GET", .path = "/tts/v1/config", .body = "",
       .closeConnection = true},
      cancellation);
  Json::Value json;
  Json::Reader reader;
  if (!reader.parse(response.body, json) || !json["info"].isObject() ||
      !json["info"].isMember("sampleRate"))
    throw std::runtime_error("argus-tts config response unreadable");
  return json["info"]["sampleRate"].asInt();
}

std::vector<float> TtsHttpClient::synthesize(const TtsRequest& req,
                                             std::stop_token cancellation) const
{
  checkCancellation(cancellation);
  const RawResponse response =
      exchange({.method = "POST",
                .path = "/tts/v1/synthesize",
                .body = jsonBody(req),
                .closeConnection = true},
               cancellation);
  checkCancellation(cancellation);
  return pcmFromBytes(response.body);
}

void TtsHttpClient::synthesizeStream(const TtsRequest& req,
                                     TtsChunkCallback onChunk) const
{
  synthesizeStream({.request = req, .onChunk = std::move(onChunk), .cancellation = {}});
}

void TtsHttpClient::synthesizeStream(const TtsRequest& req,
                                     TtsChunkCallback onChunk,
                                     std::stop_token cancellation) const
{
  synthesizeStream({.request = req, .onChunk = std::move(onChunk), .cancellation = cancellation});
}

void TtsHttpClient::synthesizeStream(TtsRemoteStreamInput input) const
{
  const auto& req = input.request;
  const auto& onChunk = input.onChunk;
  const auto& cancellation = input.cancellation;
  checkCancellation(cancellation);
  std::vector<float> pending;
  stream({.method = "POST",
          .path = "/tts/v1/synthesize-stream",
          .body = jsonBody(req),
          .closeConnection = false},
         {.onChunk = [&pending, &onChunk, &cancellation](const char* data, size_t size) {
           checkCancellation(cancellation);
           if (size % sizeof(float) != 0)
             throw std::runtime_error(
                 "argus-tts stream chunk is not float32-aligned");
           pending.resize(size / sizeof(float));
           std::memcpy(pending.data(), data, size);
           checkCancellation(cancellation);
           onChunk(pending);
         },
          .cancellation = cancellation});
}

std::shared_ptr<argus::tts::Client> TtsClient::rpcClient() const
{
  const auto target = ConfigService::getString("tts.grpc_target");
  if (target.empty())
    return {};
  auto client = rpcClient_.load();
  if (!client) {
    const auto timeout = TtsRemoteConfig::resolve().timeoutMs;
    client = std::make_shared<argus::tts::Client>(argus::tts::ClientConfig{
        .target = target,
        .credential = ConfigService::getString("tts.grpc_credential"),
        .timeout = std::chrono::milliseconds(timeout)});
    std::shared_ptr<argus::tts::Client> empty;
    if (!rpcClient_.compare_exchange_strong(empty, client))
      client = std::move(empty);
  }
  return client;
}

float TtsClient::defaultSpeed(std::stop_token cancellation) const
{
  checkCancellation(cancellation);
  if (const auto client = rpcClient())
    return client->capabilities(cancellation).defaultSpeed;
  const auto config = TtsRemoteConfig::resolve();
  if (!config.enabled())
    throw std::runtime_error("tts.remote_url is not configured");
  return TtsHttpClient(config.url, config.timeoutMs).defaultSpeed(cancellation);
}

int TtsClient::sampleRate(std::stop_token cancellation) const
{
  checkCancellation(cancellation);
  if (const auto client = rpcClient())
    return client->capabilities(cancellation).sampleRate;
  const auto config = TtsRemoteConfig::resolve();
  if (!config.enabled())
    throw std::runtime_error("tts.remote_url is not configured");
  return TtsHttpClient(config.url, config.timeoutMs).sampleRate(cancellation);
}

std::vector<float> TtsClient::synthesize(const TtsRequest& req,
                                           std::stop_token cancellation) const
{
  checkCancellation(cancellation);
  if (!ConfigService::getString("tts.grpc_target").empty()) {
    std::vector<float> samples;
    synthesizeStream(req, [&samples](const std::vector<float>& chunk) {
      samples.insert(samples.end(), chunk.begin(), chunk.end());
    }, cancellation);
    checkCancellation(cancellation);
    return samples;
  }
  const auto config = TtsRemoteConfig::resolve();
  if (!config.enabled())
    throw std::runtime_error("tts.remote_url is not configured");
  return TtsHttpClient(config.url, config.timeoutMs).synthesize(req, cancellation);
}

void TtsClient::synthesizeStream(const TtsRequest& req,
                                 TtsChunkCallback onChunk) const
{
  synthesizeStream({.request = req, .onChunk = std::move(onChunk), .cancellation = {}});
}

void TtsClient::synthesizeStream(const TtsRequest& req,
                                 TtsChunkCallback onChunk,
                                 std::stop_token cancellation) const
{
  synthesizeStream({.request = req, .onChunk = std::move(onChunk), .cancellation = cancellation});
}

void TtsClient::synthesizeStream(TtsRemoteStreamInput input) const
{
  const auto& req = input.request;
  auto& onChunk = input.onChunk;
  const auto& cancellation = input.cancellation;
  checkCancellation(cancellation);
  if (const auto client = rpcClient()) {
    client->synthesize({.text = req.text,
                        .voice = req.voiceId,
                        .language = langCode(req.lang),
                        .speed = req.speed,
                        .quality = static_cast<argus::tts::Quality>(req.quality),
                        .cancellation = cancellation,
                        .onChunk = [callback = std::move(onChunk)](argus::tts::AudioChunk chunk) {
                          callback(chunk.samples);
                          return true;
                        }});
    return;
  }
  const auto config = TtsRemoteConfig::resolve();
  if (!config.enabled())
    throw std::runtime_error("tts.remote_url is not configured");
  TtsHttpClient(config.url, config.timeoutMs)
      .synthesizeStream(std::move(input));
}

bool TtsClient::remote() const
{
  return !ConfigService::getString("tts.grpc_target").empty() ||
         TtsRemoteConfig::resolve().enabled();
}
