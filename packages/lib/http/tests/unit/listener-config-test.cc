#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <filesystem>
#include <fstream>
#include <http/listener-config.hxx>
#include <string>
#include <unistd.h>

namespace
{

class TempConfig
{
public:
  explicit TempConfig(const std::string& body)
      : path_(std::filesystem::temp_directory_path() /
              ("argus-listener-config-test-" + std::to_string(::getpid()) +
               ".toml"))
  {
    std::ofstream(path_) << body;
    ConfigService::load(path_.string());
  }

  ~TempConfig() { std::filesystem::remove(path_); }

  TempConfig(const TempConfig&) = delete;
  TempConfig& operator=(const TempConfig&) = delete;
  TempConfig(TempConfig&&) = delete;
  TempConfig& operator=(TempConfig&&) = delete;

private:
  std::filesystem::path path_;
};

}

TEST_CASE("internal TLS config reads the directory and the listener address")
{
  const TempConfig config(
      "[rpc]\naddress = \"0.0.0.0:7129\"\n\n"
      "[internal_tls]\ndir = \"/run/secrets/internal-tls\"\n"
      "address = \"0.0.0.0:7329\"\n");
  const auto internalTls = InternalTlsConfig::resolve();
  CHECK(internalTls.directory == "/run/secrets/internal-tls");
  CHECK(internalTls.address == "0.0.0.0:7329");
}

TEST_CASE("internal TLS config without its table is off")
{
  const TempConfig config("[rpc]\naddress = \"127.0.0.1:7129\"\n");
  const auto internalTls = InternalTlsConfig::resolve();
  CHECK(internalTls.directory.empty());
  CHECK(internalTls.address.empty());
}

TEST_CASE("an empty internal TLS directory keeps the address it was given")
{
  const TempConfig config("[internal_tls]\ndir = \"\"\n"
                          "address = \"127.0.0.1:7329\"\n");
  const auto internalTls = InternalTlsConfig::resolve();
  CHECK(internalTls.directory.empty());
  CHECK(internalTls.address == "127.0.0.1:7329");
}
