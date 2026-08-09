#include "tapo-talk-client.hxx"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <drogon/drogon.h>
#include <shared/services/tapo/tapo-audio.hxx>
#include <shared/services/tapo/tapo-crypto.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <thread>
#include <utility>

namespace
{

const std::string kBoundary = "--client-stream-boundary--";
constexpr int kTargetSampleRate = 8000;
constexpr int64_t kClockRate = 90000;
constexpr int kTablesEveryMs = 500;

std::string lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return value;
}

std::string between(const std::string& text, const std::string& open,
                    const std::string& close)
{
  const size_t begin = text.find(open);
  if (begin == std::string::npos)
    return {};
  const size_t start = begin + open.size();
  const size_t end = text.find(close, start);
  if (end == std::string::npos)
    return {};
  return text.substr(start, end - start);
}

struct Biquad
{
  double b0{1}, b1{0}, b2{0}, a1{0}, a2{0};
  double z1{0}, z2{0};

  void apply(std::vector<double>& samples)
  {
    for (auto& s : samples) {
      const double out = b0 * s + z1;
      z1 = b1 * s - a1 * out + z2;
      z2 = b2 * s - a2 * out;
      s = out;
    }
  }
};

std::vector<int16_t> equalizeForSpeaker(const std::vector<int16_t>& samples)
{
  constexpr double kFs = 8000.0;
  std::vector<double> buf;
  buf.reserve(samples.size());
  for (const auto s : samples)
    buf.push_back(s);

  Biquad highPass;
  {
    const double w0 = 2.0 * M_PI * 150.0 / kFs;
    const double alpha = std::sin(w0) / (2.0 * 0.707);
    const double a0 = 1.0 + alpha;
    highPass.b0 = (1.0 + std::cos(w0)) / 2.0 / a0;
    highPass.b1 = -(1.0 + std::cos(w0)) / a0;
    highPass.b2 = highPass.b0;
    highPass.a1 = -2.0 * std::cos(w0) / a0;
    highPass.a2 = (1.0 - alpha) / a0;
  }
  highPass.apply(buf);

  Biquad presence;
  {
    const double w0 = 2.0 * M_PI * 2500.0 / kFs;
    const double alpha = std::sin(w0) / (2.0 * 1.0);
    const double amp = std::pow(10.0, 4.0 / 40.0);
    const double a0 = 1.0 + alpha / amp;
    presence.b0 = (1.0 + alpha * amp) / a0;
    presence.b1 = (-2.0 * std::cos(w0)) / a0;
    presence.b2 = (1.0 - alpha * amp) / a0;
    presence.a1 = (-2.0 * std::cos(w0)) / a0;
    presence.a2 = (1.0 - alpha / amp) / a0;
  }
  presence.apply(buf);

  std::vector<int16_t> out;
  out.reserve(buf.size());
  for (const auto v : buf)
    out.push_back(static_cast<int16_t>(std::clamp(v, -32768.0, 32767.0)));
  return out;
}

} // namespace

std::string TapoTalkPart::header(const std::string& name) const
{
  const std::string needle = lower(name);
  for (const auto& entry : headers) {
    if (lower(entry.name) == needle)
      return entry.value;
  }
  return {};
}

TapoTalkClient::TapoTalkClient(TapoTalkConfig config)
    : config_(std::move(config)), muxer_(config_.ts)
{
}

TapoTalkClient::~TapoTalkClient()
{
  close();
}

bool TapoTalkClient::isOpen() const
{
  return open_;
}

Json::Value TapoTalkClient::state() const
{
  Json::Value value(Json::objectValue);
  value["host"] = config_.host;
  value["port"] = config_.port;
  value["framing"] = tapoTalkFramingToString(config_.framing);
  value["passwordVariant"] = passwordVariant_;
  value["sessionId"] = sessionId_;
  value["keyExchangeNonce"] = keyExchangeNonce_;
  value["mode"] = config_.mode;
  value["open"] = open_;
  return value;
}

std::string TapoTalkClient::requestHead(const std::string& authorization) const
{
  std::string head = "POST /stream HTTP/1.1\r\n";
  head += "Host: " + config_.host + ":" + std::to_string(config_.port) + "\r\n";
  head += "Content-Type: multipart/mixed;boundary=" + kBoundary + "\r\n";
  head += "Connection: keep-alive\r\n";
  head += "User-Agent: Tapo CameraClient Android\r\n";
  if (!authorization.empty())
    head += "Authorization: " + authorization + "\r\n";
  switch (config_.framing) {
    case TapoTalkFraming::NegativeLength:
      head += "Content-Length: -1\r\n";
      break;
    case TapoTalkFraming::Chunked:
      head += "Transfer-Encoding: chunked\r\n";
      break;
    case TapoTalkFraming::None:
      break;
  }
  head += "\r\n";
  return head;
}

TapoResult TapoTalkClient::handshake(const std::string& authorization,
                                     TapoHttpResponse& response)
{
  connection_ = std::make_unique<TapoConnection>();
  const TapoEndpoint endpoint{.host = config_.host,
                              .port = config_.port,
                              .tls = false,
                              .connectTimeoutMs = config_.connectTimeoutMs,
                              .ioTimeoutMs = config_.ioTimeoutMs};
  if (!connection_->open(endpoint))
    return TapoResult::failure(connection_->error());
  if (!connection_->write(requestHead(authorization)))
    return TapoResult::failure(connection_->error());
  response = TapoHttpResponse{};
  if (!TapoHttp::readResponseHead(*connection_, response))
    return TapoResult::failure(response.error.empty()
                                   ? "no response from talk channel"
                                   : response.error);
  return TapoResult::success(Json::Value());
}

TapoResult TapoTalkClient::authenticate()
{
  TapoHttpResponse probe;
  const auto opened = handshake({}, probe);
  if (!opened.ok)
    return opened;

  if (probe.status == 200) {
    passwordVariant_ = "none";
    keyExchangeNonce_ = between(probe.header("Key-Exchange"), "nonce=\"", "\"");
    return TapoResult::success(Json::Value());
  }
  if (probe.status != 401)
    return TapoResult::failure("unexpected talk status " +
                               std::to_string(probe.status));

  const auto challenge =
      tapo_crypto::parseDigestChallenge(probe.header("WWW-Authenticate"));
  if (challenge.nonce.empty())
    return TapoResult::failure("talk channel did not send a digest challenge");

  std::vector<std::pair<std::string, std::string>> candidates;
  if (challenge.encryptType == "3") {
    candidates.push_back(
        {"sha256", tapo_crypto::sha256Hex(config_.cloudPassword)});
    candidates.push_back({"plain", config_.cloudPassword});
    candidates.push_back({"md5", tapo_crypto::md5Hex(config_.cloudPassword)});
  }
  else {
    candidates.push_back({"plain", config_.cloudPassword});
    candidates.push_back({"md5", tapo_crypto::md5Hex(config_.cloudPassword)});
    candidates.push_back(
        {"sha256", tapo_crypto::sha256Hex(config_.cloudPassword)});
  }

  std::string lastError = "digest authentication rejected";
  for (const auto& [variant, password] : candidates) {
    connection_.reset();
    const tapo_crypto::DigestInput input{.username = config_.username,
                                         .password = password,
                                         .realm = challenge.realm,
                                         .nonce = challenge.nonce,
                                         .qop = challenge.qop,
                                         .opaque = challenge.opaque,
                                         .algorithm = challenge.algorithm,
                                         .method = "POST",
                                         .uri = "/stream",
                                         .cnonce = tapo_crypto::randomHex(16),
                                         .nonceCount = 1};
    TapoHttpResponse response;
    const auto attempt =
        handshake(tapo_crypto::buildDigestHeader(input), response);
    if (!attempt.ok) {
      lastError = attempt.error;
      continue;
    }
    if (response.status == 200) {
      passwordVariant_ = variant;
      keyExchangeNonce_ =
          between(response.header("Key-Exchange"), "nonce=\"", "\"");
      LOG_INFO << "tapo talk: authenticated with password variant '" << variant
               << "'";
      return TapoResult::success(Json::Value());
    }
    lastError = "digest rejected (status " + std::to_string(response.status) +
                ", variant " + variant + ")";
  }
  connection_.reset();
  return TapoResult::failure(lastError);
}

bool TapoTalkClient::writePart(const std::vector<TapoHttpHeader>& headers,
                               const std::string& body)
{
  if (!connection_)
    return false;
  std::string part = "--" + kBoundary + "\r\n";
  for (const auto& entry : headers)
    part += entry.name + ": " + entry.value + "\r\n";
  part += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  part += body + "\r\n";

  if (config_.framing != TapoTalkFraming::Chunked)
    return connection_->write(part);

  char length[32];
  std::snprintf(length, sizeof(length), "%zx\r\n", part.size());
  return connection_->write(length) && connection_->write(part) &&
         connection_->write("\r\n");
}

bool TapoTalkClient::readPart(TapoTalkPart& part)
{
  if (!connection_)
    return false;
  part = TapoTalkPart{};

  std::string line;
  for (;;) {
    if (!connection_->readLine(line))
      return false;
    if (line.rfind("--", 0) == 0)
      break;
  }
  for (;;) {
    if (!connection_->readLine(line))
      return false;
    if (line.empty())
      break;
    const size_t colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    std::string value = line.substr(colon + 1);
    const size_t begin = value.find_first_not_of(" \t");
    part.headers.push_back({line.substr(0, colon), begin == std::string::npos
                                                       ? ""
                                                       : value.substr(begin)});
  }

  const std::string length = part.header("Content-Length");
  if (length.empty())
    return true;
  const size_t size = std::strtoul(length.c_str(), nullptr, 10);
  if (size == 0)
    return true;
  return connection_->readExactly(size, part.body);
}

TapoResult TapoTalkClient::startSession()
{
  Json::Value payload(Json::objectValue);
  payload["params"]["talk"]["mode"] = config_.mode;
  payload["params"]["method"] = "get";
  payload["seq"] = static_cast<Json::Int64>(seq_++);
  payload["type"] = "request";

  const std::string body = json_util::toString(payload);
  const std::vector<TapoHttpHeader> headers = {{"Content-Type",
                                                "application/json"},
                                               {"X-If-Encrypt", "0"}};
  if (!writePart(headers, body))
    return TapoResult::failure("cannot send talk session request");

  TapoTalkPart part;
  if (!readPart(part))
    return TapoResult::failure("no answer to talk session request");

  sessionId_ = part.header("X-Session-Id");
  const Json::Value answer = json_util::fromString(part.body);
  if (sessionId_.empty() && answer.isObject()) {
    const auto& params = answer["params"];
    const auto& talk = params["talk"];
    const auto extract = [](const Json::Value& node) -> std::string {
      if (node.isString())
        return node.asString();
      if (node.isInt64())
        return std::to_string(node.asInt64());
      return {};
    };
    if (talk.isMember("session_id"))
      sessionId_ = extract(talk["session_id"]);
    else if (params.isMember("session_id"))
      sessionId_ = extract(params["session_id"]);
  }
  if (sessionId_.empty())
    return TapoResult::failure("talk session id not returned: " + part.body);

  LOG_INFO << "tapo talk: session " << sessionId_ << " opened in mode "
           << config_.mode;
  return TapoResult::success(answer);
}

TapoResult TapoTalkClient::open()
{
  close();
  const auto authenticated = authenticate();
  if (!authenticated.ok)
    return authenticated;

  const auto session = startSession();
  if (!session.ok) {
    connection_.reset();
    return session;
  }

  muxer_.reset();
  pts90k_ = 0;
  sentSamples_ = 0;
  open_ = true;
  return session;
}

TapoResult TapoTalkClient::send(const TapoTalkAudio& audio,
                                const CancellationToken& token)
{
  if (!open_ || !connection_)
    return TapoResult::failure("talk channel not open");
  if (audio.samples.empty())
    return TapoResult::success(Json::Value());

  const auto mono = tapo_audio::resample({.samples = audio.samples,
                                          .sourceRate = audio.sampleRate,
                                          .targetRate = kTargetSampleRate});
  const auto equalized = equalizeForSpeaker(mono);
  int16_t peak = 1;
  for (const auto sample : equalized)
    if (std::abs(sample) > peak)
      peak = std::abs(sample);
  const double gain = std::min(3.0, 26000.0 / peak);
  std::vector<int16_t> gained;
  gained.reserve(equalized.size());
  for (const auto sample : equalized)
    gained.push_back(static_cast<int16_t>(
        std::clamp(static_cast<double>(sample) * gain, -32768.0, 32767.0)));
  const auto encoded = tapo_audio::encodeALaw(gained);
  const size_t packetBytes =
      static_cast<size_t>(kTargetSampleRate * config_.packetMs / 1000);
  if (packetBytes == 0)
    return TapoResult::failure("invalid packet size");

  const int64_t ptsStep = kClockRate * config_.packetMs / 1000;
  auto deadline = std::chrono::steady_clock::now();
  int sinceTables = kTablesEveryMs;
  size_t sent = 0;

  for (size_t offset = 0; offset < encoded.size(); offset += packetBytes) {
    if (token.cancelled()) {
      LOG_DEBUG << "tapo talk: cancelled after " << sent << " packets";
      break;
    }

    const size_t take = std::min(packetBytes, encoded.size() - offset);
    std::string payload;
    if (sinceTables >= kTablesEveryMs) {
      payload += muxer_.tables();
      sinceTables = 0;
    }
    payload += muxer_.frame(
        {.payload =
             std::vector<uint8_t>(encoded.begin() + static_cast<long>(offset),
                                  encoded.begin() +
                                      static_cast<long>(offset + take)),
         .pts90k = pts90k_});
    pts90k_ += ptsStep;
    sinceTables += config_.packetMs;

    const std::vector<TapoHttpHeader> headers = {{"Content-Type", "audio/mp2t"},
                                                 {"X-If-Encrypt", "0"},
                                                 {"X-Session-Id", sessionId_}};
    if (!writePart(headers, payload)) {
      open_ = false;
      return TapoResult::failure("talk channel write failed");
    }
    ++sent;
    sentSamples_ += static_cast<int64_t>(take);

    if (config_.pace) {
      deadline += std::chrono::milliseconds(config_.packetMs);
      std::this_thread::sleep_until(deadline);
    }
  }

  Json::Value result(Json::objectValue);
  result["packets"] = static_cast<Json::Int64>(sent);
  result["cancelled"] = token.cancelled();
  return TapoResult::success(result);
}

TapoResult TapoTalkClient::sendChunk(const TapoTalkSendInput& input,
                                     const CancellationToken& token)
{
  if (!open_) {
    const auto opened = open();
    if (!opened.ok)
      return opened;
  }

  auto result =
      send({.samples = input.samples, .sampleRate = input.sampleRate}, token);
  if (result.ok || !input.reopenOnFailure)
    return result;

  close();
  const auto reopened = open();
  if (!reopened.ok)
    return reopened;
  return send({.samples = input.samples, .sampleRate = input.sampleRate},
              token);
}

int64_t TapoTalkClient::sentDurationMs() const
{
  return sentSamples_ * 1000 / kTargetSampleRate;
}

void TapoTalkClient::close()
{
  if (connection_)
    connection_.reset();
  open_ = false;
  sessionId_.clear();
}
