#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace shutdown_signal
{

struct Drain
{
  std::string name;
  std::function<void()> requestStop;
  std::function<bool()> drained;
};

template <typename T>
Drain drainOf(T& drain, std::string_view name)
{
  return {.name = std::string(name),
          .requestStop = [&drain] { drain.requestStop(); },
          .drained = [&drain] { return drain.drained(); }};
}

using QuitHook = std::function<void()>;

void onStop(Drain drain);

void onQuit(QuitHook hook);

void requestStop();

[[nodiscard]] bool drained();

}
