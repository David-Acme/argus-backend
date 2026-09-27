#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/camera-driver/tapo-driver.hxx>
#include <shared/services/tapo/tapo-crypto.hxx>

#include <string>

namespace
{
std::string digestField(const std::string& header, const std::string& key)
{
  const std::string needle = key + "=\"";
  const size_t start = header.find(needle);
  if (start == std::string::npos)
    return {};
  const size_t from = start + needle.size();
  const size_t end = header.find('"', from);
  if (end == std::string::npos)
    return {};
  return header.substr(from, end - from);
}

tapo_crypto::DigestInput digestInput(const std::string& password,
                                     const std::string& algorithm)
{
  return {.username = "admin",
           .password = password,
           .realm = "TP-Link IP-Camera",
           .nonce = "0123456789abcdef0123456789abcdef",
           .qop = "auth",
           .opaque = "opaque",
           .algorithm = algorithm,
           .method = "POST",
           .uri = "/stream",
           .cnonce = "0123456789abcdef",
           .nonceCount = 1};
}
}

TEST_CASE("the talk digest is computed in lowercase hex")
{
  const std::string header =
      tapo_crypto::buildDigestHeader(digestInput("secret", "MD5"));
  CHECK(digestField(header, "response") == "421fc6e14bbc0d952066e3825dd19391");
  CHECK(header.find("username=\"admin\"") != std::string::npos);
  CHECK(header.find("algorithm=MD5") != std::string::npos);
}

TEST_CASE("password variants are uppercase hex too")
{
  CHECK(tapo_crypto::md5Hex("secret") == "5EBE2294ECD0E0F08EAB7690D2A6EE69");
  CHECK(tapo_crypto::sha256Hex("secret") == "2BB80D537B1DA3E38BD30361AA855686BDE0EACD7162FEF6A25FE97BF527A25B");
}

TEST_CASE("the talk channel authenticates as admin")
{
  CHECK(tapoTalkUsername() == "admin");
}
