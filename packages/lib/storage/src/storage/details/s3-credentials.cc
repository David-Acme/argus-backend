#include "s3-credentials.hxx"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <json/json.h>
#include <trantor/utils/Logger.h>
#include <utility>

namespace
{

enum class FileState : std::uint8_t
{
  Unknown,
  Usable,
  Unusable,
};

std::atomic<FileState> gFileState{FileState::Unknown};

void noteFileState(const std::string& path, FileState state)
{
  if (gFileState.exchange(state) == state)
    return;
  if (state == FileState::Usable)
    LOG_INFO << "S3 keys come from the credentials file " << path;
  else
    LOG_WARN << "S3 credentials file " << path
             << " is missing, unreadable or malformed; the configured keys are used";
}

}

namespace s3_credentials
{

std::optional<Credentials> fromFile(const std::string& path)
{
  std::ifstream input(path);
  if (!input)
    return std::nullopt;
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  if (!Json::parseFromStream(builder, input, &root, &errors) ||
      !root.isObject())
    return std::nullopt;
  const Json::Value& keys = std::as_const(root)["credentials"];
  if (!keys.isObject() || !keys["accessKey"].isString() ||
      !keys["secretKey"].isString())
    return std::nullopt;
  Credentials credentials{.accessKey = keys["accessKey"].asString(),
                          .secretKey = keys["secretKey"].asString()};
  if (credentials.accessKey.empty() || credentials.secretKey.empty())
    return std::nullopt;
  return credentials;
}

Credentials resolve(const Source& source)
{
  if (source.credentialsFile.empty())
    return source.fallback;
  auto credentials = fromFile(source.credentialsFile);
  noteFileState(source.credentialsFile,
                credentials ? FileState::Usable : FileState::Unusable);
  if (credentials)
    return *std::move(credentials);
  return source.fallback;
}

}
