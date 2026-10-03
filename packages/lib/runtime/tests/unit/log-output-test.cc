#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <runtime/log-output.hxx>

#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
struct ChildOutput
{
  std::string text;
  bool arrived{false};
};

ChildOutput lineFromSilentChild(bool flushEachLine, std::chrono::milliseconds wait)
{
  std::array<int, 2> fds{};
  REQUIRE(::pipe(fds.data()) == 0);
  std::fflush(stdout);
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    ::dup2(fds[1], STDOUT_FILENO);
    ::close(fds[0]);
    ::close(fds[1]);
    if (flushEachLine)
      log_output::flushEachLine();
    std::fputs("argus log line\n", stdout);
    ::pause();
    ::_exit(0);
  }
  ::close(fds[1]);
  pollfd readable{.fd = fds[0], .events = POLLIN, .revents = 0};
  ChildOutput output;
  if (::poll(&readable, 1, static_cast<int>(wait.count())) == 1) {
    std::array<char, 64> buffer{};
    const ssize_t got = ::read(fds[0], buffer.data(), buffer.size());
    if (got > 0) {
      output.text.assign(buffer.data(), static_cast<size_t>(got));
      output.arrived = true;
    }
  }
  ::kill(child, SIGKILL);
  ::waitpid(child, nullptr, 0);
  ::close(fds[0]);
  return output;
}
}

TEST_CASE("a log line reaches a redirected stdout while the process keeps running")
{
  const ChildOutput output = lineFromSilentChild(true, std::chrono::seconds(2));
  CHECK(output.arrived);
  CHECK(output.text == "argus log line\n");
}

TEST_CASE("without it the same line stays in the block buffer")
{
  CHECK_FALSE(lineFromSilentChild(false, std::chrono::milliseconds(300)).arrived);
}
