#pragma once

#include <optional>
#include <string>

namespace s3_credentials
{

struct Credentials
{
  std::string accessKey;
  std::string secretKey;
};

struct Source
{
  std::string credentialsFile;
  Credentials fallback;
};

[[nodiscard]] std::optional<Credentials> fromFile(const std::string& path);
[[nodiscard]] Credentials resolve(const Source& source);

}
