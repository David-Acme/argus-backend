#include "remote-gate.hxx"

#include <gateway/gateway-errors.hxx>
#include <http/api-response.hxx>
#include <http/cors.hxx>

#include <string>

namespace
{
bool isRemoteRestrictedPath(const std::string& path)
{
  return path == "/pairing" || path == "/auth/register";
}
}

RemoteGate::RemoteGate(RemoteConfig config) : config_(config)
{
}

drogon::HttpResponsePtr RemoteGate::check(const drogon::HttpRequestPtr& req,
                                          bool remote)
{
  if (remote)
    req->getAttributes()->insert(RemoteGate::kRemoteContextKey, true);

  if (remote && !config_.enabled && isRemoteRestrictedPath(req->path())) {
    auto resp = ApiResponse::error(GatewayErrors::RemoteNotAllowed);
    Cors::apply(resp);
    return resp;
  }

  return nullptr;
}
