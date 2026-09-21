#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <grpc/grpc-server-identity.hxx>

#include <string>
#include <utility>
#include <vector>

// The receiving side needs no channel for these two: they are what turns a
// presented credential into the caller a server acts as.

TEST_CASE("constantTimeEquals answers on content, never on where it differs")
{
  CHECK(argus::client::constantTimeEquals("s3cret", "s3cret"));
  CHECK(argus::client::constantTimeEquals("", ""));

  // A difference in the middle, at the end and in a single byte are all
  // refusals; the comparison must not return early on the first of them.
  CHECK_FALSE(argus::client::constantTimeEquals("s3cret", "s3cres"));
  CHECK_FALSE(argus::client::constantTimeEquals("s3cret", "s3cre"));
  CHECK_FALSE(argus::client::constantTimeEquals("s3cret", "s3cret "));
  CHECK_FALSE(argus::client::constantTimeEquals("", "x"));
}

TEST_CASE("callerCredentialsFromPairs keeps the complete pairs in order")
{
  const std::vector<std::pair<std::string, std::string>> pairs{
      {"camera", "camera-secret"},
      {"", "orphan-secret"},
      {"gateway", ""},
      {"voice", "voice-secret"}};

  const std::vector<argus::client::CallerCredential> credentials =
      argus::client::callerCredentialsFromPairs(pairs);

  // A half-filled pair is dropped: a service with no name cannot be authorized
  // as anybody, and an empty secret must never compare equal to an absent one.
  REQUIRE(credentials.size() == 2);
  CHECK(credentials[0].service == "camera");
  CHECK(credentials[0].secret == "camera-secret");
  CHECK(credentials[1].service == "voice");
  CHECK(credentials[1].secret == "voice-secret");

  CHECK(argus::client::callerCredentialsFromPairs({}).empty());
}
