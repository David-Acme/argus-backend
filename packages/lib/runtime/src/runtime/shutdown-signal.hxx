#pragma once

#include <chrono>
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

inline constexpr std::chrono::milliseconds kDefaultDeadline{15000};

inline constexpr int kForcedExitCode = 130;

void setDeadline(std::chrono::milliseconds deadline);

[[nodiscard]] std::chrono::milliseconds deadline();

void onSignal();

[[nodiscard]] bool drained();

}
