#include <runtime/shutdown-signal.hxx>

#include <drogon/drogon.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

namespace shutdown_signal
{
namespace
{

constexpr double kPollSeconds = 0.05;
constexpr std::chrono::milliseconds kDeadline{10000};

std::mutex& registryMutex()
{
  static std::mutex mutex;
  return mutex;
}

std::vector<Drain>& registry()
{
  static std::vector<Drain> drains;
  return drains;
}

std::atomic<bool>& stopRequestedFlag()
{
  static std::atomic<bool> flag{false};
  return flag;
}

std::chrono::steady_clock::time_point& deadline()
{
  static std::chrono::steady_clock::time_point at;
  return at;
}

std::vector<Drain> snapshot()
{
  std::scoped_lock lock(registryMutex());
  return registry();
}

std::mutex& quitHookMutex()
{
  static std::mutex mutex;
  return mutex;
}

std::vector<QuitHook>& quitHooks()
{
  static std::vector<QuitHook> hooks;
  return hooks;
}

std::vector<QuitHook> quitHookSnapshot()
{
  std::scoped_lock lock(quitHookMutex());
  return quitHooks();
}

void runQuitHooks()
{
  for (const auto& hook : quitHookSnapshot()) {
    try {
      hook();
    }
    catch (const std::exception& e) {
      LOG_ERROR << "Shutdown signal: a quit hook failed: " << e.what();
    }
    catch (...) {
      LOG_ERROR << "Shutdown signal: a quit hook failed with an unknown error";
    }
  }
}

bool drainedOrLog(const Drain& drain)
{
  try {
    return drain.drained();
  }
  catch (const std::exception& e) {
    LOG_ERROR << "Shutdown signal: drain '" << drain.name
              << "' failed to report its state: " << e.what();
  }
  catch (...) {
    LOG_ERROR << "Shutdown signal: drain '" << drain.name
              << "' failed to report its state with an unknown error";
  }
  return false;
}

void pollAndQuit()
{
  if (!drained()) {
    if (std::chrono::steady_clock::now() < deadline()) {
      drogon::app().getLoop()->runAfter(kPollSeconds, pollAndQuit);
      return;
    }
    std::size_t pending = 0;
    for (const auto& drain : snapshot()) {
      if (drainedOrLog(drain))
        continue;
      ++pending;
      LOG_ERROR << "Shutdown signal: drain '" << drain.name
                << "' is still running after the shutdown deadline";
    }
    LOG_ERROR << "Shutdown signal: quitting with " << pending
              << " drain(s) that never reported drained";
  }
  runQuitHooks();
  drogon::app().quit();
}

void installHandlers()
{
  static std::once_flag once;
  std::call_once(once, [] {
    drogon::app().setTermSignalHandler([] { requestStop(); });
    drogon::app().setIntSignalHandler([] { requestStop(); });
  });
}

void stopOrLog(const std::string& name,
               const std::function<void()>& requestStop)
{
  try {
    requestStop();
  }
  catch (const std::exception& e) {
    LOG_ERROR << "Shutdown signal: drain '" << name
              << "' refused the stop request: " << e.what();
  }
  catch (...) {
    LOG_ERROR << "Shutdown signal: drain '" << name
              << "' refused the stop request with an unknown error";
  }
}

}

void onStop(Drain drain)
{
  const std::string name = drain.name;
  std::function<void()> requestStop;
  bool late = false;
  {
    std::scoped_lock lock(registryMutex());
    late = stopRequestedFlag().load(std::memory_order_acquire);
    if (late)
      requestStop = drain.requestStop;
    else
      registry().push_back(std::move(drain));
  }
  if (late) {
    LOG_WARN << "Shutdown signal: drain '" << name
             << "' was registered after the shutdown request; it is stopped "
                "at once and the quit does not wait for it";
    stopOrLog(name, requestStop);
  }
  installHandlers();
}

void onQuit(QuitHook hook)
{
  {
    std::scoped_lock lock(quitHookMutex());
    quitHooks().push_back(std::move(hook));
  }
  installHandlers();
}

bool drained()
{
  for (const auto& drain : snapshot()) {
    if (!drainedOrLog(drain))
      return false;
  }
  return true;
}

void requestStop()
{
  if (stopRequestedFlag().exchange(true, std::memory_order_acq_rel))
    return;
  deadline() = std::chrono::steady_clock::now() + kDeadline;
  const std::vector<Drain> pending = snapshot();
  for (const auto& drain : pending)
    stopOrLog(drain.name, drain.requestStop);
  LOG_INFO << "Shutdown signal: stop requested for " << pending.size()
           << " drain(s)";
  drogon::app().getLoop()->runAfter(kPollSeconds, pollAndQuit);
}

}
