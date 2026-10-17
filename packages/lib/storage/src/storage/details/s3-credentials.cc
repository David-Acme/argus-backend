#include "s3-credentials.hxx"

#include <fstream>
#include <json/json.h>
#include <utility>

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

}
