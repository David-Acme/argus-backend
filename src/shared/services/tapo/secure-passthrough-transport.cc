#include "secure-passthrough-transport.hxx"

#include <drogon/drogon.h>
#include <shared/services/tapo/tapo-crypto.hxx>
#include <shared/services/tapo/tapo-http.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <utility>

namespace
{

constexpr int kSecureModeProbeCode = -40413;
constexpr int kTokenExpiredCode = -40401;
constexpr int kGenericFailureCode = -1;
constexpr size_t kCnonceChars = 8;

TapoEndpoint endpointOf(const TapoCredentials& credentials)
{
  return {.host = credentials.host,
          .port = credentials.port,
          .tls = true,
          .connectTimeoutMs = credentials.connectTimeoutMs,
          .ioTimeoutMs = credentials.requestTimeoutMs};
}

std::vector<TapoHttpHeader> commonHeaders()
{
  return {{"Content-Type", "application/json"},
          {"Accept", "application/json"},
          {"User-Agent", "Tapo CameraClient Android"},
          {"Connection", "close"},
          {"requestByApp", "true"}};
}

int errorCodeOf(const Json::Value& response)
{
  if (response.isMember("error_code"))
    return response["error_code"].asInt();
  return 0;
}

std::string pytapoJson(const Json::Value& value)
{
  if (value.isObject()) {
    std::string out = "{";
    for (auto it = value.begin(); it != value.end(); ++it) {
      if (out.size() > 1)
        out += ", ";
      out += "\"" + it.name() + "\": " + pytapoJson(*it);
    }
    out += "}";
    return out;
  }
  if (value.isArray()) {
    std::string out = "[";
    for (const auto& element : value) {
      if (out.size() > 1)
        out += ", ";
      out += pytapoJson(element);
    }
    out += "]";
    return out;
  }
  if (value.isString()) {
    std::string out = "\"";
    for (const char c : value.asString()) {
      if (c == '"')
        out += "\\\"";
      else if (c == '\\')
        out += "\\\\";
      else
        out += c;
    }
    out += "\"";
    return out;
  }
  if (value.isBool())
    return value.asBool() ? "true" : "false";
  if (value.isNull())
    return "null";
  if (value.isInt64() || value.isUInt64())
    return std::to_string(value.asInt64());
  return json_util::toString(value);
}

} // namespace

SecurePassthroughTransport::SecurePassthroughTransport(TapoCredentials credentials)
    : credentials_(std::move(credentials))
{
}

TapoTransportKind SecurePassthroughTransport::kind() const
{
  return TapoTransportKind::SecurePassthrough;
}

bool SecurePassthroughTransport::isAuthenticated() const
{
  return authenticated_;
}

Json::Value SecurePassthroughTransport::state() const
{
  Json::Value value(Json::objectValue);
  value["transport"] = tapoTransportKindToString(kind());
  value["hashAlgorithm"] = tapoHashAlgorithmToString(hashAlgorithm_);
  value["username"] = credentials_.username;
  value["authenticated"] = authenticated_;
  return value;
}

TapoResult SecurePassthroughTransport::postPlain(const Json::Value& payload,
                                                 Json::Value& out)
{
  TapoHttpRequest request;
  request.endpoint = endpointOf(credentials_);
  request.method = "POST";
  request.path = "/";
  request.headers = commonHeaders();
  request.headers.push_back({"Referer", "https://" + credentials_.host});
  request.body = json_util::toString(payload);

  const auto response = TapoHttp::send(request);
  if (!response.ok)
    return TapoResult::failure(response.error.empty() ? "transport error"
                                                      : response.error);
  out = json_util::fromString(response.body);
  if (out.isNull())
    return TapoResult::failure("malformed response body");
  return TapoResult::success(out);
}

bool SecurePassthroughTransport::fetchHandshake(HandshakeData& handshake,
                                                TapoResult& failure)
{
  cnonce_ = tapo_crypto::randomHex(kCnonceChars);

  Json::Value payload(Json::objectValue);
  payload["method"] = "login";
  payload["params"]["cnonce"] = cnonce_;
  payload["params"]["encrypt_type"] = "3";
  payload["params"]["username"] = credentials_.username;

  Json::Value response;
  const auto sent = postPlain(payload, response);
  if (!sent.ok) {
    failure = sent;
    return false;
  }

  const auto& data = response["result"]["data"];
  if (!data.isMember("nonce") || !data.isMember("device_confirm")) {
    failure = TapoResult::failure("device does not advertise secure mode",
                                  errorCodeOf(response));
    return false;
  }
  if (errorCodeOf(response) != kSecureModeProbeCode)
    LOG_DEBUG << "tapo: unexpected handshake code " << errorCodeOf(response);

  handshake.nonce = data["nonce"].asString();
  handshake.deviceConfirm = data["device_confirm"].asString();
  return true;
}

bool SecurePassthroughTransport::resolveHashAlgorithm(const HandshakeData& handshake)
{
  const std::string sha256Password = tapo_crypto::sha256Hex(credentials_.password);
  const std::string md5Password = tapo_crypto::md5Hex(credentials_.password);

  const std::string sha256Confirm =
      tapo_crypto::sha256Hex(cnonce_ + sha256Password + handshake.nonce) +
      handshake.nonce + cnonce_;
  if (sha256Confirm == handshake.deviceConfirm) {
    hashAlgorithm_ = TapoHashAlgorithm::Sha256;
    hashedPassword_ = sha256Password;
    return true;
  }

  const std::string md5Confirm =
      tapo_crypto::sha256Hex(cnonce_ + md5Password + handshake.nonce) +
      handshake.nonce + cnonce_;
  if (md5Confirm == handshake.deviceConfirm) {
    hashAlgorithm_ = TapoHashAlgorithm::Md5;
    hashedPassword_ = md5Password;
    return true;
  }

  LOG_WARN << "tapo: device_confirm mismatch (sha256=" << sha256Confirm
           << " md5=" << md5Confirm << " cnonce=" << cnonce_
           << " nonce=" << handshake.nonce << ")";
  return false;
}

void SecurePassthroughTransport::deriveKeys(const std::string& nonce)
{
  const std::string hashedKey =
      tapo_crypto::sha256Hex(cnonce_ + hashedPassword_ + nonce);
  const auto lsk = tapo_crypto::sha256Raw("lsk" + cnonce_ + nonce + hashedKey);
  const auto ivb = tapo_crypto::sha256Raw("ivb" + cnonce_ + nonce + hashedKey);
  lsk_.assign(lsk.begin(), lsk.begin() + 16);
  ivb_.assign(ivb.begin(), ivb.begin() + 16);
}

TapoResult SecurePassthroughTransport::login()
{
  authenticated_ = false;

  HandshakeData handshake;
  TapoResult failure;
  if (!fetchHandshake(handshake, failure))
    return failure;

  if (!resolveHashAlgorithm(handshake))
    return TapoResult::failure("device_confirm mismatch: wrong password or user");

  nonce_ = handshake.nonce;
  const std::string digestPassword =
      tapo_crypto::sha256Hex(hashedPassword_ + cnonce_ + nonce_) + cnonce_ + nonce_;

  Json::Value payload(Json::objectValue);
  payload["method"] = "login";
  payload["params"]["cnonce"] = cnonce_;
  payload["params"]["encrypt_type"] = "3";
  payload["params"]["username"] = credentials_.username;
  payload["params"]["digest_passwd"] = digestPassword;

  Json::Value response;
  const auto sent = postPlain(payload, response);
  if (!sent.ok)
    return sent;

  const auto& result = response["result"];
  if (!result.isMember("stok"))
    return TapoResult::failure("login rejected", errorCodeOf(response));

  stok_ = result["stok"].asString();
  seq_ = result.isMember("start_seq") ? result["start_seq"].asInt64() : 0;
  deriveKeys(nonce_);
  authenticated_ = true;

  LOG_INFO << "tapo: secure passthrough session established with "
           << credentials_.host << " (hash="
           << tapoHashAlgorithmToString(hashAlgorithm_) << ")";
  return TapoResult::success(result);
}

TapoResult SecurePassthroughTransport::sendEncrypted(const Json::Value& payload)
{
  const std::string plain = pytapoJson(payload);
  const int64_t seq = seq_++;

  const auto cipher = tapo_crypto::aes128CbcEncrypt(
      {.data = tapo_crypto::toBytes(plain), .key = lsk_, .iv = ivb_});
  if (cipher.empty())
    return TapoResult::failure("cannot encrypt request");

  Json::Value envelope(Json::objectValue);
  envelope["method"] = "securePassthrough";
  envelope["params"]["request"] = tapo_crypto::base64Encode(cipher);
  const std::string envelopePlain = pytapoJson(envelope);

  const std::string tag = tapo_crypto::sha256Hex(
      tapo_crypto::sha256Hex(hashedPassword_ + cnonce_) + envelopePlain +
      std::to_string(seq));

  TapoHttpRequest request;
  request.endpoint = endpointOf(credentials_);
  request.method = "POST";
  request.path = "/stok=" + stok_ + "/ds";
  request.headers = commonHeaders();
  request.headers.push_back({"Referer", "https://" + credentials_.host});
  request.headers.push_back({"Seq", std::to_string(seq)});
  request.headers.push_back({"Tapo_tag", tag});
  request.body = envelopePlain;

  const auto response = TapoHttp::send(request);
  if (!response.ok)
    return TapoResult::failure(response.error.empty() ? "transport error"
                                                      : response.error);

  const Json::Value outer = json_util::fromString(response.body);
  if (outer.isNull())
    return TapoResult::failure("malformed response body");

  const int code = errorCodeOf(outer);
  if (!outer["result"].isMember("response")) {
    LOG_WARN << "tapo: response without payload, outer="
             << json_util::toString(outer) << " seq=" << seq;
    return TapoResult::failure("response without payload", code);
  }

  const auto decoded = tapo_crypto::base64Decode(outer["result"]["response"].asString());
  const auto clear = tapo_crypto::aes128CbcDecrypt(
      {.data = decoded, .key = lsk_, .iv = ivb_});
  if (clear.empty())
    return TapoResult::failure("cannot decrypt response", code);

  const Json::Value inner = json_util::fromString(tapo_crypto::toText(clear));
  if (inner.isNull())
    return TapoResult::failure("malformed decrypted payload", code);

  const int innerCode = errorCodeOf(inner);
  if (innerCode != 0 && innerCode != kSecureModeProbeCode) {
    TapoResult result = TapoResult::failure("device returned error", innerCode);
    result.data = inner;
    return result;
  }
  return TapoResult::success(inner);
}

TapoResult SecurePassthroughTransport::request(const Json::Value& payload)
{
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (!authenticated_) {
      const auto session = login();
      if (!session.ok)
        return session;
    }

    auto result = sendEncrypted(payload);
    const bool expired =
        !result.ok &&
        (result.errorCode == kTokenExpiredCode ||
         result.errorCode == kGenericFailureCode ||
         result.error == "cannot decrypt response");
    if (!expired)
      return result;

    LOG_DEBUG << "tapo: session expired, re-authenticating (attempt "
              << attempt + 1 << ")";
    authenticated_ = false;
  }

  return sendEncrypted(payload);
}
