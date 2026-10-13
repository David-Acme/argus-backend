#pragma once

#include <filesystem>
#include <string>

namespace tunnel::test
{
struct TestIdentity
{
  std::string certificatePath;
  std::string keyPath;
  std::string publicKeyPem;
  std::string commonName;
};

struct IdentityInput
{
  std::filesystem::path directory;
  std::string name;
  std::string commonName;
};

std::filesystem::path makeTempDirectory(const std::string& prefix);
TestIdentity mintIdentity(const IdentityInput& input);
}
