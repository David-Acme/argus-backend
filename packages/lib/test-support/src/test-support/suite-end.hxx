#pragma once

#include <functional>

namespace test_support
{
void atSuiteEnd(std::function<void()> hook);
}
