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

struct JwtVerifyInput
{
  const std::string& token;
  const std::string& secret;
};

class JwtService
{
public:
  explicit JwtService(JwtRole role);
  ~JwtService() = default;

  [[nodiscard]] std::string generate(const JwtGenerateInput& input) const;

  [[nodiscard]] std::map<std::string, std::string> verify(const JwtVerifyInput& input) const;

  [[nodiscard]] std::string
  generateAccess(const std::map<std::string, std::string>& claims) const;
  [[nodiscard]] std::string
  generateRefresh(const std::map<std::string, std::string>& claims) const;
  [[nodiscard]] std::map<std::string, std::string>
  verifyAccess(const std::string& token) const;
  [[nodiscard]] bool isExpired(const std::string& token) const;
  [[nodiscard]] std::map<std::string, std::string>
  verifyRefresh(const std::string& token) const;

  [[nodiscard]] int64_t refreshTtlSeconds() const { return refreshTtlSeconds_; }
  [[nodiscard]] int64_t accessTtlSeconds() const { return accessTtlSeconds_; }

  static constexpr std::string_view kTypeClaim = "typ";
  static constexpr std::string_view kAccessType = "access";
  static constexpr std::string_view kRefreshType = "refresh";

private:
  std::string accessSecret_;
  std::string refreshSecret_;
  int64_t accessTtlSeconds_;
  int64_t refreshTtlSeconds_;
};
