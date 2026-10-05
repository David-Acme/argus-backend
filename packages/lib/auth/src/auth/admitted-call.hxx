#pragma once

#include <auth/auth-errors.hxx>
#include <drogon/utils/coroutine.h>
#include <errors/response-exception.hxx>
#include <runtime/blocking-task.hxx>

#include <functional>
#include <utility>

namespace auth_admission
{

template <typename T>
drogon::Task<T> admitted(std::function<T()> call)
{
  try {
    co_return co_await BlockingTask<T>(std::move(call), BlockingLane::Light,
                                       BlockingAdmission::RejectWhenFull);
  }
  catch (const BlockingLaneFull&) {
    throw ResponseException(AuthErrors::AuthBusy);
  }
}

}
