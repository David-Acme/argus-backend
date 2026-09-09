#include "s3-storage-service.hxx"

#include "s3-signing.hxx"

#include <array>
#include <config/app-config.hxx>
#include <chrono>
#include <ctime>
#include <drogon/HttpClient.h>
#include <drogon/drogon.h>
#include <openssl/rand.h>
#include <shared/exceptions/response-exception.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <stdexcept>

namespace
{
struct S3Config
{
  std::string endpoint;
  std::string hostHeader;
  std::string bucket;
  std::string accessKey;
  std::string secretKey;
  std::string region;
};

std::string timestampUtc()
{
  const auto now = std::time(nullptr);
  std::tm utc{};
  gmtime_r(&now, &utc);
  std::array<char, 17> value{};
  if (std::strftime(value.data(), value.size(), "%Y%m%dT%H%M%SZ", &utc) == 0)
    throw std::runtime_error("Unable to format S3 timestamp");
  return value.data();
}

std::string randomKeyPart()
{
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), bytes.size()) != 1)
    throw std::runtime_error("Unable to create S3 object key");
  return s3_signing::hexLower(bytes.data(), bytes.size());
}

S3Config loadConfig()
{
  if (ConfigService::getString("storage.mode") != "s3")
    throw std::runtime_error("Private object storage is disabled");

  S3Config config{
      .endpoint = ConfigService::getString("storage.s3.endpoint"),
      .hostHeader = "",
      .bucket = ConfigService::getString("storage.s3.bucket"),
      .accessKey = ConfigService::getString("storage.s3.access_key"),
      .secretKey = ConfigService::getString("storage.s3.secret_key"),
      .region = ConfigService::getString("storage.s3.region"),
  };
  if (config.endpoint.empty() || config.bucket.empty() || config.accessKey.empty() ||
      config.secretKey.empty() || config.region.empty()) {
    throw std::runtime_error("Private object storage is not configured");
  }

  const auto schemeEnd = config.endpoint.find("://");
  if (schemeEnd == std::string::npos)
    throw std::runtime_error("Invalid S3 endpoint");
  const auto scheme = config.endpoint.substr(0, schemeEnd);
  if (scheme != "http" && scheme != "https")
    throw std::runtime_error("Invalid S3 endpoint scheme");
  const auto authority = config.endpoint.substr(schemeEnd + 3);
  if (authority.empty() || authority.find('/') != std::string::npos ||
      authority.find('@') != std::string::npos) {
    throw std::runtime_error("Invalid S3 endpoint authority");
  }
  config.hostHeader = authority;
  return config;
}

std::string objectPath(const S3Config& config, const std::string& objectKey)
{
  return "/" + config.bucket + "/" + objectKey;
}

struct SendInput
{
  const S3Config& config;
  const std::string& method;
  const std::string& objectKey;
  const std::string& body;
  const std::string& contentType;
};

drogon::Task<std::string> send(const SendInput& input)
{
  const S3Config& config = input.config;
  const std::string& method = input.method;
  const std::string& objectKey = input.objectKey;
  const std::string& body = input.body;
  const std::string& contentType = input.contentType;

  const auto payloadHash = s3_signing::sha256Hex(body);
  const auto timestamp = timestampUtc();
  const auto path = objectPath(config, objectKey);
  s3_signing::SigV4Input sig{
      .method = method,
      .canonicalUri = path,
      .canonicalQuery = "",
      .headers = {
          {"host", config.hostHeader},
          {"x-amz-content-sha256", payloadHash},
          {"x-amz-date", timestamp},
      },
      .payloadHash = payloadHash,
      .accessKey = config.accessKey,
      .secretKey = config.secretKey,
      .region = config.region,
      .timestamp = timestamp,
  };
  if (!contentType.empty())
    sig.headers.emplace("content-type", contentType);
  const auto signedRequest = s3_signing::sign(sig);

  auto request = drogon::HttpRequest::newHttpRequest();
  request->setMethod(method == "PUT" ? drogon::Put
                     : method == "DELETE" ? drogon::Delete
                                           : drogon::Get);
  request->setPath(path);
  request->setPathEncode(false);
  request->addHeader("Host", config.hostHeader);
  request->addHeader("x-amz-content-sha256", payloadHash);
  request->addHeader("x-amz-date", timestamp);
  request->addHeader("Authorization", signedRequest.authorization);
  if (!contentType.empty())
    request->addHeader("Content-Type", contentType);
  if (method == "PUT")
    request->setBody(body);

  const auto client = drogon::HttpClient::newHttpClient(config.endpoint);
  const auto response = co_await client->sendRequestCoro(request, 10.0);
  const auto status = response->getStatusCode();
  if (status < drogon::k200OK || status >= drogon::k300MultipleChoices) {
    throw std::runtime_error("S3 request failed with HTTP " +
                             std::to_string(static_cast<int>(status)));
  }
  co_return std::string(response->getBody());
}
} // namespace

bool S3StorageService::isConfigured() const
{
  try {
    static_cast<void>(loadConfig());
    return true;
  }
  catch (...) {
    return false;
  }
}

drogon::Task<S3StoredObject>
S3StorageService::putPortrait(int64_t userId, const std::string& image) const
{
  if (userId <= 0 || image.empty())
    throw ResponseException("Invalid portrait upload", 422,
                            AppConfig::ERROR_CODE_BAD_REQUEST);

  const auto config = loadConfig();
  const auto key = "portraits/" + std::to_string(userId) + "/" + randomKeyPart() + ".jpg";
  static_cast<void>(co_await send({.config = config,
                                   .method = "PUT",
                                   .objectKey = key,
                                   .body = image,
                                   .contentType = "image/jpeg"}));
  co_return S3StoredObject{
      .objectKey = key,
      .sha256 = s3_signing::sha256Hex(image),
      .byteSize = static_cast<int64_t>(image.size()),
  };
}

drogon::Task<std::string>
S3StorageService::get(const std::string& objectKey) const
{
  if (objectKey.empty())
    throw ResponseException("Invalid portrait object", 422,
                            AppConfig::ERROR_CODE_BAD_REQUEST);
  co_return co_await send({.config = loadConfig(),
                           .method = "GET",
                           .objectKey = objectKey,
                           .body = "",
                           .contentType = ""});
}

drogon::Task<void> S3StorageService::remove(const std::string& objectKey) const
{
  if (objectKey.empty())
    co_return;
  static_cast<void>(co_await send({.config = loadConfig(),
                                   .method = "DELETE",
                                   .objectKey = objectKey,
                                   .body = "",
                                   .contentType = ""}));
}
