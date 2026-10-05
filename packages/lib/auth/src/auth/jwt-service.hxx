#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

enum class JwtRole : std::uint8_t
{
  Issuer = 0,
  Verifier
};

struct JwtGenerateInput
{
  const std::map<std::string, std::string>& claims;
  const std::string& secret;
  int64_t expiresInSeconds;
};

class JwtService
{
public:
  explicit JwtService(JwtRole role = JwtRole::Issuer);
  ~JwtService() = default;

  std::string generate(const JwtGenerateInput& input) const;

  std::map<std::string, std::string> verify(const std::string& token,
                                            const std::string& secret) const;

  std::string
  generateAccess(const std::map<std::string, std::string>& claims) const;
  std::string
  generateRefresh(const std::map<std::string, std::string>& claims) const;
  std::map<std::string, std::string>
  verifyAccess(const std::string& token) const;
  std::map<std::string, std::string>
  verifyRefresh(const std::string& token) const;

  int64_t refreshTtlSeconds() const { return refreshTtlSeconds_; }
  int64_t accessTtlSeconds() const { return accessTtlSeconds_; }

  static constexpr std::string_view kTypeClaim = "typ";
  static constexpr std::string_view kAccessType = "access";
  static constexpr std::string_view kRefreshType = "refresh";

private:
  std::string accessSecret_;
  std::string refreshSecret_;
  int64_t accessTtlSeconds_;
  int64_t refreshTtlSeconds_;
};
