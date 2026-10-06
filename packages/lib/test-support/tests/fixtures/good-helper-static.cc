#include <test-support/app-runner.hxx>

using test_support::AppRunner;

void boot()
{
  static const auto runner = [] {
    return std::make_unique<AppRunner>();
  }();
}
