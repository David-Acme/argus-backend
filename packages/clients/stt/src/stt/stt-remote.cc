#include "stt-remote.hxx"

#include <config/config-service.hxx>
#include <stt/stt-client.hxx>

#include <json/json.h>
#include <net/loopback-socket.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace
{
constexpr const char* kPcmMime = "audio/x-argus-pcm-s16";

int16_t sampleFromFloat(float value)
{
  const float clamped = std::max(-1.0F, std::min(1.0F, value));
  const long scaled = std::lround(clamped * kPcmScale);
  return static_cast<int16_t>(std::clamp(scaled, -32768L, 32767L));
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
  throw std::runtime_error("argus-stt " + detail);
}

std::string pcmBytes(const std::vector<float>& audioSamples)
{
  std::string bytes;
  bytes.reserve(audioSamples.size() * sizeof(int16_t));
  for (const float sample : audioSamples) {
    const int16_t raw = sampleFromFloat(sample);
    bytes.append(reinterpret_cast<const char*>(&raw), sizeof(raw));
  }
  return bytes;
}

std::string requestHead(const SttWireRequest& request, const argus::net::Endpoint& address)
{
  std::string head = "POST " + request.path + " HTTP/1.1\r\n";
  head += "Host: " + address.host + "\r\n";
  head += "Content-Type: " + request.contentType + "\r\n";
  head += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
  head += "Connection: close\r\n\r\n";
  return head;
}

struct Head
{
  int status{0};
  std::string::size_type bodyStart{0};
};

Head parseHead(const std::string& wire)
{
  Head head;
  const auto split = wire.find("\r\n\r\n");
  if (split == std::string::npos)
    throw std::runtime_error("argus-stt response has no header block");
  head.bodyStart = split + 4;

  const auto lineEnd = wire.find("\r\n");
  const std::string statusLine = wire.substr(0, lineEnd);
  const auto space1 = statusLine.find(' ');
  const auto space2 = statusLine.find(' ', space1 + 1);
  if (space1 == std::string::npos || space2 == std::string::npos)
    throw std::runtime_error("argus-stt malformed status line");
  head.status = std::stoi(statusLine.substr(space1 + 1, space2 - space1 - 1));
  return head;
}

}

SttRemoteConfig SttRemoteConfig::resolve()
{
  SttRemoteConfig config;
  config.url = ConfigService::getString("stt.remote_url");
  if (const int ms = ConfigService::getInt("stt.remote_timeout_ms"); ms > 0)
    config.timeoutMs = ms;
  return config;
}

SttHttpClient::SttHttpClient(std::string baseUrl, int timeoutMs)
    : baseUrl_(std::move(baseUrl)), timeoutMs_(timeoutMs)
{
  if (argus::net::parseEndpoint(baseUrl_).host.empty())
    throw std::runtime_error("argus-stt remote_url has no host");
}

SttHttpClient::RawResponse SttHttpClient::exchange(
    const SttWireRequest& request) const
{
  const auto address = argus::net::parseEndpoint(baseUrl_);
  const auto connection = argus::net::connectLoopback(
      {.host = address.host,
       .port = address.port,
       .timeout = std::chrono::milliseconds(timeoutMs_),
       .cancellation = {}});
  if (!connection.connected())
    throw std::runtime_error("argus-stt unreachable at " + baseUrl_);

  const std::string outgoing = requestHead(request, address) + request.body;
  if (argus::net::sendAll({.fd = connection.socket.get(), .data = outgoing, .cancellation = {}}) !=
      argus::net::NetStatus::Ok)
    throw std::runtime_error("argus-stt request send failed");

  const std::string wire =
      argus::net::readUntilClosed({.fd = connection.socket.get(),
                                   .deadline = std::chrono::steady_clock::now() +
                                               std::chrono::milliseconds(timeoutMs_),
                                   .cancellation = {}})
          .data;
  if (wire.empty())
    throw std::runtime_error("argus-stt closed the connection before answering");

  const Head parsed = parseHead(wire);
  if (parsed.status != 200)
    throwEnvelopeError(parsed.status, wire.substr(parsed.bodyStart));
  return {.status = parsed.status, .body = wire.substr(parsed.bodyStart)};
}

std::string SttHttpClient::transcribe(const std::vector<float>& audioSamples,
                                      const std::string& lang) const
{
  if (audioSamples.empty())
    throw std::runtime_error("argus-stt transcribe needs a non-empty body");

  SttWireRequest request;
  request.path = "/stt/v1/transcribe?lang=" + lang;
  request.body = pcmBytes(audioSamples);
  request.contentType = kPcmMime;

  const RawResponse response = exchange(request);
  Json::Value json;
  Json::Reader reader;
  if (!reader.parse(response.body, json) || !json["info"].isObject() ||
      !json["info"].isMember("text"))
    throw std::runtime_error("argus-stt transcribe response unreadable");
  return json["info"]["text"].asString();
}

std::shared_ptr<argus::stt::Client> SttClient::rpcClient() const
{
  const auto target = ConfigService::getString("stt.grpc_target");
  if (target.empty())
    return {};
  auto cached = rpcCache_.load();
  while (!cached || cached->target != target) {
    auto built = std::make_shared<RpcCache>(RpcCache{
        .target = target,
        .client = std::make_shared<argus::stt::Client>(argus::stt::ClientConfig{
            .target = target,
            .credential = ConfigService::getString("stt.grpc_credential"),
            .timeout = std::chrono::milliseconds(
                SttRemoteConfig::resolve().timeoutMs)})});
    if (rpcCache_.compare_exchange_weak(cached, built))
      return built->client;
  }
  return cached->client;
}

std::string SttClient::transcribe(const std::vector<float>& audioSamples,
                                  const std::string& lang) const
{
  if (const auto client = rpcClient()) {
    argus::stt::TranscribeInput input;
    input.samples = audioSamples;
    input.sampleRate = kWireSampleRate;
    input.language = lang;
    return client->transcribe(input);
  }
  const auto config = SttRemoteConfig::resolve();
  if (!config.enabled())
    throw std::runtime_error("stt.remote_url is not configured");
  return SttHttpClient(config.url, config.timeoutMs).transcribe(audioSamples, lang);
}

std::unique_ptr<argus::stt::TranscribeStream>
SttClient::openStream(argus::stt::StreamInput input) const
{
  if (const auto client = rpcClient())
    return client->openStream(std::move(input));
  return nullptr;
}

bool SttClient::remote() const
{
  return !ConfigService::getString("stt.grpc_target").empty() ||
         SttRemoteConfig::resolve().enabled();
}
