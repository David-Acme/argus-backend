#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/wrapper/nats/nats-bus.hxx>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

// Opt-in live check against a real NATS + JetStream (ARGUS_NATS_URL).
TEST_CASE("a durable consumer exhausts MaxDeliver and settles the message")
{
  const char* url = std::getenv("ARGUS_NATS_URL");
  if (url == nullptr || *url == '\0') {
    MESSAGE("ARGUS_NATS_URL not set; live DLQ check skipped");
    return;
  }

  NatsBus bus;
  NatsBus::Options options;
  options.url = url;
  options.reconnectWaitMs = 200;
  options.maxReconnects = 5;
  REQUIRE(bus.connect(options));
  REQUIRE(bus.ensureStream({.name = "ARGUS_GUARD_DLQTEST",
                            .subjects = {"argus.test.guard.dlq"},
                            .maxAgeNs = 3600LL * 1000000000,
                            .duplicatesNs = 0}));

  std::mutex mutex;
  std::condition_variable cv;
  std::vector<int> deliveries;
  bool settled = false;
  const auto subscription = bus.subscribeDurable(
      {.stream = "ARGUS_GUARD_DLQTEST",
       .durable = "dlq-live-" + std::to_string(::getpid()),
       .subject = "argus.test.guard.dlq",
       .deliverAll = false,
       .maxDeliver = 3,
       .handler = [&mutex, &cv, &deliveries, &settled](
                      const NatsBus::DurableMessage& message,
                      NatsBus::DurableSettlement settlement) {
         {
           std::lock_guard lock(mutex);
           deliveries.push_back(message.delivered);
           if (message.delivered >= 3)
             settled = true;
         }
         if (message.delivered >= 3) {
           settlement.term();
           cv.notify_all();
           return;
         }
         settlement.nak();
         cv.notify_all();
       }});
  REQUIRE(subscription.has_value());

  REQUIRE(bus.publish("argus.test.guard.dlq", "{\"poison\":true}"));
  {
    std::unique_lock lock(mutex);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(20),
                        [&settled]() { return settled; }));
  }
  CHECK(deliveries.size() >= 3);
  CHECK(deliveries.back() == 3);

  bus.unsubscribe(*subscription);
  bus.drain();
}
