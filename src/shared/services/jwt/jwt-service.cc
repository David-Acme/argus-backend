#include "jwt-service.hxx"

#include <chrono>
#include <drogon/drogon.h>
#include <jwt-cpp/traits/nlohmann-json/defaults.h>
#include <shared/services/config-service/config-service.hxx>

JwtService::JwtService()
    : accessSecret_(ConfigService::getString("jwt.secret")),
      refreshSecret_(ConfigService::getString("jwt.refresh_secret")),
      accessTtlSeconds_(ConfigService::getInt("jwt.access_ttl_minutes") * 60),
      refreshTtlSeconds_(ConfigService::getInt("jwt.refresh_ttl_days") * 86400)
{
}

std::string
JwtService::generate(const std::map<std::string, std::string>& claims,
                     const std::string& secret, int64_t expiresInSeconds) const
{
  auto builder = jwt::create()
                     .set_issuer("argus")
                     .set_issued_at(std::chrono::system_clock::now())
                     .set_expires_at(std::chrono::system_clock::now() +
                                     std::chrono::seconds{expiresInSeconds});

  for (const auto& [key, value] : claims) {
    builder.set_payload_claim(key, jwt::claim(std::string(value)));
  }

  return builder.sign(jwt::algorithm::hs256{secret});
}
std::map<std::string, std::string>
JwtService::verify(const std::string& token, const std::string& secret) const
{
  try {
    auto decoded = jwt::decode(token);

    auto verifier =
        jwt::verify().allow_algorithm(jwt::algorithm::hs256{secret});
    verifier.verify(decoded);

    std::map<std::string, std::string> result;
    auto payload = decoded.get_payload_json();

    for (const auto& [key, val] : payload) {
      if (val.is_string()) {
        result[key] = val.get<std::string>();
      }
      else if (val.is_number_integer()) {
        result[key] = std::to_string(val.get<int64_t>());
      }
    }

    return result;
  }
  catch (const std::exception& ex) {
    LOG_WARN << "JWT verification failed: " << ex.what();
    return {};
  }
}
std::string JwtService::generateAccess(
    const std::map<std::string, std::string>& claims) const
{
  return generate(claims, accessSecret_, accessTtlSeconds_);
}

std::string JwtService::generateRefresh(
    const std::map<std::string, std::string>& claims) const
{
  return generate(claims, refreshSecret_, refreshTtlSeconds_);
}

std::map<std::string, std::string>
JwtService::verifyAccess(const std::string& token) const
{
  return verify(token, accessSecret_);
}

std::map<std::string, std::string>
JwtService::verifyRefresh(const std::string& token) const
{
  return verify(token, refreshSecret_);
}
