#include "jwt-service.hxx"

#include <shared/services/config-service/config-service.hxx>

#include <jwt-cpp/traits/nlohmann-json/defaults.h>

#include <chrono>

std::string JwtService::generate(const std::map<std::string, std::string>& claims,
                                 const std::string& secret,
                                 int64_t expiresInSeconds)
{
  auto builder = jwt::create()
                     .set_issuer("argus")
                     .set_issued_at(std::chrono::system_clock::now())
                     .set_expires_at(std::chrono::system_clock::now()
                                     + std::chrono::seconds{expiresInSeconds});

  for (const auto& [key, value] : claims) {
    builder.set_payload_claim(key, jwt::claim(std::string(value)));
  }

  return builder.sign(jwt::algorithm::hs256{secret});
}

std::map<std::string, std::string> JwtService::verify(const std::string& token,
                                                      const std::string& secret)
{
  auto decoded = jwt::decode(token);

  auto verifier = jwt::verify().allow_algorithm(jwt::algorithm::hs256{secret});
  verifier.verify(decoded);

  std::map<std::string, std::string> result;
  auto payload = decoded.get_payload_json();

  for (const auto& [key, val] : payload) {
    if (val.is_string()) {
      result[key] = val.get<std::string>();
    } else if (val.is_number_integer()) {
      result[key] = std::to_string(val.get<int64_t>());
    }
  }

  return result;
}

std::string JwtService::generateAccess(const std::map<std::string, std::string>& claims)
{
  return generate(claims, ConfigService::getString("jwt.secret"), 900);
}

std::string JwtService::generateRefresh(const std::map<std::string, std::string>& claims)
{
  return generate(claims, ConfigService::getString("jwt.refresh_secret"), 604800);
}

std::map<std::string, std::string> JwtService::verifyAccess(const std::string& token)
{
  return verify(token, ConfigService::getString("jwt.secret"));
}

std::map<std::string, std::string> JwtService::verifyRefresh(const std::string& token)
{
  return verify(token, ConfigService::getString("jwt.refresh_secret"));
}
