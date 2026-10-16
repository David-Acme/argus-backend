#include "refusal-log.hxx"

namespace auth_refusal
{
std::string_view reasonOfVerdict(std::string_view verdictReason)
{
  if (verdictReason.find("expired") != std::string_view::npos)
    return "expired";
  if (verdictReason.find("device") != std::string_view::npos)
    return "device-mismatch";
  return "invalid";
}

std::string line(std::string_view path, int status, std::string_view reason)
{
  return "auth refused: " + std::string(path) + " status " + std::to_string(status) + " reason " +
         std::string(reason);
}
}
