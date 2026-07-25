#pragma once

#include <cstdint>
#include <map>
#include <string>

class JwtService
{
public:
  JwtService() = delete;
  ~JwtService() = delete;

  static std::string generate(const std::map<std::string, std::string>& claims,
                              const std::string& secret,
                              int64_t expiresInSeconds = 3600);

  static std::map<std::string, std::string> verify(const std::string& token,
                                                   const std::string& secret);

  static std::string generateAccess(const std::map<std::string, std::string>& claims);
  static std::string generateRefresh(const std::map<std::string, std::string>& claims);
  static std::map<std::string, std::string> verifyAccess(const std::string& token);
  static std::map<std::string, std::string> verifyRefresh(const std::string& token);
};
