#include <doctest/doctest.h>
#include <drogon/utils/coroutine.h>
#include <feature/camera-control/services/duty-gate.hxx>
#include <feature/camera-control/services/ptz-lease.hxx>

#include <optional>

namespace
{
PtzClaim claimOf(PtzHolder holder, int64_t userId, int64_t atMs)
{
  return {.cameraId = 1, .holder = holder, .userId = userId, .atMs = atMs};
}
}

TEST_CASE("a person always wins the lease, and Argus waits out the park time")
{
  PtzLease lease;
  lease.configure({.parkMs = 60000, .idleReleaseMs = 60000});

  CHECK(lease.claim(claimOf(PtzHolder::Argus, 0, 1000)) == PtzClaimResult::Granted);
  CHECK(lease.claim(claimOf(PtzHolder::Person, 7, 2000)) == PtzClaimResult::Preempted);
  CHECK_FALSE(lease.holds(claimOf(PtzHolder::Argus, 0, 2000)));
  CHECK(lease.claim(claimOf(PtzHolder::Argus, 0, 3000)) == PtzClaimResult::Refused);
  CHECK(lease.claim(claimOf(PtzHolder::Viewer, 8, 3000)) == PtzClaimResult::Refused);

  lease.release(claimOf(PtzHolder::Person, 7, 10000));
  CHECK(lease.stateOf(1, 10000).holder == PtzHolder::None);
  CHECK(lease.stateOf(1, 10000).parkUntilMs == 70000);
  CHECK(lease.claim(claimOf(PtzHolder::Argus, 0, 69999)) == PtzClaimResult::Refused);
  CHECK(lease.claim(claimOf(PtzHolder::Argus, 0, 70000)) == PtzClaimResult::Granted);
}

TEST_CASE("a viewer outranks Argus and yields to a person")
{
  PtzLease lease;
  CHECK(lease.claim(claimOf(PtzHolder::Viewer, 8, 1000)) == PtzClaimResult::Granted);
  CHECK(lease.claim(claimOf(PtzHolder::Argus, 0, 1500)) == PtzClaimResult::Refused);
  CHECK(lease.claim(claimOf(PtzHolder::Person, 7, 2000)) == PtzClaimResult::Preempted);
  CHECK(lease.stateOf(1, 2000).holder == PtzHolder::Person);
  CHECK(lease.stateOf(1, 2000).userId == 7);
}

TEST_CASE("an idle person lets go after the idle time, and the park time starts there")
{
  PtzLease lease;
  lease.configure({.parkMs = 60000, .idleReleaseMs = 60000});
  CHECK(lease.claim(claimOf(PtzHolder::Person, 7, 1000)) == PtzClaimResult::Granted);
  CHECK(lease.claim(claimOf(PtzHolder::Person, 7, 30000)) == PtzClaimResult::Granted);
  CHECK(lease.stateOf(1, 89999).holder == PtzHolder::Person);
  const PtzLeaseState idle = lease.stateOf(1, 90000);
  CHECK(idle.holder == PtzHolder::None);
  CHECK(idle.parkUntilMs == 150000);
  CHECK(lease.claim(claimOf(PtzHolder::Argus, 0, 120000)) == PtzClaimResult::Refused);
  CHECK(lease.claim(claimOf(PtzHolder::Argus, 0, 150000)) == PtzClaimResult::Granted);
}

TEST_CASE("only the holder can release, and a released Argus lease leaves no park time")
{
  PtzLease lease;
  CHECK(lease.claim(claimOf(PtzHolder::Argus, 0, 1000)) == PtzClaimResult::Granted);
  lease.release(claimOf(PtzHolder::Person, 7, 1500));
  CHECK(lease.stateOf(1, 1500).holder == PtzHolder::Argus);
  lease.release(claimOf(PtzHolder::Argus, 0, 2000));
  CHECK(lease.stateOf(1, 2000).holder == PtzHolder::None);
  CHECK(lease.claim(claimOf(PtzHolder::Argus, 0, 2001)) == PtzClaimResult::Granted);
}

TEST_CASE("the duty gate refuses a guard it cannot ask and keeps an answer for its time")
{
  DutyGate gate;
  gate.configure({.client = nullptr, .ttlMs = 15000});

  CHECK_FALSE(drogon::sync_wait(gate.onDuty(4)));
  CHECK_FALSE(gate.cached(4, 0).has_value());

  gate.remember({.userId = 4, .onDuty = true, .atMs = 1000});
  CHECK(gate.cached(4, 15999) == std::optional<bool>(true));
  CHECK_FALSE(gate.cached(4, 16000).has_value());

  gate.remember({.userId = 5, .onDuty = false, .atMs = 1000});
  CHECK(gate.cached(5, 2000) == std::optional<bool>(false));

  gate.configure({.client = nullptr, .ttlMs = 15000});
  CHECK_FALSE(gate.cached(5, 2000).has_value());
}
