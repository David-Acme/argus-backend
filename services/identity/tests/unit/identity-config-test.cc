#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <config/identity-config.hxx>

#include <filesystem>
#include <fstream>
#include <string>

namespace
{

void writeConfig(const std::filesystem::path& path, const std::string& body)
{
  std::ofstream file(path);
  file << body;
}

class TempConfig
{
public:
  TempConfig(const char* name, const std::string& body) : name_(name)
  {
    writeConfig(name_, body);
    ConfigService::load(name_);
  }

  ~TempConfig() { std::filesystem::remove(name_); }

  TempConfig(const TempConfig&) = delete;
  TempConfig& operator=(const TempConfig&) = delete;

private:
  std::filesystem::path name_;
};

}

TEST_CASE("identity db and listener config resolve the service defaults")
{
  const TempConfig config("identity-config-test-defaults.toml", "[identity]\n");

  const IdentityDbConfig db = IdentityConfig::resolveDb();
  CHECK(db.dbPath == "database/identity.db");
  CHECK(db.schemaPath == "services/identity/database/schema.sql");

  const ListenerConfig listener = IdentityConfig::resolveListener();
  CHECK(listener.host == "0.0.0.0");
  CHECK(listener.port == 7044);
  CHECK(listener.tls);
  CHECK(listener.certPath == "certs/server.pem");
  CHECK(listener.keyPath == "certs/server.key");
  CHECK(listener.minTlsProtocol == "TLSv1.2");

  const IdentityRpcConfig rpc = IdentityConfig::resolveRpc();
  CHECK(rpc.listener.host == "127.0.0.1");
  CHECK(rpc.listener.port == 7040);
  CHECK(rpc.secret.empty());
  CHECK_FALSE(rpc.reachableBeyondLoopback());

  CHECK(IdentityConfig::resolveSyncControl().target.empty());
  CHECK_FALSE(IdentityConfig::resolveFace().enabled);
}

TEST_CASE("identity config honors the identity and cert section overrides")
{
  const TempConfig config("identity-config-test-overrides.toml",
                          "[identity]\n"
                          "host = \"127.0.0.1\"\n"
                          "port = 7444\n"
                          "plain = true\n"
                          "min_protocol = \"TLSv1.3\"\n"
                          "db = \"/tmp/argus-test/identity.db\"\n"
                          "schema = \"/tmp/argus-test/identity-schema.sql\"\n"
                          "[cert]\n"
                          "server_cert = \"/tmp/argus-test/server.pem\"\n"
                          "server_key = \"/tmp/argus-test/server.key\"\n");

  const IdentityDbConfig db = IdentityConfig::resolveDb();
  CHECK(db.dbPath == "/tmp/argus-test/identity.db");
  CHECK(db.schemaPath == "/tmp/argus-test/identity-schema.sql");

  const ListenerConfig listener = IdentityConfig::resolveListener();
  CHECK(listener.host == "127.0.0.1");
  CHECK(listener.port == 7444);
  CHECK_FALSE(listener.tls);
  CHECK(listener.certPath == "/tmp/argus-test/server.pem");
  CHECK(listener.keyPath == "/tmp/argus-test/server.key");
  CHECK(listener.minTlsProtocol == "TLSv1.3");
}

TEST_CASE("identity rpc config gates a non-loopback listener on the secret")
{
  const TempConfig config("identity-config-test-rpc.toml",
                          "[server]\n"
                          "host = \"127.0.0.1\"\n"
                          "grpc_port = 7040\n"
                          "[identity]\n"
                          "rpc_secret = \"\"\n");

  const IdentityRpcConfig loopback = IdentityConfig::resolveRpc();
  CHECK(loopback.listener.host == "127.0.0.1");
  CHECK(loopback.listener.port == 7040);
  CHECK(loopback.secret.empty());
  CHECK_FALSE(loopback.reachableBeyondLoopback());

  ConfigService::setRuntimeString("server.host", "172.19.0.1");
  const IdentityRpcConfig exposed = IdentityConfig::resolveRpc();
  CHECK(exposed.reachableBeyondLoopback());
  CHECK(exposed.secret.empty());

  ConfigService::setRuntimeString("identity.rpc_secret", "fleet-secret");
  const IdentityRpcConfig guarded = IdentityConfig::resolveRpc();
  CHECK(guarded.secret == "fleet-secret");
  CHECK(guarded.reachableBeyondLoopback());

  ConfigService::setRuntimeString("server.host", "127.0.0.1");
  ConfigService::setRuntimeString("identity.rpc_secret", "");
  CHECK_FALSE(IdentityConfig::resolveRpc().reachableBeyondLoopback());
}

TEST_CASE("identity peer and face config resolve from their sections")
{
  const TempConfig config("identity-config-test-peers.toml",
                          "[sync]\n"
                          "control_target = \"argus-sync:7041\"\n"
                          "control_secret = \"control-secret\"\n"
                          "[face]\n"
                          "enabled = true\n");

  const IdentitySyncControlConfig sync = IdentityConfig::resolveSyncControl();
  CHECK(sync.target == "argus-sync:7041");
  CHECK(sync.secret == "control-secret");
  CHECK(IdentityConfig::resolveFace().enabled);
}
