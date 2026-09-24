#include "logical-routes.hxx"

#include <algorithm>
#include <drogon/HttpAppFramework.h>
#include <string_view>

namespace
{

constexpr std::string_view kHealthPath = "/health";

std::string leadingSegment(const std::string& path)
{
  const size_t start = path.find_first_not_of('/');
  if (start == std::string::npos)
    return {};
  const size_t end = path.find('/', start);
  if (end == std::string::npos)
    return path.substr(start);
  return path.substr(start, end - start);
}

}

std::vector<std::string>
logicalRoutesFrom(const std::vector<std::string>& patterns)
{
  std::vector<std::string> routes;
  for (const std::string& pattern : patterns) {
    if (pattern.empty() || pattern.front() != '/' || pattern == kHealthPath)
      continue;
    std::string segment = leadingSegment(pattern);
    if (segment.empty())
      continue;
    if (std::ranges::find(routes, segment) == routes.end())
      routes.push_back(std::move(segment));
  }
  std::ranges::sort(routes);
  return routes;
}

std::vector<std::string> logicalRoutes()
{
  std::vector<std::string> patterns;
  for (const auto& handlerInfo : drogon::app().getHandlersInfo())
    patterns.push_back(std::get<0>(handlerInfo));
  return logicalRoutesFrom(patterns);
}
