#pragma once

#include <cstdint>
#include <map>
#include <string>

class JwtService
{
public:
  JwtService();
  ~JwtService() = default;

  std::string generate(const std::map<std::string, std::string>& claims,
                       const std::string& secret,
                       int64_t expiresInSeconds = 3600) const;

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

private:
  std::string accessSecret_;
  std::string refreshSecret_;
  int64_t accessTtlSeconds_;
  int64_t refreshTtlSeconds_;
};
