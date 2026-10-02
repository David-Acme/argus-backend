#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <shared/services/face/face-service.hxx>

#include <atomic>
#include <chrono>
#include <thread>

TEST_CASE("a disabled face service answers instead of blocking the caller")
{
  FaceService::instance().disable();

  std::atomic<bool> answered{false};
  std::thread caller([&] {
    CHECK_FALSE(FaceService::instance().identify("not an image").has_value());
    CHECK_FALSE(
        FaceService::instance().extractImage("not an image").has_value());
    answered.store(true);
  });

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!answered.load() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  REQUIRE(answered.load());
  caller.join();
  CHECK_FALSE(FaceService::instance().isLoaded());
}
