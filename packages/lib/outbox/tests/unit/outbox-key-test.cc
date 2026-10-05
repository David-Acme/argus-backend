#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <outbox/outbox-backoff.hxx>
#include <outbox/outbox-key.hxx>
#include <outbox/outbox-status.hxx>

#include <chrono>
#include <string>

using outbox::OutboxTiming;
using outbox::retryDelay;

TEST_CASE("a transition id names the table, the record and the discriminator")
{
  const std::string patio = outbox::transitionId({.prefix = "camera-change:",
                                                  .table = "camera",
                                                  .recordId = 7,
                                                  .discriminator = "patio"});
  CHECK(patio == outbox::transitionId({.prefix = "camera-change:",
                                       .table = "camera",
                                       .recordId = 7,
                                       .discriminator = "patio"}));
  CHECK(patio.rfind("camera-change:", 0) == 0);
  CHECK(patio.size() == std::string("camera-change:").size() + 32);
  CHECK(patio != outbox::transitionId({.prefix = "camera-change:",
                                       .table = "camera",
                                       .recordId = 8,
                                       .discriminator = "patio"}));
  CHECK(patio != outbox::transitionId({.prefix = "camera-change:",
                                       .table = "zone",
                                       .recordId = 7,
                                       .discriminator = "patio"}));
  CHECK(outbox::transitionId({.prefix = "x:",
                              .table = "camera",
                              .recordId = 17,
                              .discriminator = "add"}) !=
        outbox::transitionId({.prefix = "x:",
                              .table = "camera",
                              .recordId = 1,
                              .discriminator = "7add"}));
  CHECK(outbox::transitionId({.prefix = "identity-change:",
                              .table = "camera",
                              .recordId = 7,
                              .discriminator = "patio"})
            .substr(16) == patio.substr(14));
}

TEST_CASE("a unique id spells its entropy in hex after the prefix")
{
  const outbox::MsgIdEntropy zero{};
  CHECK(outbox::uniqueId("auth-action:", zero) ==
        "auth-action:00000000000000000000000000000000");
  outbox::MsgIdEntropy full{};
  full.fill(0xFF);
  CHECK(outbox::uniqueId("auth-session:", full) ==
        "auth-session:ffffffffffffffffffffffffffffffff");
  outbox::MsgIdEntropy mixed{};
  mixed[0] = 0x0A;
  mixed[15] = 0xB1;
  CHECK(outbox::uniqueId("", mixed) == "0a0000000000000000000000000000b1");

  const auto first = outbox::drawEntropy();
  const auto second = outbox::drawEntropy();
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  CHECK(first != second);
}

TEST_CASE("a legacy id and a fingerprint are stable")
{
  CHECK(outbox::legacyId("identity-action:", 7) == "identity-action:7");
  CHECK(outbox::legacyId("identity-action:", 7) !=
        outbox::legacyId("identity-action:", 8));
  CHECK(outbox::fingerprint(R"({"id":7})") ==
        outbox::fingerprint(R"({"id":7})"));
  CHECK(outbox::fingerprint(R"({"id":7})") !=
        outbox::fingerprint(R"({"id":8})"));
  CHECK(outbox::fingerprint("").size() == 64);
}

TEST_CASE("the status spells the CHECK constraint's values")
{
  using outbox::OutboxStatus;
  CHECK(outbox::outboxStatusToString(OutboxStatus::Pending) == "pending");
  CHECK(outbox::outboxStatusToString(OutboxStatus::Sent) == "sent");
  CHECK(outbox::outboxStatusFromString("sent") == OutboxStatus::Sent);
  CHECK(outbox::outboxStatusFromString("pending") == OutboxStatus::Pending);
  CHECK(outbox::outboxStatusFromString("other") == OutboxStatus::Pending);
}

TEST_CASE("a stuck relay backs off exponentially up to its ceiling")
{
  const OutboxTiming timing{
      .retryMs = 500, .maxRetryMs = 5000, .progressMs = 50, .batch = 64};
  CHECK(retryDelay(timing, 0) == std::chrono::milliseconds(500));
  CHECK(retryDelay(timing, 1) == std::chrono::milliseconds(500));
  CHECK(retryDelay(timing, 2) == std::chrono::milliseconds(1000));
  CHECK(retryDelay(timing, 3) == std::chrono::milliseconds(2000));
  CHECK(retryDelay(timing, 4) == std::chrono::milliseconds(4000));
  CHECK(retryDelay(timing, 5) == std::chrono::milliseconds(5000));
  CHECK(retryDelay(timing, 1000) == std::chrono::milliseconds(5000));

  const OutboxTiming flat{
      .retryMs = 20, .maxRetryMs = 0, .progressMs = 50, .batch = 64};
  CHECK(retryDelay(flat, 9) == std::chrono::milliseconds(20));

  const OutboxTiming zero{
      .retryMs = 0, .maxRetryMs = 3, .progressMs = 50, .batch = 64};
  CHECK(retryDelay(zero, 1) == std::chrono::milliseconds(1));
  CHECK(retryDelay(zero, 30) == std::chrono::milliseconds(3));
}
