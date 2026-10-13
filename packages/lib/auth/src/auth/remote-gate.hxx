#pragma once

#include <auth/remote-config.hxx>
#include <cstdint>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

enum class RemoteSurface : uint8_t
{
  Public,
  Sealed,
};

class RemoteGate
{
public:
  explicit RemoteGate(RemoteConfig config);
  RemoteGate(RemoteConfig config, RemoteSurface surface);

  [[nodiscard]] drogon::HttpResponsePtr check(const drogon::HttpRequestPtr& req,
                                              bool remote) const;
  [[nodiscard]] drogon::HttpResponsePtr checkRequest(
      const drogon::HttpRequestPtr& req) const;

private:
  [[nodiscard]] bool refuses(const drogon::HttpRequestPtr& req) const;

  RemoteConfig config_;
  RemoteSurface surface_{RemoteSurface::Public};
};

void installRemoteGate(RemoteGate gate);
