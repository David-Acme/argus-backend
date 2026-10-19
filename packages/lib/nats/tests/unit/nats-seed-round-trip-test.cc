#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <nats/nats.h>
#include <string>
#include <string_view>
#include <thread>

namespace
{
constexpr const char* kAllowed = "argus.roundtrip.v1.allowed";
constexpr const char* kRefused = "argus.roundtrip.v1.refused";

std::string env(const char* name)
{
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

bool unconfigured()
{
  return env("ARGUS_NATS_ROUNDTRIP_URL").empty() ||
         env("ARGUS_NATS_ROUNDTRIP_NKEY").empty() ||
         env("ARGUS_NATS_ROUNDTRIP_SEED").empty() ||
         env("ARGUS_NATS_ROUNDTRIP_OTHER_SEED").empty();
}

struct Refusals
{
  std::atomic<int> notPermitted{0};
};

void onAsyncError(natsConnection* connection, natsSubscription* subscription,
                  natsStatus error, void* closure)
{
  (void)connection;
  (void)subscription;
  if (error == NATS_NOT_PERMITTED)
    static_cast<Refusals*>(closure)->notPermitted.fetch_add(1);
}

using OptionsPtr = std::unique_ptr<natsOptions, decltype(&natsOptions_Destroy)>;
using ConnectionPtr =
    std::unique_ptr<natsConnection, decltype(&natsConnection_Destroy)>;
using SubscriptionPtr =
    std::unique_ptr<natsSubscription, decltype(&natsSubscription_Destroy)>;

struct Attempt
{
  ConnectionPtr connection{nullptr, &natsConnection_Destroy};
  natsStatus status{NATS_ERR};
};

struct ConnectInput
{
  std::string seedFile;
  Refusals* refusals{nullptr};
};

Attempt connectWith(const ConnectInput& input)
{
  natsOptions* rawOptions = nullptr;
  Attempt attempt;
  if (natsOptions_Create(&rawOptions) != NATS_OK)
    return attempt;
  const OptionsPtr options(rawOptions, &natsOptions_Destroy);
  const std::string url = env("ARGUS_NATS_ROUNDTRIP_URL");
  const std::string nkey = env("ARGUS_NATS_ROUNDTRIP_NKEY");
  natsStatus status = natsOptions_SetURL(options.get(), url.c_str());
  if (status == NATS_OK)
    status = natsOptions_SetNKeyFromSeed(options.get(), nkey.c_str(),
                                         input.seedFile.c_str());
  if (status == NATS_OK)
    status = natsOptions_SetMaxReconnect(options.get(), 0);
  if (status == NATS_OK)
    status = natsOptions_SetErrorHandler(options.get(), onAsyncError,
                                         input.refusals);
  natsConnection* rawConnection = nullptr;
  if (status == NATS_OK)
    status = natsConnection_Connect(&rawConnection, options.get());
  attempt.connection.reset(rawConnection);
  attempt.status = status;
  return attempt;
}

bool awaitRefusal(const Refusals& refusals)
{
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (refusals.notPermitted.load() > 0)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}
}

TEST_CASE("a seed issued by nats-users.sh authenticates through "
          "natsOptions_SetNKeyFromSeed, publishes inside its allow-list and "
          "is refused outside it" *
          doctest::skip(unconfigured()))
{
  Refusals refusals;
  const Attempt attempt = connectWith(
      {.seedFile = env("ARGUS_NATS_ROUNDTRIP_SEED"), .refusals = &refusals});
  REQUIRE_MESSAGE(attempt.status == NATS_OK,
                  "the seed did not authenticate: "
                      << std::string(natsStatus_GetText(attempt.status)));
  natsConnection* connection = attempt.connection.get();

  natsSubscription* rawSubscription = nullptr;
  REQUIRE(natsConnection_SubscribeSync(&rawSubscription, connection,
                                       kAllowed) == NATS_OK);
  const SubscriptionPtr subscription(rawSubscription,
                                     &natsSubscription_Destroy);
  REQUIRE(natsConnection_PublishString(connection, kAllowed, "inside") ==
          NATS_OK);
  REQUIRE(natsConnection_Flush(connection) == NATS_OK);
  natsMsg* message = nullptr;
  REQUIRE(natsSubscription_NextMsg(&message, subscription.get(), 2000) ==
          NATS_OK);
  const std::unique_ptr<natsMsg, decltype(&natsMsg_Destroy)>
      received(message, &natsMsg_Destroy);
  CHECK(std::string_view(natsMsg_GetData(received.get()),
                         static_cast<size_t>(natsMsg_GetDataLength(
                             received.get()))) == "inside");
  CHECK(refusals.notPermitted.load() == 0);

  REQUIRE(natsConnection_PublishString(connection, kRefused, "outside") ==
          NATS_OK);
  static_cast<void>(natsConnection_Flush(connection));
  CHECK_MESSAGE(awaitRefusal(refusals),
                "the broker accepted a publish outside the allow-list");
  const char* lastError = nullptr;
  static_cast<void>(natsConnection_GetLastError(connection, &lastError));
  REQUIRE(lastError != nullptr);
  CHECK(std::string_view(lastError).find("Permissions Violation for Publish to "
                                         "\"argus.roundtrip.v1.refused\"") !=
        std::string_view::npos);
}

TEST_CASE("another seed presented with that public key is refused at connect" *
          doctest::skip(unconfigured()))
{
  Refusals refusals;
  const Attempt attempt =
      connectWith({.seedFile = env("ARGUS_NATS_ROUNDTRIP_OTHER_SEED"),
                   .refusals = &refusals});
  CHECK(attempt.status != NATS_OK);
  CHECK(attempt.connection == nullptr);
}
