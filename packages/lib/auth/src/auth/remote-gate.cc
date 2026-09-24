#include "remote-gate.hxx"

#include <auth/auth-errors.hxx>
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
  if (remote && !config_.enabled && isRemoteRestrictedPath(req->path())) {
    auto resp = ApiResponse::error(AuthErrors::RemoteNotAllowed);
    Cors::apply(resp);
    return resp;
  }

  return nullptr;
}
