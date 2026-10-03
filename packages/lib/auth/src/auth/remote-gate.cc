#include "remote-gate.hxx"

#include <auth/auth-errors.hxx>
#include <http/api-response.hxx>
#include <http/cors.hxx>

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace
{
constexpr std::array<std::string_view, 2> kRemoteRestrictedPaths{"/pairing",
                                                                 "/auth/register"};

bool sameRoute(std::string_view path, std::string_view route)
{
  return std::ranges::equal(path, route, [](char left, char right) {
    return std::tolower(static_cast<unsigned char>(left)) ==
           std::tolower(static_cast<unsigned char>(right));
  });
}

bool isRemoteRestrictedPath(std::string_view path)
{
  return std::ranges::any_of(kRemoteRestrictedPaths, [path](std::string_view route) {
    return sameRoute(path, route);
  });
}
}

RemoteGate::RemoteGate(RemoteConfig config) : config_(config)
{
}

drogon::HttpResponsePtr RemoteGate::check(const drogon::HttpRequestPtr& req,
                                          bool remote)
{
  if (remote && !config_.enabled && isRemoteRestrictedPath(req->path())) {
    auto resp = ApiResponse::error(AuthErrors::RemoteNotAllowed);
    Cors::apply(resp);
    return resp;
  }

  return nullptr;
}
