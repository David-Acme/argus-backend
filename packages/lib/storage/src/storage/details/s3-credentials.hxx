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

[[nodiscard]] std::optional<Credentials> fromFile(const std::string& path);

}
