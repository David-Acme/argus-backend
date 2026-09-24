#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <server/remote-config.hxx>
#include <string>

class RemoteGate
{
public:
  static inline const std::string kRemoteContextKey{"remote_ctx"};

  explicit RemoteGate(RemoteConfig config);

  drogon::HttpResponsePtr check(const drogon::HttpRequestPtr& req,
                                bool remote);

private:
  RemoteConfig config_;
};
