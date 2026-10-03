#include "log-output.hxx"

#include <cstdio>

namespace log_output
{
void flushEachLine()
{
  std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
}
}
