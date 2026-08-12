#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <shared/services/queue/job-queue.hxx>
#include <shared/wrapper/thread-budget/thread-budget.hxx>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{

int failures = 0;

void report(bool ok, const char* what)
{
  std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", what);
  if (!ok)
    ++failures;
}

const char* kDbPath = "/tmp/argus-queue-probe.db";
const char* kDonePath = "/tmp/argus-queue-probe-done.txt";

void cleanupDb()
{
  std::remove(kDbPath);
  std::remove((std::string(kDbPath) + "-wal").c_str());
  std::remove((std::string(kDbPath) + "-shm").c_str());
  std::remove(kDonePath);
}

std::set<int64_t> readDoneFile()
{
  std::set<int64_t> ids;
  std::ifstream file(kDonePath);
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty())
      ids.insert(std::strtoll(line.c_str(), nullptr, 10));
  }
  return ids;
}

int testPriorityOrdering()
{
  std::printf("== priority ordering ==\n");
  QueueManager manager;
  std::vector<int> order;
  std::mutex orderMutex;
  manager.registerQueue({.name = "probe.prio",
                         .workers = 1,
                         .maxAttempts = 3,
                         .durable = true},
                        [&](const Job& job) {
                          std::lock_guard<std::mutex> lock(orderMutex);
                          order.push_back(std::atoi(job.payload.c_str()));
                          return JobResult{};
                        });
  manager.start(kDbPath);
  std::mt19937 rng(7);
  std::vector<int> priorities;
  for (int i = 0; i < 300; ++i)
    priorities.push_back(static_cast<int>(rng() % 10));
  for (size_t i = 0; i < priorities.size(); ++i) {
    manager.add("probe.prio", std::to_string(priorities[i]),
                {.priority = priorities[i],
                 .maxAttempts = 3,
                 .backoffMs = 1000,
                 .delayMs = 50,
                 .durable = false,
                 .dedupeKey = ""});
  }
  manager.drain(5000);
  manager.shutdown();
  bool ok = order.size() == priorities.size();
  for (size_t i = 1; ok && i < order.size(); ++i) {
    if (order[i] > order[i - 1]) {
      ok = false;
      std::printf("  [FAIL] out-of-order at %zu: %d > %d\n", i, order[i],
                  order[i - 1]);
    }
  }
  report(ok, "single worker processes in strict priority order");
  return failures;
}

int testConcurrency()
{
  std::printf("== concurrency ==\n");
  QueueManager manager;
  std::atomic<int> done{0};
  manager.registerQueue({.name = "probe.par",
                         .workers = 4,
                         .maxAttempts = 3,
                         .durable = true},
                        [&](const Job&) {
                          std::this_thread::sleep_for(
                              std::chrono::milliseconds(20));
                          done.fetch_add(1);
                          return JobResult{};
                        });
  manager.start(kDbPath);
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 40; ++i)
    manager.add("probe.par", std::to_string(i),
                {.priority = 0,
                 .maxAttempts = 3,
                 .backoffMs = 1000,
                 .delayMs = 0,
                 .durable = false,
                 .dedupeKey = ""});
  manager.drain(10000);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  const auto dbgStats = manager.stats("probe.par");
  manager.shutdown();
  const bool ok = done.load() == 40 && ms < 400;
  std::printf("  completed=%d elapsed=%lld ms (serial would be 800) "
              "stats: waiting=%zu active=%zu completed=%zu failed=%zu\n",
              done.load(), static_cast<long long>(ms), dbgStats.waiting,
              dbgStats.active, dbgStats.completed, dbgStats.failed);
  report(ok, "4 workers run jobs concurrently");
  return failures;
}

int testRetryBackoff()
{
  std::printf("== retry / backoff ==\n");
  QueueManager manager;
  std::atomic<int> runs{0};
  std::atomic<int> sawAttempts{0};
  manager.registerQueue({.name = "probe.retry",
                         .workers = 1,
                         .maxAttempts = 3,
                         .durable = true},
                        [&](const Job& job) {
                          runs.fetch_add(1);
                          sawAttempts.store(job.attempts);
                          if (job.attempts < 2)
                            return JobResult{.ok = false, .error = "flaky"};
                          return JobResult{};
                        });
  manager.start(kDbPath);
  manager.add("probe.retry", "x",
              {.priority = 0,
               .maxAttempts = 3,
               .backoffMs = 10,
               .delayMs = 0,
               .durable = true,
               .dedupeKey = ""});
  manager.drain(10000);
  const auto stats = manager.stats("probe.retry");
  manager.shutdown();
  const bool ok = runs.load() == 3 && stats.completed == 1 &&
                  stats.failed == 0 && sawAttempts.load() == 2;
  std::printf("  runs=%d attempts-before-success=%d\n", runs.load(),
              sawAttempts.load());
  report(ok, "retries with backoff until success");

  QueueManager manager2;
  manager2.registerQueue({.name = "probe.retry2",
                          .workers = 1,
                          .maxAttempts = 3,
                          .durable = true},
                         [&](const Job&) {
                           return JobResult{.ok = false, .error = "always"};
                         });
  manager2.start(kDbPath);
  manager2.add("probe.retry2", "x",
               {.priority = 0,
                .maxAttempts = 2,
                .backoffMs = 5,
                .delayMs = 0,
                .durable = true,
                .dedupeKey = ""});
  manager2.drain(10000);
  const auto stats2 = manager2.stats("probe.retry2");
  manager2.shutdown();
  const bool ok2 = stats2.failed == 1 && stats2.completed == 0;
  std::printf("  maxAttempts=2 -> failed=%zu\n", stats2.failed);
  report(ok2, "max attempts exhausted -> failed state");
  return failures;
}

int testDedupe()
{
  std::printf("== dedupe ==\n");
  QueueManager manager;
  std::atomic<int> runs{0};
  manager.registerQueue({.name = "probe.dedupe",
                         .workers = 1,
                         .maxAttempts = 3,
                         .durable = true},
                        [&](const Job&) {
                          runs.fetch_add(1);
                          return JobResult{};
                        });
  manager.start(kDbPath);
  for (int i = 0; i < 5; ++i)
    manager.add("probe.dedupe", "payload-" + std::to_string(i),
                {.priority = 0,
                 .maxAttempts = 3,
                 .backoffMs = 1000,
                 .delayMs = 0,
                 .durable = true,
                 .dedupeKey = "same-key"});
  for (int i = 0; i < 5; ++i)
    manager.add("probe.dedupe", "payload-" + std::to_string(i),
                {.priority = 0,
                 .maxAttempts = 3,
                 .backoffMs = 1000,
                 .delayMs = 0,
                 .durable = true,
                 .dedupeKey = "key-" + std::to_string(i)});
  manager.drain(10000);
  manager.shutdown();
  const bool ok = runs.load() == 6;
  std::printf("  deduped runs=%d (expected 1 + 5)\n", runs.load());
  report(ok, "same dedupeKey collapses to one job");
  return failures;
}

int testDrain()
{
  std::printf("== drain correctness ==\n");
  QueueManager manager;
  std::atomic<int> done{0};
  std::atomic<bool> drainSawActive{false};
  std::atomic<int> maxActive{0};
  std::atomic<int> activeNow{0};
  manager.registerQueue({.name = "probe.drain",
                         .workers = 3,
                         .maxAttempts = 3,
                         .durable = true},
                        [&](const Job&) {
                          const int cur = activeNow.fetch_add(1) + 1;
                          int observed = maxActive.load();
                          while (
                              cur > observed &&
                              !maxActive.compare_exchange_weak(observed, cur))
                            ;
                          std::this_thread::sleep_for(
                              std::chrono::milliseconds(1 + (done.load() % 5)));
                          activeNow.fetch_sub(1);
                          done.fetch_add(1);
                          return JobResult{};
                        });
  manager.start(kDbPath);
  for (int round = 0; round < 5; ++round) {
    for (int i = 0; i < 100; ++i)
      manager.add("probe.drain", std::to_string(round * 100 + i),
                  {.priority = 0,
                   .maxAttempts = 3,
                   .backoffMs = 1000,
                   .delayMs = 0,
                   .durable = false,
                   .dedupeKey = ""});
    manager.drain(10000);
    const auto stats = manager.stats("probe.drain");
    if (stats.waiting != 0 || stats.active != 0)
      drainSawActive.store(true);
  }
  manager.shutdown();
  const bool ok = done.load() == 500 && !drainSawActive.load();
  std::printf("  done=%d maxConcurrent=%d\n", done.load(), maxActive.load());
  report(ok, "drain returns only with empty queue and zero active");
  return failures;
}

int childCrashWorker()
{
  QueueManager manager;
  std::atomic<int> processed{0};
  std::atomic<bool> release{false};
  std::mutex fileMutex;
  std::ofstream doneFile(kDonePath, std::ios::app);
  manager.registerQueue({.name = "probe.crash",
                         .workers = 4,
                         .maxAttempts = 3,
                         .durable = true},
                        [&](const Job& job) {
                          while (!release.load(std::memory_order_acquire))
                            std::this_thread::yield();
                          std::this_thread::sleep_for(
                              std::chrono::milliseconds(1));
                          const std::string line =
                              std::to_string(job.id) + "\n";
                          {
                            std::lock_guard<std::mutex> lock(fileMutex);
                            doneFile << line;
                            doneFile.flush();
                          }
                          if (processed.fetch_add(1) + 1 >= 2000)
                            std::raise(SIGKILL);
                          return JobResult{};
                        });
  manager.start(kDbPath);
  for (int i = 0; i < 5000; ++i)
    manager.add("probe.crash", std::to_string(i));
  release.store(true, std::memory_order_release);
  manager.drain(60000);
  return 0;
}

int testCrashRecovery()
{
  std::printf("== durability across crash ==\n");
  cleanupDb();
  const pid_t child = fork();
  if (child == 0) {
    childCrashWorker();
    _exit(0);
  }
  int status = 0;
  waitpid(child, &status, 0);
  const std::set<int64_t> childDone = readDoneFile();
  std::printf("  child killed mid-run (status=%d, completed=%zu)\n", status,
              childDone.size());

  QueueManager manager;
  std::set<int64_t> parentDone;
  std::mutex setMutex;
  manager.registerQueue({.name = "probe.crash",
                         .workers = 4,
                         .maxAttempts = 3,
                         .durable = true},
                        [&](const Job& job) {
                          std::lock_guard<std::mutex> lock(setMutex);
                          parentDone.insert(job.id);
                          return JobResult{};
                        });
  manager.start(kDbPath);
  manager.drain(60000);
  const auto stats = manager.stats("probe.crash");
  manager.shutdown();

  std::set<int64_t> duplicate;
  for (const int64_t id : parentDone) {
    if (childDone.count(id))
      duplicate.insert(id);
  }
  std::set<int64_t> unionSet = childDone;
  unionSet.insert(parentDone.begin(), parentDone.end());
  const bool complete = unionSet.size() == 5000;
  const bool noLost = stats.waiting == 0 && stats.active == 0;
  const bool atMostInflight = duplicate.size() <= 4;
  std::printf("  parent completed=%zu union=%zu waiting=%zu "
              "duplicates-with-child=%zu\n",
              parentDone.size(), unionSet.size(), stats.waiting,
              duplicate.size());
  const bool ok = complete && noLost && atMostInflight;
  report(ok, "no lost jobs across a mid-run kill; at-most-once rerun");
  return failures;
}

int runQueueBench()
{
  std::printf("\n== queue-bench ==\n");
  QueueManager manager;
  std::atomic<int> done{0};
  manager.registerQueue({.name = "probe.bench",
                         .workers = 4,
                         .maxAttempts = 3,
                         .durable = true},
                        [&](const Job&) {
                          done.fetch_add(1);
                          return JobResult{};
                        });
  manager.start(kDbPath);
  constexpr int kJobs = 10000;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kJobs; ++i)
    manager.add("probe.bench", std::to_string(i),
                {.priority = 0,
                 .maxAttempts = 3,
                 .backoffMs = 1000,
                 .delayMs = 0,
                 .durable = false,
                 .dedupeKey = ""});
  manager.drain(60000);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  manager.shutdown();
  const double jps = kJobs * 1000.0 / static_cast<double>(ms);
  std::printf("  %d jobs in %lld ms -> %.0f jobs/s (non-durable)\n", kJobs,
              static_cast<long long>(ms), jps);
  report(done.load() == kJobs && ms > 0, "non-durable throughput bench ran");

  QueueManager manager2;
  std::atomic<int> done2{0};
  manager2.registerQueue({.name = "probe.benchd",
                          .workers = 4,
                          .maxAttempts = 3,
                          .durable = true},
                         [&](const Job&) {
                           done2.fetch_add(1);
                           return JobResult{};
                         });
  manager2.start(kDbPath);
  const auto t1 = std::chrono::steady_clock::now();
  for (int i = 0; i < kJobs; ++i)
    manager2.add("probe.benchd", std::to_string(i));
  manager2.drain(60000);
  const auto ms2 = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t1)
                       .count();
  manager2.shutdown();
  const double jps2 = kJobs * 1000.0 / static_cast<double>(ms2);
  std::printf("  %d jobs in %lld ms -> %.0f jobs/s (durable)\n", kJobs,
              static_cast<long long>(ms2), jps2);
  report(done2.load() == kJobs, "durable throughput bench ran");
  return failures;
}

} // namespace

int main(int argc, char** argv)
{
  bool doTest = false;
  bool doBench = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--queue-test") == 0)
      doTest = true;
    else if (std::strcmp(argv[i], "--queue-bench") == 0)
      doBench = true;
    else {
      std::printf("unknown flag: %s\n", argv[i]);
      return 2;
    }
  }
  if (!doTest && !doBench)
    doTest = doBench = true;

  int rc = 0;
  if (doTest) {
    cleanupDb();
    rc |= testPriorityOrdering();
    rc |= testConcurrency();
    rc |= testRetryBackoff();
    rc |= testDedupe();
    rc |= testDrain();
    rc |= testCrashRecovery();
    cleanupDb();
  }
  if (doBench) {
    cleanupDb();
    rc |= runQueueBench();
    cleanupDb();
  }
  std::printf("\n%s\n", rc == 0 ? "ALL GATES PASS" : "FAILURES PRESENT");
  return rc;
}
