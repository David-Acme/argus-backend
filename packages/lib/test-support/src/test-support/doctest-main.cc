#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <test-support/suite-end.hxx>

#include <functional>
#include <mutex>
#include <ranges>
#include <utility>
#include <vector>

namespace
{
struct SuiteEndHooks
{
  std::mutex mutex;
  std::vector<std::function<void()>> hooks;
};

SuiteEndHooks& suiteEndHooks()
{
  static SuiteEndHooks registry;
  return registry;
}

void runSuiteEndHooks()
{
  std::vector<std::function<void()>> hooks;
  {
    std::scoped_lock lock(suiteEndHooks().mutex);
    hooks.swap(suiteEndHooks().hooks);
  }
  for (const auto& hook : hooks | std::views::reverse)
    hook();
}
}

void test_support::atSuiteEnd(std::function<void()> hook)
{
  std::scoped_lock lock(suiteEndHooks().mutex);
  suiteEndHooks().hooks.push_back(std::move(hook));
}

int main(int argc, char** argv)
{
  doctest::Context context(argc, argv);
  const int result = context.run();
  runSuiteEndHooks();
  return result;
}
