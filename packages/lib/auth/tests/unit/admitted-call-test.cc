#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/admitted-call.hxx>
#include <auth/auth-errors.hxx>
#include <errors/response-exception.hxx>
#include <runtime/blocking-pool.hxx>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace
{
class LightLaneHold
{
public:
  LightLaneHold()
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
      fill();
      const auto threads = static_cast<std::size_t>(blocking_pool::statsOf(BlockingLane::Light).threads);
      if (started_->load() >= threads && saturated())
        return;
      if (std::chrono::steady_clock::now() >= deadline)
        return;
      std::this_thread::yield();
    }
  }

  ~LightLaneHold()
  {
    gate_->count_down();
    for (auto seen = done_->load(); seen < held_; seen = done_->load())
      done_->wait(seen);
  }

  LightLaneHold(const LightLaneHold&) = delete;
  LightLaneHold& operator=(const LightLaneHold&) = delete;

  [[nodiscard]] std::size_t held() const { return held_; }

  [[nodiscard]] bool saturated() const
  {
    const auto stats = blocking_pool::statsOf(BlockingLane::Light);
    return stats.idle == 0 &&
           stats.queued >= blocking_pool::limitsFor(BlockingLane::Light).maxQueued &&
           started_->load() >= static_cast<std::size_t>(stats.threads);
  }

private:
  void fill()
  {
    while (blocking_pool::trySubmit(BlockingLane::Light,
                                    [gate = gate_, started = started_, done = done_] {
                                      started->fetch_add(1);
                                      gate->wait();
                                      done->fetch_add(1);
                                      done->notify_all();
                                    }))
      ++held_;
  }

  std::shared_ptr<std::latch> gate_ = std::make_shared<std::latch>(1);
  std::shared_ptr<std::atomic<std::size_t>> started_ =
      std::make_shared<std::atomic<std::size_t>>(0);
  std::shared_ptr<std::atomic<std::size_t>> done_ =
      std::make_shared<std::atomic<std::size_t>>(0);
  std::size_t held_{0};
};

struct Refusal
{
  bool refused{false};
  int status{0};
  std::string code;
};

Refusal refusalOf(std::function<int()> call)
{
  try {
    drogon::sync_wait(auth_admission::admitted<int>(std::move(call)));
  }
  catch (const ResponseException& refusal) {
    return {.refused = true, .status = refusal.statusCode(), .code = refusal.errorCode()};
  }
  return {};
}
}

TEST_CASE("an admitted call runs on the light lane while it has room")
{
  CHECK(drogon::sync_wait(auth_admission::admitted<int>([] { return 7; })) == 7);
}

TEST_CASE("a full light lane refuses the call with the busy answer instead of "
          "queueing it")
{
  std::atomic<bool> ran{false};
  {
    const LightLaneHold hold;
    REQUIRE(hold.held() >= blocking_pool::limitsFor(BlockingLane::Light).maxQueued);
    REQUIRE(hold.saturated());
    const auto refusal = refusalOf([&ran] {
      ran.store(true);
      return 1;
    });
    REQUIRE(refusal.refused);
    CHECK(refusal.status == AuthErrors::AuthBusy.status);
    CHECK(refusal.code == std::string(AuthErrors::AuthBusy.wireCode()));
  }
  CHECK_FALSE(ran.load());
  CHECK(drogon::sync_wait(auth_admission::admitted<int>([] { return 9; })) == 9);
}

TEST_CASE("a refusal the call itself throws reaches the caller unchanged")
{
  const auto refusal = refusalOf([]() -> int {
    throw ResponseException(AuthErrors::AuthUnavailable);
  });
  REQUIRE(refusal.refused);
  CHECK(refusal.status == AuthErrors::AuthUnavailable.status);
}
