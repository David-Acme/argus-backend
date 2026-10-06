#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/guard/repositories/wind-down/wind-down-repository.hxx>

class GuardModuleWindDown
{
public:
  [[nodiscard]] drogon::Task<WindDownReport> run() const;

private:
  WindDownRepository repository_;
};
