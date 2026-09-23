#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <json/value.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{

std::string writeTemp(const std::string& name, const std::string& body)
{
  std::ofstream out(name);
  out << body;
  return name;
}

std::string readFile(const std::string& name)
{
  std::ifstream in(name);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

}

TEST_CASE("dotted paths resolve through the nested tables")
{
  const std::string path = writeTemp("config-service-test.toml", R"(
[server]
port = 7040
name = "argus-test"

[llm]
gpu_layers = -1
enabled = true
ratio = 0.25
)");

  ConfigService::load(path);

  CHECK(ConfigService::getInt("server.port") == 7040);
  CHECK(ConfigService::getString("server.name") == "argus-test");
  CHECK(ConfigService::getInt("llm.gpu_layers") == -1);
  CHECK(ConfigService::getBool("llm.enabled"));
  CHECK(ConfigService::getDouble("llm.ratio") == doctest::Approx(0.25));

  std::remove(path.c_str());
}

TEST_CASE("an absent key answers its type default and hasKey reports so")
{
  const std::string path = writeTemp("config-service-absent.toml", R"(
[server]
port = 7040
)");

  ConfigService::load(path);

  CHECK(ConfigService::getString("server.absent").empty());
  CHECK(ConfigService::getInt("server.absent") == 0);
  CHECK(ConfigService::getBool("server.absent") == false);
  CHECK(ConfigService::getDouble("server.absent") == doctest::Approx(0.0));

  CHECK(ConfigService::hasKey("server.port"));
  CHECK_FALSE(ConfigService::hasKey("server.absent"));
  CHECK_FALSE(ConfigService::hasKey("server.port.deeper"));
  CHECK_FALSE(ConfigService::hasKey("nothing.here"));

  std::remove(path.c_str());
}

TEST_CASE("the runtime override wins over the file and hasKey sees it")
{
  const std::string path = writeTemp("config-service-override.toml", R"(
[override]
demo = "from-file"
)");

  ConfigService::load(path);
  CHECK(ConfigService::getString("override.demo") == "from-file");

  ConfigService::setRuntimeString("override.demo", "from-override");
  CHECK(ConfigService::getString("override.demo") == "from-override");
  CHECK(ConfigService::hasKey("override.demo"));

  CHECK_FALSE(ConfigService::hasKey("storage.s3.bucket"));
  ConfigService::setRuntimeString("storage.s3.bucket", "argus-test");
  CHECK(ConfigService::hasKey("storage.s3.bucket"));
  CHECK(ConfigService::getString("storage.s3.bucket") == "argus-test");

  ConfigService::setRuntimeString("feature.enabled", "true");
  CHECK(ConfigService::getBool("feature.enabled"));

  std::remove(path.c_str());
}

TEST_CASE("a malformed file is fatal and leaves the loaded config alone")
{
  const std::string good = writeTemp("config-service-good.toml", R"(
[server]
port = 7040
)");
  ConfigService::load(good);

  const std::string bad = writeTemp("config-service-bad.toml", R"(
[server
port = 7040
)");
  CHECK_THROWS_AS(ConfigService::load(bad), std::runtime_error);

  CHECK(ConfigService::getInt("server.port") == 7040);

  std::remove(good.c_str());
  std::remove(bad.c_str());
}

TEST_CASE("an overlay replaces the top-level table it carries")
{
  const std::string base = writeTemp("config-service-base.toml", R"(
[server]
port = 7040
name = "base"

[mdns]
name = "base-mdns"
)");
  const std::string overlay = writeTemp("config-service-overlay.toml", R"(
[server]
name = "overlay"
)");

  ConfigService::load(base);
  ConfigService::loadOverlay(overlay);

  CHECK(ConfigService::getString("server.name") == "overlay");
  CHECK_FALSE(ConfigService::hasKey("server.port"));
  CHECK(ConfigService::getString("mdns.name") == "base-mdns");

  ConfigService::load(base);
  CHECK(ConfigService::getInt("server.port") == 7040);
  CHECK(ConfigService::getString("server.name") == "base");

  std::remove(base.c_str());
  std::remove(overlay.c_str());
}

TEST_CASE("getStringPairs reads a table of scalars in the table's key order")
{
  const std::string path = writeTemp("config-service-pairs.toml", R"(
[peers]
beta = "127.0.0.1:7041"
alpha = "127.0.0.1:7040"
gamma = 7
)");

  ConfigService::load(path);

  const std::vector<std::pair<std::string, std::string>> pairs =
      ConfigService::getStringPairs("peers");
  REQUIRE(pairs.size() == 2);
  CHECK(pairs[0].first == "alpha");
  CHECK(pairs[0].second == "127.0.0.1:7040");
  CHECK(pairs[1].first == "beta");
  CHECK(pairs[1].second == "127.0.0.1:7041");

  CHECK(ConfigService::getStringPairs("server").empty());
  CHECK(ConfigService::getStringPairs("nothing.here").empty());

  std::remove(path.c_str());
}

TEST_CASE("drogonConfig answers empty when the file carries no drogon table")
{
  const std::string path = writeTemp("config-service-drogon.toml", R"(
[server]
port = 7040
)");

  ConfigService::load(path);
  CHECK(ConfigService::drogonConfig().isNull());

  std::remove(path.c_str());
}

TEST_CASE("a persisting setter rewrites the file and keeps its comments")
{
  const std::string path = writeTemp("config-service-persist.toml", R"(
# Written once by the provisioning script; the comments are the operator's.
[storage]
# where the objects live
bucket = "old-bucket"
region = "eu-west-1"

[feature]
enabled = false
)");

  ConfigService::load(path);

  CHECK(ConfigService::setString("storage.bucket", "new-bucket"));
  CHECK(ConfigService::setInt("storage.retries", 3));
  CHECK(ConfigService::setBool("feature.enabled", true));

  const std::string written = readFile(path);
  CHECK(written.find("bucket = \"new-bucket\"") != std::string::npos);
  CHECK(written.find("old-bucket") == std::string::npos);
  CHECK(written.find("# Written once by the provisioning script") !=
        std::string::npos);
  CHECK(written.find("# where the objects live") != std::string::npos);
  CHECK(written.find("region = \"eu-west-1\"") != std::string::npos);
  CHECK(written.find("retries = 3") != std::string::npos);
  CHECK(written.find("enabled = true") != std::string::npos);
  CHECK(ConfigService::getString("storage.bucket") == "new-bucket");
  CHECK(ConfigService::getInt("storage.retries") == 3);
  CHECK(ConfigService::getBool("feature.enabled"));

  std::remove(path.c_str());
}
