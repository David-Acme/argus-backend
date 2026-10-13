#include "remote-gate.hxx"

#include <auth/auth-errors.hxx>
#include <auth/jwt-filter.hxx>
#include <drogon/drogon.h>
#include <http/api-response.hxx>
#include <http/cors.hxx>

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <utility>

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

RemoteGate::RemoteGate(RemoteConfig config) : config_(std::move(config))
{
}

RemoteGate::RemoteGate(RemoteConfig config, RemoteSurface surface)
    : config_(std::move(config)), surface_(surface)
{
}

drogon::HttpResponsePtr RemoteGate::check(const drogon::HttpRequestPtr& req,
                                          bool remote) const
{
  if (!remote || !refuses(req))
    return nullptr;
  auto resp = ApiResponse::error(AuthErrors::RemoteNotAllowed);
  Cors::apply(resp);
  return resp;
}

drogon::HttpResponsePtr RemoteGate::checkRequest(
    const drogon::HttpRequestPtr& req) const
{
  return check(req, requestIsRemote(req, config_));
}

bool RemoteGate::refuses(const drogon::HttpRequestPtr& req) const
{
  if (surface_ == RemoteSurface::Sealed)
    return JwtFilter::extractToken(req).empty();
  return !config_.enabled && isRemoteRestrictedPath(req->path());
}

void installRemoteGate(RemoteGate gate)
{
  drogon::app().registerPreRoutingAdvice(
      [gate = std::move(gate)](const drogon::HttpRequestPtr& req,
                               drogon::AdviceCallback&& callback,
                               drogon::AdviceChainCallback&& chain) {
        if (auto refusal = gate.checkRequest(req)) {
          callback(refusal);
          return;
        }
        chain();
      });
}
